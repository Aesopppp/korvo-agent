#include "ws_service.h"
#include "audio_out.h"
#include "proto_service.h"
#include "response_flow.h"
#include "ws_rx_policy.h"
#include "reply_boundary.h"
#include "event_bus.h"
#include "cfg_service.h"
#include "board_config.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "ws_service";
static esp_websocket_client_handle_t client;
static atomic_int state = WS_STATE_DISCONNECTED;
static atomic_uint generation = 1;
static bool client_started;
static char uri[256];
static uint8_t *frame;
static size_t frame_size, received;
static int opcode;
static QueueHandle_t rx_queue;
static SemaphoreHandle_t playback_lock;
static TaskHandle_t rx_task;
static response_phase_t response_phase = RESPONSE_IDLE;
static uint32_t rx_drop_count;
static atomic_size_t rx_bytes;
static atomic_uint wire_turn, wire_done_turn;
static uint32_t playing_turn;
static uint32_t played_frames, discarded_frames;
static bool playback_eof;
static atomic_bool cancel_requested;
/* Scheduling hint only: never use this to discard audio. */
static atomic_bool cancel_waiting;
static atomic_int last_control_event;
static atomic_uint last_rx_ms;
typedef struct {
    uint8_t *data;
    size_t len;
    int opcode;
    uint32_t generation;
    uint32_t turn;
} rx_frame_t;

static void publish(app_event_id_t id, uint32_t gen)
{
    /* Integer generation tag, not a borrowed pointer. */
    if (gen == atomic_load(&generation))
        event_bus_publish(id, (void *)(uintptr_t)gen, 0);
}
uint32_t ws_service_generation(void) { return atomic_load(&generation); }
static bool current(const rx_frame_t *packet)
{
    return packet->generation == atomic_load(&generation) &&
           (state == WS_STATE_CONNECTED || state == WS_STATE_AUTHING);
}

static void log_server_error(const proto_msg_t *msg)
{
    /* Do not dump JSON: the server may echo auth/input frames. */
    char message[193] = {0}, secret[256];
    size_t len = msg->error_msg_len > 0 ? (size_t)msg->error_msg_len : 0;
    if (len >= sizeof(message)) len = sizeof(message) - 1;
    if (len) memcpy(message, msg->error_msg, len);
    bool sensitive = strstr(message, "sk-") || strstr(message, "token") ||
                     strstr(message, "password") || strstr(message, "Bearer");
    const cfg_key_t keys[] = { CFG_KEY_ACOS_TOKEN, CFG_KEY_WIFI_PASSWORD };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        cfg_service_get_str(keys[i], secret, sizeof(secret), "");
        if (secret[0] && strstr(message, secret)) sensitive = true;
    }
    for (size_t i = 0; i < len; ++i)
        if ((unsigned char)message[i] < 32) message[i] = ' ';
    /* Codes are bounded and allowed only as plain identifiers. */
    char code[65] = {0};
    int n = msg->error_code_text_len;
    if (n > 64) n = 64;
    for (int i = 0; i < n; ++i) {
        char c = msg->error_code_text[i];
        code[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_') ? c : '?';
    }
    ESP_LOGW(TAG, "Server error: code=%d/%s message=%s", msg->error_code, code,
             sensitive ? "[redacted]" : message);
}
static bool cancel_already_done(const proto_msg_t *msg)
{
    /* A cancel racing response.audio.done is reported by some gateways only
     * as this message. It means the response is already stopped. */
    return ws_cancel_inactive(msg->error_code_text, msg->error_code_text_len,
                              msg->error_msg, msg->error_msg_len);
}

static bool field_equals(const char *text, int len, const char *expected)
{
    return text && len >= 0 && (size_t)len == strlen(expected) &&
           memcmp(text, expected, len) == 0;
}

static bool terminal_boundary(const proto_msg_t *msg)
{
    return msg->evt == PROTO_EVT_RESPONSE_DONE ||
           (msg->evt == PROTO_EVT_RESPONSE_TERMINAL &&
            (field_equals(msg->status, msg->status_len, "completed") ||
             field_equals(msg->status, msg->status_len, "cancelled") ||
             field_equals(msg->status, msg->status_len, "canceled")));
}

/* Runs before queueing, without the playback mutex or any I2S/network wait.
 * The worker preserves FIFO ordering; these atomics only tell cancel whether
 * the server has already finished the response still playing locally. */
