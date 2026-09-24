/**
 * @file app_main.c
 * @brief Korvo Agent 入口 — FreeRTOS 任务编排与事件分发中枢
 *
 * ══════════════════════════════════════════════════════════════════════════════
 * 系统架构
 * ══════════════════════════════════════════════════════════════════════════════
 * P0 收音链路：MIC(ES7210) → I²S DMA → AFE(WakeNet VAD) → RingBuffer
 *              → audio_in(VAD切句) → Opus编码 → WS JSON帧 → ACOS云端
 *
 * P1放音链路：ACOS WS(Opus) → Opus解码 → ES8311 DAC → NS4150功放 → 扬声器
 *              支持 Barge-In 打断
 *
 * 依赖：ESP-IDF v5.4.4 + esp-sr v2.5.3
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_event.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "board_config.h"
#include "event_bus.h"
#include "app_state.h"
#include "wifi_service.h"
#include "cfg_service.h"
#include "log_service.h"
#include "led_service.h"
#include "audio_out.h"
#include "ws_service.h"
#include "proto_service.h"
#include "audio_in.h"
#include "wake_word.h"

static const char *TAG = "app_main";

/* ── 内部任务 & 队列 ────────────────────────────────────────── */

static event_bus_t s_bus;
static led_service_t s_led;


#define MAIN_TASK_PRIORITY   5
#define MAIN_TASK_STACK     8192

/* ── 系统初始化 ─────────────────────────────────────────────── */

