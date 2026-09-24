/**
 * @file app_state.h
 * @brief 应用状态机 — 驱动 main_task 状态转移与 LED 指示
 */
#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "esp_err.h"
#include "event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_STATE_INIT        = 0,
    APP_STATE_WIFI_CONNECTING,
    APP_STATE_WIFI_CONNECTED,
    APP_STATE_WS_CONNECTING,
    APP_STATE_WS_CONNECTED,
    APP_STATE_WAIT_WAKE,
    APP_STATE_LISTENING,
    APP_STATE_PLAYING,
    APP_STATE_ERROR,
    APP_STATE_SLEEP,
    APP_STATE_CANCELLING,
} app_state_t;

const char *app_state_name(app_state_t state);
app_state_t app_state_get(void);
app_state_t app_state_set(app_state_t state);
app_state_t app_state_handle_event(const app_event_t *evt, app_state_t prev);
typedef struct {
    app_state_t state;
    SemaphoreHandle_t lock;
    event_bus_t bus;
    bool p1_enabled;
    esp_timer_handle_t silence_timer;
    int64_t silence_timeout_ms;
    struct { uint64_t total_speech_turns; } audio_stats;
} app_state_global_t;
esp_err_t app_state_init(event_bus_t bus);
app_state_global_t *app_state_get_global(void);
esp_err_t app_state_start_listening(void);
esp_err_t app_state_back_to_wait_wake(bool from_timer);
void app_state_reset_silence_timer(void);
void app_state_pause_silence_timer(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_STATE_H */