static void observe_control(const proto_msg_t *msg)
{
    atomic_store(&last_control_event, msg->evt);
    if (msg->evt == PROTO_EVT_SPEECH_STOPPED) {
        atomic_fetch_add(&wire_turn, 1);
        atomic_store(&wire_done_turn, 0);
    } else if (terminal_boundary(msg)) {
        atomic_store(&wire_done_turn, atomic_load(&wire_turn));
        ESP_LOGI(TAG, "RX response boundary: turn=%lu event=%d",
                 (unsigned long)atomic_load(&wire_turn), msg->evt);
    }
}

static void release_rx_frame(uint8_t *data, size_t len)
{
    if (!data) return;
    free(data);
    atomic_fetch_sub(&rx_bytes, len + 1);
}

static void drop_audio(void)
{
    if ((++rx_drop_count & 31u) == 1u)
        ESP_LOGW(TAG, "Audio backlog limit: dropped %lu incoming frames; connection retained",
                 (unsigned long)rx_drop_count);
}

static void enqueue_rx_frame(rx_frame_t *packet)
{
    if (xQueueSend(rx_queue, packet, 0) == pdTRUE) return;
    release_rx_frame(packet->data, packet->len);
    if (packet->opcode == 2) drop_audio();
    else {
        /* Audio leaves reserved slots. A control-only overflow is a real
         * protocol failure: never silently lose an auth/end boundary. */
        ESP_LOGE(TAG, "Control queue exhausted; recovering session");
        publish(EV_WS_ERROR, packet->generation);
    }
}

static void handle_frame(const rx_frame_t *packet)
{
    if (!current(packet)) return;
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    if (!current(packet)) { xSemaphoreGive(playback_lock); return; }
    if (packet->opcode == 2) {
        /* Decode/I2S on this worker, never on the client's locked RX task.
         * Cancel takes this same lock, so old audio cannot restart playback. */
        if (state == WS_STATE_CONNECTED &&
            !playback_eof &&
            reply_packet_current(playing_turn, packet->turn) &&
            response_accept_audio(&response_phase)) {
            audio_out_set_streaming(true);
            if (audio_out_play_opus(packet->data, packet->len) < 0)
                publish(EV_WS_SERVER_ERROR, packet->generation);
            else ++played_frames;
        } else if (reply_packet_current(playing_turn, packet->turn)) {
            ++discarded_frames;
        }
        xSemaphoreGive(playback_lock);
        return;
    }
    proto_msg_t msg;
    if (proto_parse_json((char *)packet->data, &msg) != PROTO_OK) {
        xSemaphoreGive(playback_lock); return;
    }
    switch (msg.evt) {
    case PROTO_EVT_PROXY_CONNECTED:
        if (state != WS_STATE_AUTHING) break;
        state = WS_STATE_CONNECTED;
        publish(EV_WS_AUTH_OK, packet->generation);
        break;
    case PROTO_EVT_SPEECH_STARTED:
        /* Speech detection does not acknowledge response.cancel. */
        publish(EV_WS_SPEECH_STARTED, packet->generation);
        break;
    case PROTO_EVT_SPEECH_STOPPED:
        if (response_phase == RESPONSE_CANCELLING) break;
        playing_turn = packet->turn;
        playback_eof = false;
        played_frames = discarded_frames = 0;
        response_expect(&response_phase);
        publish(EV_WS_SPEECH_STOPPED, packet->generation);
        break;
    case PROTO_EVT_RESPONSE_DONE:
    case PROTO_EVT_RESPONSE_TERMINAL: {
        if (!terminal_boundary(&msg) || !reply_packet_current(playing_turn, packet->turn)) break;
        if (response_phase == RESPONSE_PLAYING || response_phase == RESPONSE_WAITING) {
            if (!playback_eof) {
                playback_eof = true;
                audio_out_set_streaming(false);
                ESP_LOGI(TAG, "Response received; draining PCM tail turn=%lu", (unsigned long)playing_turn);
            }
            break;
        }
        response_end_t end = response_finish(&response_phase);
        if (end != RESPONSE_END_IGNORE)
            ESP_LOGI(TAG, "Playback boundary: turn=%lu reason=%s played=%lu discarded=%lu",
                     (unsigned long)playing_turn, end == RESPONSE_END_CANCELLED ? "cancelled" : "normal",
                     (unsigned long)played_frames, (unsigned long)discarded_frames);
        if (end == RESPONSE_END_CANCELLED) atomic_store(&cancel_requested, false);
        if (end != RESPONSE_END_IGNORE) audio_out_set_streaming(false);
        if (end == RESPONSE_END_CANCELLED) publish(EV_WS_CANCEL_DONE, packet->generation);
        else if (end == RESPONSE_END_NORMAL) publish(EV_WS_RESPONSE_DONE, packet->generation);
        break;
    }
    case PROTO_EVT_ERROR:
        if (cancel_already_done(&msg) && state == WS_STATE_CONNECTED) {
            ESP_LOGI(TAG, "Cancel already completed; keeping WebSocket connected");
            if (response_phase == RESPONSE_CANCELLING) {
                response_finish(&response_phase);
                atomic_store(&cancel_requested, false);
                publish(EV_WS_CANCEL_DONE, packet->generation);
            }
        } else {
            log_server_error(&msg);
            /* Business error != transport disconnect. Main explicitly
             * recovers the session instead of poisoning connection state. */
            response_phase = RESPONSE_DISCARD;
            publish(EV_WS_SERVER_ERROR, packet->generation);
        }
        break;
    default: break;
    }
    xSemaphoreGive(playback_lock);
}
static void finish_playback_if_drained(void)
{
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    if (playback_eof && state == WS_STATE_CONNECTED &&
        response_playback_finished(response_phase, playback_eof, audio_out_is_drained())) {
        response_finish(&response_phase);
        playback_eof = false;
        ESP_LOGI(TAG, "Playback drained: turn=%lu decoded=%lu discarded=%lu",
                 (unsigned long)playing_turn, (unsigned long)played_frames,
                 (unsigned long)discarded_frames);
        publish(EV_WS_RESPONSE_DONE, atomic_load(&generation));
    }
    xSemaphoreGive(playback_lock);
}
static void receive_task(void *arg)
{
    (void)arg;
    rx_frame_t packet;
    while (true) {
        if (xQueueReceive(rx_queue, &packet, pdMS_TO_TICKS(10)) == pdTRUE) {
            handle_frame(&packet);
            release_rx_frame(packet.data, packet.len);
        }
        finish_playback_if_drained();
        /* On two cores, let the waiting main task acquire playback_lock
         * between frames. Keep all queued audio until it commits cancel. */
        if (atomic_load(&cancel_waiting)) vTaskDelay(1);
    }
    vTaskDelete(NULL);
}

