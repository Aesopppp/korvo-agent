/**
 * @file app_state.c
 * @brief 应用状态机实现
 */

#include "app_state.h"
#include <string.h>
#include "esp_log.h"
#include "audio_in.h"
#include "wake_word.h"
#include "event_bus.h"
#include "ws_service.h"

static const char *TAG = "app_state";

app_state_t app_state_handle_event(const app_event_t *evt, app_state_t prev)
{
    if (!evt) return prev;
    switch (evt->id) {
    case EV_AUDIO_WAKE_DETECTED:
        return app_state_start_listening()==ESP_OK ? APP_STATE_LISTENING : APP_STATE_ERROR;
    case EV_AUDIO_SPEECH_START:
        app_state_reset_silence_timer(); return APP_STATE_LISTENING;
    case EV_AUDIO_OUT_START:
        return APP_STATE_PLAYING;
    case EV_AUDIO_BARGE_IN:
        /* The main task handles the barge-in side effects in a strict order:
         * stop playback, cancel the server response, then restart audio_in.
         * Do not start the upload task here or it can race response.cancel. */
        return APP_STATE_LISTENING;
    case EV_AUDIO_SILENCE_TIMEOUT:
        app_state_back_to_wait_wake(true); return APP_STATE_WAIT_WAKE;
    case EV_AUDIO_SPEAKING_STARTED: return APP_STATE_PLAYING;
    case EV_AUDIO_OUT_DONE:
    case EV_AUDIO_SPEAKING_DONE:
        /* Keep the conversation active after playback. The next utterance
         * can start without another wake word; the silence timer provides the
         * one-minute inactivity boundary back to WAIT_WAKE. */
        return app_state_start_listening() == ESP_OK
                   ? APP_STATE_LISTENING : APP_STATE_ERROR;
    case EV_SYSTEM_ERROR:
    case EV_AUDIO_INPUT_ERROR: return APP_STATE_ERROR;
    default: return prev;
    }
}

/* 默认 1 分钟静音超时（毫秒） */
#ifndef CONFIG_SILENCE_TIMEOUT_MS
#define CONFIG_SILENCE_TIMEOUT_MS 60000
#endif

/* 全局单例 */
static app_state_global_t s_global = {
    .state               = APP_STATE_INIT,
    .lock                = NULL,
    .bus                 = NULL,
    .p1_enabled          = true,   /* P1 默认使能，编译期可关闭 */
    .silence_timer       = NULL,
    .silence_timeout_ms  = CONFIG_SILENCE_TIMEOUT_MS,
};

/* 互斥锁保护 */
static SemaphoreHandle_t s_state_lock = NULL;

/* ---------------------------------------------------------------------------
 * 静音超时定时器回调
 *--------------------------------------------------------------------------*/
static void silence_timeout_callback(void *arg)
{
    ESP_LOGW(TAG, "Silence timeout: no speech for %lld ms, going back to WAIT_WAKE",
             (int64_t)(intptr_t)arg);
    /* 定时器回调中不能销毁自己，通过事件通知 app_main 处理 */
    event_bus_publish(EV_AUDIO_SILENCE_TIMEOUT, NULL, 0);
}

/* ---------------------------------------------------------------------------
 * 定时器辅助函数
 *--------------------------------------------------------------------------*/