static esp_err_t system_init(void)
{
    ESP_LOGI(TAG, "=== Korvo Agent starting (ESP-IDF v5.4.4) ===");

#if CONFIG_SPIRAM
    if (esp_psram_get_size() > 0) {
        ESP_LOGI(TAG, "PSRAM size: %zu MB", esp_psram_get_size() / 1024 / 1024);
    }
#endif

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(cfg_service_init());
    ESP_ERROR_CHECK(app_state_init(s_bus));
    /* wifi_service_init owns netif and default event loop creation. */
    ESP_ERROR_CHECK(wifi_service_init());
    ESP_LOGI(TAG, "Wi-Fi service ready");

    s_led = led_service_init();
    if (s_led == NULL) {
        ESP_LOGE(TAG, "LED service init failed");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(audio_out_init(ES8311_I2C_ADDR));
    ESP_LOGI(TAG, "Audio output ready");


    ESP_ERROR_CHECK(ws_service_init());
    audio_in_config_t input_config = audio_in_default_config();
    input_config.bus = s_bus;
    if (!audio_in_create(&input_config)) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(wake_word_init());
    ESP_LOGI(TAG, "WebSocket service ready");

    return ESP_OK;
}

/* ── 事件分发 ──────────────────────────────────────────────── */

/* Only this task changes session state or starts/stops the WS client. */
static bool conversation_active;
static int64_t retry_at_ms, auth_deadline_ms, cancel_deadline_ms, response_deadline_ms;
static int64_t resume_deadline_ms;
static int64_t cancel_probe_ms;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void recover_session(const char *reason)
{
    ESP_LOGW(TAG, "Session recovery: %s", reason);
    if (conversation_active && !resume_deadline_ms) resume_deadline_ms = now_ms() + 60000;
    app_state_back_to_wait_wake(false);
    wake_word_reset_barge_in();
    esp_err_t err = ws_service_disconnect();
    if (err != ESP_OK) ESP_LOGW(TAG, "WS stop pending: %s", esp_err_to_name(err));
    wake_word_clear_audio_buffer();
    auth_deadline_ms = cancel_deadline_ms = response_deadline_ms = 0;
    cancel_probe_ms = 0;
    retry_at_ms = now_ms() + 5000;
    ESP_LOGW(TAG, "Reconnect scheduled in 5000 ms; resume_conversation=%d (no voice required)",
             conversation_active);
    led_set_ws_err();
}

static void resume_listening(bool keep_buffered_speech, const char *reason)
{
    if (!keep_buffered_speech) wake_word_clear_audio_buffer();
    cancel_deadline_ms = response_deadline_ms = 0;
    cancel_probe_ms = 0;
    if (app_state_start_listening() != ESP_OK) {
        recover_session("could not start audio upload");
        return;
    }
    wake_word_reset_barge_in();
    resume_deadline_ms = 0;
    led_set_listening();
    ESP_LOGI(TAG, "Conversation listening; source=%s; ready for the next question", reason);
}

static bool tagged_ws_event(event_type_t id)
{
    return id == EV_WS_CONNECTED || id == EV_WS_AUTH_OK || id == EV_WS_ERROR ||
           id == EV_WS_DISCONNECTED || id == EV_WS_SERVER_ERROR || id == EV_WS_CANCEL_DONE ||
           id == EV_WS_SPEECH_STARTED || id == EV_WS_SPEECH_STOPPED ||
           id == EV_WS_RESPONSE_DONE || id == EV_AUDIO_UPLOAD_FAILED;
}

static void main_task(void *arg)
{
    (void)arg;
    conversation_active = false;
    resume_deadline_ms = 0;
    ESP_LOGI(TAG, "Boot session=%08lx reset_reason=%d build=pcm-playback-v1; fresh wake required",
             (unsigned long)esp_random(), esp_reset_reason());
    app_state_set(APP_STATE_WAIT_WAKE);
    led_set_wifi_err();
    char ssid[33], password[65];
    cfg_get_wifi_credential(ssid, sizeof(ssid), password, sizeof(password));
    wifi_service_start(ssid, password);

    while (1) {
        event_t event;
        if (event_bus_receive(s_bus, &event, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (tagged_ws_event(event.type) &&
                (uint32_t)(uintptr_t)event.payload != ws_service_generation()) continue;
            app_state_t current_state = app_state_get();
            switch (event.type) {
            case EV_WIFI_GOT_IP:
                ESP_LOGI(TAG, "Wi-Fi ready; scheduling server connection");
                if (ws_service_get_state() != WS_STATE_CONNECTED && !auth_deadline_ms)
                    retry_at_ms = now_ms();
                if (ws_service_get_state() != WS_STATE_CONNECTED) led_set_ws_err();
                break;
            case EV_WIFI_DISCONNECTED:
            case EV_WIFI_CONNECT_FAILED:
                recover_session("Wi-Fi unavailable");
                led_set_wifi_err();
                break;
            case EV_WS_CONNECTED:
                ESP_LOGI(TAG, "WebSocket transport connected; authenticating");
                auth_deadline_ms = now_ms() + 10000;
                if (ws_service_authenticate() != ESP_OK) recover_session("auth send failed");
                break;
            case EV_WS_AUTH_OK:
                auth_deadline_ms = retry_at_ms = 0;
                ESP_LOGI(TAG, "WebSocket authenticated; audio upload ready");
                if (conversation_active && resume_deadline_ms && now_ms() < resume_deadline_ms)
                    resume_listening(false, "reconnect of previously awakened conversation");
                else {
                    conversation_active = false;
                    resume_deadline_ms = 0;
                    wake_word_reset();
                    led_set_waiting_wake();
                    ESP_LOGI(TAG, "Online; WAIT_WAKE; microphone upload off until wake word");
                }
                break;
            case EV_WS_DISCONNECTED:
                recover_session("WebSocket transport disconnected");
                break;
            case EV_WS_ERROR:
                recover_session("WebSocket transport/receive failure");
                break;
            case EV_WS_SERVER_ERROR:
                recover_session("server protocol error (see ws_service error code)");
                break;
            case EV_AUDIO_UPLOAD_FAILED:
                recover_session("audio upload failed");
                break;
            case EV_AUDIO_WAKE_DETECTED:
                if (ws_service_get_state() != WS_STATE_CONNECTED) {
                    ESP_LOGW(TAG, "Wake ignored: server not authenticated yet");
                    wake_word_reset();
                    break;
                }
                if (!conversation_active) {
                    conversation_active = true;
                    resume_listening(false, "wake word");
                }
                break;
            case EV_AUDIO_SPEECH_START:
            case EV_WS_SPEECH_STARTED:
                if (conversation_active && current_state == APP_STATE_LISTENING)
                    app_state_reset_silence_timer();
                break;
            case EV_WS_SPEECH_STOPPED:
                if (!conversation_active || current_state != APP_STATE_LISTENING) break;
                ESP_LOGI(TAG, "Server speech_stopped; waiting for reply");
                app_state_pause_silence_timer();
                if (audio_in_stop(audio_in_get_handle()) != ESP_OK) {
                    recover_session("audio stop timeout");
                    break;
                }
                app_state_set(APP_STATE_PLAYING);
                response_deadline_ms = now_ms() + 45000;
                break;
            case EV_AUDIO_OUT_START:
                if (conversation_active && current_state == APP_STATE_PLAYING &&
                    ws_service_get_state() == WS_STATE_CONNECTED) {
                    response_deadline_ms = now_ms() + 180000;
                    led_set_speaking();
                }
                break;
            case EV_AUDIO_BARGE_IN: {
                if (!conversation_active || current_state != APP_STATE_PLAYING ||
                    ws_service_get_state() != WS_STATE_CONNECTED) break;
                esp_err_t err = ws_service_cancel_response();
                if (err == ESP_ERR_NOT_FOUND) {
                    /* Reply finished just before this queued VAD event. Its
                     * normal completion event will resume listening once. */
                    wake_word_reset_barge_in();
                    break;
                }
                if (err != ESP_OK) { recover_session("response.cancel send failed"); break; }
                app_state_set(APP_STATE_CANCELLING);
                app_state_pause_silence_timer();
                response_deadline_ms = 0;
                cancel_probe_ms = now_ms() + 3000;
                cancel_deadline_ms = now_ms() + 8000;
                ESP_LOGI(TAG, "Playback stopped; waiting for cancelled reply boundary");
                break;
            }
            case EV_WS_CANCEL_DONE:
                if (conversation_active && current_state == APP_STATE_CANCELLING) {
                    ESP_LOGI(TAG, "Cancel acknowledged; resume buffered microphone audio");
                    resume_listening(true, "barge-in completed");
                }
                break;
            case EV_WS_RESPONSE_DONE:
                if (conversation_active && current_state == APP_STATE_PLAYING) {
                    ESP_LOGI(TAG, "Reply complete; continue conversation");
                    resume_listening(false, "reply completed");
                }
                break;
            case EV_AUDIO_SILENCE_TIMEOUT:
                /* Ignore an already queued timeout from before playback. */
                if (current_state != APP_STATE_LISTENING) break;
                if (esp_timer_is_active(app_state_get_global()->silence_timer)) break;
                conversation_active = false;
                resume_deadline_ms = 0;
                app_state_back_to_wait_wake(true);
                wake_word_clear_audio_buffer();
                led_set_waiting_wake();
                break;
            case EV_AUDIO_INPUT_ERROR:
                recover_session("audio input error");
                break;
            default: break;
            }
        }
        int64_t now = now_ms();
        if (cancel_probe_ms && now >= cancel_probe_ms) {
            cancel_probe_ms = 0;
            ESP_LOGW(TAG, "Cancel confirmation delayed; keeping connection while waiting (limit 8 s)");
            ws_service_log_cancel_wait();
        }
        if (resume_deadline_ms && now >= resume_deadline_ms) {
            conversation_active = false;
            resume_deadline_ms = 0;
            wake_word_reset();
        }
        if (cancel_deadline_ms && now >= cancel_deadline_ms) {
            ws_service_log_cancel_wait();
            recover_session("cancel acknowledgement timed out");
        }
        else if (response_deadline_ms && now >= response_deadline_ms)
            recover_session("server reply timed out");
        else if (auth_deadline_ms && now >= auth_deadline_ms)
            recover_session("connection/authentication timed out");
        if (retry_at_ms && now >= retry_at_ms && wifi_service_get_state() == WIFI_STATE_GOT_IP) {
            ESP_LOGI(TAG, "Automatic WebSocket connection attempt; no voice trigger");
            /* Stop is idempotent, and retries a prior failed stop before start. */
            esp_err_t err = ws_service_disconnect();
            if (err == ESP_OK) err = ws_service_connect();
            if (err == ESP_OK) {
                retry_at_ms = 0;
                auth_deadline_ms = now_ms() + 20000;
            } else retry_at_ms = now_ms() + 5000;
        }
        led_service_poll(s_led, esp_log_timestamp());
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
/* ── 应用入口 ───────────────────────────────────────────────── */

void app_main(void)
{
    s_bus = event_bus_create(32);
    if (s_bus == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        abort();
    }

    if (system_init() != ESP_OK) {
        ESP_LOGE(TAG, "system_init failed, rebooting in 3s");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    xTaskCreatePinnedToCore(main_task, "main", MAIN_TASK_STACK,
                            NULL, MAIN_TASK_PRIORITY, NULL, 0);

    ESP_LOGI(TAG, "app_main done, main_task running on core 0");
}