static void on_ws(void *arg, esp_event_base_t base, int32_t id, void *event)
{
    (void)arg; (void)base;
    uint32_t gen = atomic_load(&generation);
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        state = WS_STATE_AUTHING;
        publish(EV_WS_CONNECTED, gen);
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED) {
        state = WS_STATE_DISCONNECTED;
        release_rx_frame(frame, frame_size); frame = NULL; frame_size = received = 0;
        publish(EV_WS_DISCONNECTED, gen);
    } else if (id == WEBSOCKET_EVENT_ERROR) {
        state = WS_STATE_ERROR;
        esp_websocket_event_data_t *failure = event;
        if (failure) ESP_LOGW(TAG, "Transport detail: type=%d socket=%d tls=0x%x stack=%d close=%d",
                             failure->error_handle.error_type,
                             failure->error_handle.esp_transport_sock_errno,
                             failure->error_handle.esp_tls_last_esp_err,
                             failure->error_handle.esp_tls_stack_err, failure->close_status_code);
        publish(EV_WS_ERROR, gen);
    } else if (id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *data = event;
        if (data->op_code != 1 && data->op_code != 2) return;
        if (data->payload_len <= 0 || data->payload_len > 65536 ||
            data->data_len < 0 || data->payload_offset < 0) return;
        if (data->payload_offset == 0) {
            release_rx_frame(frame, frame_size); frame = NULL; received = 0;
            frame_size = data->payload_len; opcode = data->op_code;
            size_t used = atomic_load(&rx_bytes);
            if (opcode == 2 && !ws_rx_audio_fits(uxQueueMessagesWaiting(rx_queue),
                                                used, frame_size + 1)) {
                drop_audio();
                return;
            }
            if (used + frame_size + 1 <= WS_RX_AUDIO_BYTES + WS_RX_CONTROL_BYTES)
                frame = heap_caps_malloc(frame_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!frame) {
                if (opcode == 2) drop_audio();
                else publish(EV_WS_ERROR, gen);
                return;
            }
            atomic_fetch_add(&rx_bytes, frame_size + 1);
        }
        if (!frame || (size_t)data->payload_offset != received ||
            received + (size_t)data->data_len > frame_size) return;
        memcpy(frame + received, data->data_ptr, data->data_len);
        received += data->data_len;
        if (received == frame_size) {
            frame[received] = 0;
            atomic_store(&last_rx_ms, (uint32_t)(esp_timer_get_time() / 1000));
            rx_frame_t packet = {.data=frame, .len=received, .opcode=opcode, .generation=gen};
            if (opcode == 1) {
                proto_msg_t msg;
                proto_result_t result = proto_parse_json((char *)frame, &msg);
                if (result == PROTO_OK) observe_control(&msg);
                else if (atomic_load(&cancel_requested))
                    ESP_LOGW(TAG, "Unrecognized control while cancelling (parse=%d, bytes=%u)",
                             result, (unsigned)received);
            }
            packet.turn = atomic_load(&wire_turn);
            enqueue_rx_frame(&packet);
            frame = NULL; frame_size = received = 0;
        }
    }
}