static esp_err_t start_silence_timer(void)
{
    if (s_global.silence_timer != NULL) {
        /* 已有定时器，先 stop */
        esp_err_t stop_err = esp_timer_stop(s_global.silence_timer);
        if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) return stop_err;
    } else {
        /* 创建单次定时器（不在 WAIT_WAKE 时销毁以简化生命周期管理，
         * 复用同一个 handle，WAIT_WAKE 时 stop 即可） */
        const esp_timer_create_args_t args = {
            .callback       = silence_timeout_callback,
            .arg            = (void *)(intptr_t)s_global.silence_timeout_ms,
            .dispatch_method = ESP_TIMER_TASK,  /* 回调在独立任务中执行 */
            .name           = "silence_timer",
        };
        esp_err_t err = esp_timer_create(&args, &s_global.silence_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "create silence_timer failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    esp_err_t err = esp_timer_start_once(s_global.silence_timer,
                                          s_global.silence_timeout_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start silence_timer failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t stop_silence_timer(void)
{
    if (s_global.silence_timer == NULL) {
        return ESP_OK;
    }
    esp_err_t err = esp_timer_stop(s_global.silence_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "stop silence_timer failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* ---------------------------------------------------------------------------
 * 公开 API
 *--------------------------------------------------------------------------*/
esp_err_t app_state_init(event_bus_t bus)
{
    s_state_lock = xSemaphoreCreateMutex();
    if (s_state_lock == NULL) {
        ESP_LOGE(TAG, "create lock failed");
        return ESP_FAIL;
    }

    memset(&s_global, 0, sizeof(s_global));
    s_global.lock                = s_state_lock;
    s_global.bus                 = bus;
    s_global.state               = APP_STATE_INIT;
    s_global.silence_timeout_ms  = CONFIG_SILENCE_TIMEOUT_MS;

    ESP_LOGI(TAG, "app_state initialized (silence_timeout=%lld ms)",
             s_global.silence_timeout_ms);
    return ESP_OK;
}

app_state_global_t *app_state_get_global(void)
{
    return &s_global;
}

app_state_t app_state_get(void)
{
    app_state_t s;
    if (s_state_lock && xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        s = s_global.state;
        xSemaphoreGive(s_state_lock);
    } else {
        s = APP_STATE_INIT;
    }
    return s;
}

app_state_t app_state_set(app_state_t new_state)
{
    app_state_t old_state = APP_STATE_INIT;
    if (s_state_lock && xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        old_state = s_global.state;
        if (old_state != new_state) {
            ESP_LOGI(TAG, "state: %s -> %s",
                     app_state_name(old_state),
                     app_state_name(new_state));
            s_global.state = new_state;
        }
        xSemaphoreGive(s_state_lock);
    }
    return old_state;
}

const char *app_state_name(app_state_t s)
{
    switch (s) {
        case APP_STATE_INIT:            return "INIT";
        case APP_STATE_WIFI_CONNECTING: return "WIFI_CONNECTING";
        case APP_STATE_WIFI_CONNECTED:  return "WIFI_CONNECTED";
        case APP_STATE_WS_CONNECTING:   return "WS_CONNECTING";
        case APP_STATE_WS_CONNECTED:    return "WS_CONNECTED";
        case APP_STATE_WAIT_WAKE:       return "WAIT_WAKE";
        case APP_STATE_LISTENING:       return "LISTENING";
        case APP_STATE_PLAYING:         return "PLAYING";
        case APP_STATE_ERROR:           return "ERROR";
        case APP_STATE_SLEEP:           return "SLEEP";
        case APP_STATE_CANCELLING:      return "CANCELLING";
        default:                        return "UNKNOWN";
    }
}

esp_err_t app_state_start_listening(void)
{
    if (ws_service_get_state() != WS_STATE_CONNECTED) {
        ESP_LOGW(TAG, "Cannot listen: WebSocket authentication is not ready");
        return ESP_ERR_INVALID_STATE;
    }
    app_state_set(APP_STATE_LISTENING);
    /*
     * 持续对话模式下，服务端发送 speech_stopped 后 app_main 会停止
     * audio_in，等待云端播放回答。response_done 之后状态仍然是
     * LISTENING，因此这里不能因为状态相同而直接返回，必须确认并重新
     * 启动 audio_in。audio_in_start_from_ringbuf() 本身对已运行任务是幂等的。
     */
    audio_in_handle_t ai = audio_in_get_handle();
    esp_err_t err = audio_in_start_from_ringbuf(ai, wake_word_get_ringbuf());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_in_start_from_ringbuf failed: %s", esp_err_to_name(err));
        app_state_set(APP_STATE_ERROR);
        return err;
    }

    /* 首次进入或回答结束后都重新开始一分钟静音计时 */
    err = start_silence_timer();
    if (err != ESP_OK) {
        /* 定时器失败不阻塞主流程，但后续无超时保护 */
        ESP_LOGW(TAG, "silence timer start failed, no timeout protection");
    }

    s_global.audio_stats.total_speech_turns++;
    ESP_LOGI(TAG, "LISTENING started (turn #%llu)", s_global.audio_stats.total_speech_turns);
    return ESP_OK;
}

esp_err_t app_state_back_to_wait_wake(bool from_timer)
{
    app_state_t prev = app_state_get();

    /* 停止静音定时器（若由定时器回调触发则跳过 stop） */
    if (!from_timer) {
        stop_silence_timer();
    }

    /* 停止 audio_in 收音 */
    audio_in_handle_t ai = audio_in_get_handle();
    esp_err_t err = audio_in_stop(ai);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_in_stop failed: %s", esp_err_to_name(err));
    }

    app_state_set(APP_STATE_WAIT_WAKE);
    wake_word_reset();
    ESP_LOGI(TAG, "Back to WAIT_WAKE, waiting for wake word...");
    return ESP_OK;
}

void app_state_reset_silence_timer(void)
{
    if (s_global.state != APP_STATE_LISTENING) {
        return;
    }
    if (s_global.silence_timer == NULL) {
        return;
    }
    /* 重启定时器（自动覆盖之前的启动） */
    esp_err_t err = esp_timer_restart(s_global.silence_timer,
                                       s_global.silence_timeout_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reset silence_timer failed: %s", esp_err_to_name(err));
    }
}

void app_state_pause_silence_timer(void)
{
    stop_silence_timer();
}
