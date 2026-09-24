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
static void on_proto_event(proto_event_t evt);

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

    proto_service_register_callback(on_proto_event);
    ESP_ERROR_CHECK(ws_service_init());
    audio_in_config_t input_config = audio_in_default_config();
    input_config.bus = s_bus;
    if (!audio_in_create(&input_config)) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(wake_word_init());
    ESP_LOGI(TAG, "WebSocket service ready");

    return ESP_OK;
}

/* ── 事件分发 ──────────────────────────────────────────────── */

static void on_proto_event(proto_event_t evt)
{
    switch (evt) {
        case PROTO_EVT_SPEECH_STARTED:
            ESP_LOGI(TAG, "[STATE] server: speech_started");
            audio_out_stop();
            break;
        case PROTO_EVT_SPEECH_STOPPED:
            ESP_LOGI(TAG, "[STATE] server: speech_stopped");
            /* Stop uplink before the server response starts playing.  Keeping
             * audio_in alive here makes it contend with the websocket receive
             * task for the client's internal mutex. */
            audio_in_stop(audio_in_get_handle());
            break;
        case PROTO_EVT_RESPONSE_DONE:
            ESP_LOGI(TAG, "[STATE] server: response_done");
            break;
        case PROTO_EVT_ERROR:
            ESP_LOGE(TAG, "[STATE] server: error event");
            break;
        default:
            ESP_LOGW(TAG, "[STATE] unknown proto event: %d", evt);
            break;
    }
}

/* ── 主循环状态机 ───────────────────────────────────────────── */

static void main_task(void *arg)
{
    (void)arg;
    app_state_t state = APP_STATE_INIT;
    app_state_t prev  = APP_STATE_INIT;

    app_state_set(APP_STATE_WAIT_WAKE);
    led_set_waiting_wake();
    char ssid[33], password[65];
    cfg_get_wifi_credential(ssid, sizeof(ssid), password, sizeof(password));
    wifi_service_start(ssid, password);

    while (1) {
        event_t queued;
        if (event_bus_receive(s_bus, &queued, pdMS_TO_TICKS(50)) == pdTRUE) {
            app_event_t event = {.id=queued.type, .data=queued.payload, .len=0};
            prev = state;
            state = app_state_handle_event(&event, prev);

            if (state != prev) {
                ESP_LOGI(TAG, "State transition: %s → %s",
                         app_state_name(prev), app_state_name(state));
                app_state_set(state);
            }

            switch (event.id) {
                case EV_WIFI_GOT_IP:
                    ws_service_connect();
                    ESP_LOGI(TAG, "Wi-Fi connected, LED → green solid");
                    led_set_wifi_ok();
                    break;
                case EV_WIFI_DISCONNECTED:
                case EV_WIFI_CONNECT_FAILED:
                    ESP_LOGW(TAG, "Wi-Fi lost, LED → red fast blink");
                    led_set_wifi_err();
                    break;
                case EV_WS_CONNECTED:
                    ESP_LOGI(TAG, "WS connected, LED → green slow blink");
                    led_set_ws_ok();
                    break;
                case EV_WS_DISCONNECTED:
                case EV_WS_ERROR:
                    wake_word_reset_barge_in();
                    ESP_LOGW(TAG, "WS disconnected, LED → red slow blink");
                    /* Do not let a new utterance upload while the client is
                     * reconnecting. It would race the transport and fail at
                     * esp_transport_write(). */
                    if (app_state_get() == APP_STATE_LISTENING ||
                        app_state_get() == APP_STATE_PLAYING) {
                        app_state_back_to_wait_wake(false);
                    }
                    led_set_ws_err();
                    break;
                case EV_AUDIO_WAKE_DETECTED:
                    ESP_LOGI(TAG, "Wake word detected, LED → green fast blink (recording)");
                    led_set_listening();
                    break;
                case EV_AUDIO_SPEAKING_STARTED:
                    ESP_LOGI(TAG, "TTS playback started, LED → green slow blink");
                    led_set_speaking();
                    break;
                case EV_AUDIO_SPEAKING_DONE:
                case EV_AUDIO_OUT_DONE:
                    wake_word_reset_barge_in();
                    ESP_LOGI(TAG, "Playback done, conversation remains active");
                    led_set_listening();
                    break;
                case EV_AUDIO_BARGE_IN:
                    ESP_LOGI(TAG, "Barge-in detected, stop playback");
                    audio_out_stop();
                    if (ws_service_cancel_response() != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to send response.cancel");
                    }
                    /* Start a fresh uplink only after the cancel frame has
                     * been queued, avoiding WebSocket lock contention. */
                    if (app_state_start_listening() != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to restart listening after barge-in");
                        app_state_set(APP_STATE_ERROR);
                    }
                    break;
                default:
                    break;
            }
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