esp_err_t ws_service_init(void)
{
    if (client) return ESP_OK;
    playback_lock = xSemaphoreCreateMutex();
    rx_queue = xQueueCreateWithCaps(WS_RX_FRAMES, sizeof(rx_frame_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!playback_lock || !rx_queue) goto fail;
    cfg_service_get_str(CFG_KEY_WS_URL, uri, sizeof(uri), ACOS_WS_URL);
    esp_websocket_client_config_t cfg = {
        .uri = uri, .disable_auto_reconnect = true, .network_timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    client = esp_websocket_client_init(&cfg);
    if (!client) goto fail;
    if (esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, on_ws, NULL) != ESP_OK) goto fail;
    if (xTaskCreatePinnedToCore(receive_task, "ws_rx_audio", 8192, NULL, 4, &rx_task, 1) != pdPASS) goto fail;
    ESP_LOGI(TAG, "RX buffer: %u frames, %u KiB Opus budget in PSRAM; %u control slots reserved",
             WS_RX_FRAMES, WS_RX_AUDIO_BYTES / 1024u, WS_RX_CONTROL_RESERVE);
    return ESP_OK;
fail:
    if (client) { esp_websocket_client_destroy(client); client = NULL; }
    if (rx_queue) { vQueueDeleteWithCaps(rx_queue); rx_queue = NULL; }
    if (playback_lock) { vSemaphoreDelete(playback_lock); playback_lock = NULL; }
    return ESP_ERR_NO_MEM;
}
esp_err_t ws_service_connect(void)
{
    if (!client) return ESP_ERR_INVALID_STATE;
    if (client_started) return ESP_OK;
    state = WS_STATE_CONNECTING;
    esp_err_t err = esp_websocket_client_start(client);
    if (err == ESP_OK) client_started = true;
    else state = WS_STATE_ERROR;
    return err;
}
esp_err_t ws_service_authenticate(void)
{
    if (!client || state != WS_STATE_AUTHING) return ESP_ERR_INVALID_STATE;
    char token[256], auth[512];
    cfg_service_get_str(CFG_KEY_ACOS_TOKEN, token, sizeof(token), "");
    if (!token[0]) { ESP_LOGE(TAG, "Configure ACOS token in NVS"); return ESP_ERR_INVALID_STATE; }
    size_t len = proto_build_auth_frame(auth, sizeof(auth), token);
    return len && esp_websocket_client_send_text(client, auth, len, pdMS_TO_TICKS(1000)) == (int)len
               ? ESP_OK : ESP_FAIL;
}
esp_err_t ws_service_disconnect(void)
{
    if (!client) return ESP_ERR_INVALID_STATE;
    state = WS_STATE_DISCONNECTED;
    if (client_started) {
        esp_err_t err = esp_websocket_client_stop(client);
        /* Component 1.8 returns ESP_FAIL if its task already stopped after
         * a transport failure (auto reconnect is disabled). That is success
         * for this idempotent main-task stop, not a reason to loop forever. */
        if (err != ESP_OK && err != ESP_FAIL) return err;
        client_started = false;
    }
    atomic_fetch_add(&generation, 1);
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    state = WS_STATE_DISCONNECTED;
    response_phase = RESPONSE_IDLE;
    playback_eof = false;
    playing_turn = 0;
    atomic_store(&wire_turn, 0);
    atomic_store(&wire_done_turn, 0);
    atomic_store(&cancel_requested, false);
    audio_out_stop();
    xSemaphoreGive(playback_lock);
    rx_frame_t packet;
    while (xQueueReceive(rx_queue, &packet, 0) == pdTRUE) release_rx_frame(packet.data, packet.len);
    release_rx_frame(frame, frame_size); frame = NULL; frame_size = received = 0;
    rx_drop_count = 0;
    return ESP_OK;
}
esp_err_t ws_service_deinit(void)
{
    if (!client) return ESP_OK;
    esp_err_t err = ws_service_disconnect();
    if (err != ESP_OK) return err;
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    if (rx_task) { vTaskDelete(rx_task); rx_task = NULL; }
    xSemaphoreGive(playback_lock);
    esp_websocket_client_destroy(client); client = NULL;
    vQueueDeleteWithCaps(rx_queue); rx_queue = NULL;
    vSemaphoreDelete(playback_lock); playback_lock = NULL;
    return ESP_OK;
}
ws_conn_state_t ws_service_get_state(void) { return state; }
esp_websocket_client_handle_t ws_service_get_client(void) { return client; }
esp_err_t ws_service_send_json(const char *text)
{
    if (!client || state != WS_STATE_CONNECTED) return ESP_ERR_INVALID_STATE;
    if (!text) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(text);
    return esp_websocket_client_send_text(client, text, len, pdMS_TO_TICKS(1000)) == (int)len ? ESP_OK : ESP_FAIL;
}
esp_err_t ws_service_send_opus_frame(const uint8_t *data, size_t len)
{
    if (!data || !len || len > 65536) return ESP_ERR_INVALID_ARG;
    if (state != WS_STATE_CONNECTED) return ESP_ERR_INVALID_STATE;
    size_t capacity = 4 * ((len + 2) / 3) + 1, written;
    char *encoded = malloc(capacity), *json = malloc(capacity + 256);
    if (!encoded || !json) { free(encoded); free(json); return ESP_ERR_NO_MEM; }
    esp_err_t err = ESP_FAIL;
    if (mbedtls_base64_encode((unsigned char *)encoded, capacity, &written, data, len) == 0) {
        encoded[written] = 0;
        if (proto_build_input_audio_append(json, capacity + 256, encoded, NULL)) err = ws_service_send_json(json);
    }
    free(encoded); free(json); return err;
}
esp_err_t ws_service_cancel_response(void)
{
    if (state != WS_STATE_CONNECTED) return ESP_ERR_INVALID_STATE;
    /* Do not skip any audio before holding this mutex and changing the phase.
     * Otherwise the worker can drain the tail and report a false NORMAL end
     * while the main task is still waiting to commit cancellation. */
    atomic_store(&cancel_waiting, true);
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    atomic_store(&cancel_waiting, false);
    if (state != WS_STATE_CONNECTED) {
        xSemaphoreGive(playback_lock);
        return ESP_ERR_INVALID_STATE;
    }
    bool pending = response_begin_cancel(&response_phase);
    const uint32_t cancelled_turn = playing_turn;
    atomic_store(&cancel_requested, pending);
    if (pending) ESP_LOGI(TAG, "Cancel committed: turn=%lu played=%lu queued=%u",
                         (unsigned long)playing_turn, (unsigned long)played_frames,
                         (unsigned)uxQueueMessagesWaiting(rx_queue));
    if (pending && audio_out_stop() != ESP_OK) {
        xSemaphoreGive(playback_lock);
        return ESP_FAIL;
    }
    bool local_only = pending && reply_boundary_received(playing_turn, atomic_load(&wire_done_turn));
    if (local_only) {
        response_finish(&response_phase);
        ESP_LOGI(TAG, "Barge-in: server already finished turn=%lu; discard local audio, keep connection",
                 (unsigned long)playing_turn);
        publish(EV_WS_CANCEL_DONE, atomic_load(&generation));
    }
    if (!pending || local_only) atomic_store(&cancel_requested, false);
    xSemaphoreGive(playback_lock);
    if (!pending) return ESP_ERR_NOT_FOUND;
    if (local_only) return ESP_OK;
    ESP_LOGI(TAG, "Barge-in: cancelling active server response turn=%lu", (unsigned long)cancelled_turn);
    return ws_service_send_json("{\"type\":\"response.cancel\"}");
}

void ws_service_log_cancel_wait(void)
{
    ESP_LOGW(TAG, "Cancel pending: wire_turn=%lu done_turn=%lu queue=%u bytes=%u last_control=%d rx_age_ms=%lu",
             (unsigned long)atomic_load(&wire_turn), (unsigned long)atomic_load(&wire_done_turn),
             (unsigned)uxQueueMessagesWaiting(rx_queue), (unsigned)atomic_load(&rx_bytes),
             atomic_load(&last_control_event),
             (unsigned long)((uint32_t)(esp_timer_get_time()/1000) - atomic_load(&last_rx_ms)));
}
