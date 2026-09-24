/**
 * @file wake_word.c
 * @brief 唤醒词检测实现 — esp-sr WakeNet
 *
 * 任务分工：
 *  feed_Task   (core 0) : I²S DMA → AFE feed
 *  detect_Task (core 1) : AFE fetch → WAKENET_DETECTED → 回调
 *
 * PCM 数据通过 ring buffer 共享给 audio_in：
 *  feed_Task 写 → ring buffer ← audio_in_task 读
 */

#include "wake_word.h"
#include "board_config.h"
#include "event_bus.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "audio_out.h"
#include "app_state.h"
#include "model_path.h"
#include "string.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include <stdatomic.h>
#include "barge_in_guard.h"
#include "esp_timer.h"

static const char *TAG = "wake_word";

/* ===========================================================================
 * 全局状态
 * ========================================================================= */

/** AFE 接口与实例 */
static const esp_afe_sr_iface_t *s_afe_iface = NULL;
static void                    *s_afe_data   = NULL;

/** ring buffer 句柄（feed_Task → audio_in） */
static RingbufHandle_t  s_ringbuf = NULL;

/** feed/detect 任务句柄 */
static TaskHandle_t     s_feed_task_h    = NULL;
static TaskHandle_t     s_detect_task_h  = NULL;

/** 运行标志 */
static volatile bool    s_task_running   = false;

/** 唤醒状态（detect_Task 写，app_main 读） */
static volatile wake_word_state_t s_wake_state = WAKE_STATE_IDLE;

/** 唤醒回调 */
static wake_word_detected_cb_t  s_wake_cb       = NULL;
static void                    *s_wake_cb_arg   = NULL;

/** 栈高水位 */
static uint32_t s_feed_stack_hiwat   = 0;
static uint32_t s_detect_stack_hiwat = 0;

/** fetch 帧大小（samples） */
static int s_afe_chunksize = 0;
static int s_fetch_chunksize = 0;
static barge_in_guard_t s_barge_guard;
static uint32_t s_barge_reject_log_ms;
static bool s_barge_pending = false;
static int64_t s_playback_started_ms = 0;
static bool s_playback_seen = false;
static SemaphoreHandle_t s_audio_buffer_lock;
static atomic_bool s_barge_reset_requested = false;

static int32_t afe_frame_energy(const int16_t *pcm, int samples)
{
    if (!pcm || samples <= 0) return 0;
    int64_t sum = 0;
    for (int i = 0; i < samples; ++i) {
        int32_t sample = pcm[i];
        sum += (int64_t)sample * sample;
    }
    return (int32_t)(sum / samples);
}

/* ===========================================================================
 * feed_Task — I²S DMA → AFE
 * ========================================================================= */

static void feed_Task(void *arg)
{
    (void)arg;
    int16_t *mic_buff = NULL;
    int16_t *ref_buff = NULL;
    int16_t *afe_buff = NULL;

    /* AFE 输入为 MMR：两路麦克风 + 一路数字播放参考。 */
    const int mic_channels = 2;
    const int afe_channels = 3;
    size_t mic_size = s_afe_chunksize * mic_channels * sizeof(int16_t);
    mic_buff = malloc(mic_size);
    ref_buff = malloc(s_afe_chunksize * sizeof(int16_t));
    afe_buff = malloc(s_afe_chunksize * afe_channels * sizeof(int16_t));
    if (mic_buff == NULL || ref_buff == NULL || afe_buff == NULL) {
        ESP_LOGE(TAG, "feed: audio buffer allocation failed");
        free(mic_buff);
        free(ref_buff);
        free(afe_buff);
        s_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "feed_Task started (chunksize=%d, input=MMR, core %d)",
             s_afe_chunksize, xPortGetCoreID());
    int64_t stats_at = esp_timer_get_time();
    uint32_t feed_max_us = 0, feed_overruns = 0, feed_count = 0;

    while (s_task_running) {
        /* 从 I²S DMA 读取 PCM */
        int ret = audio_out_read_microphones(mic_buff, mic_size);
        if (ret == 0) {
            audio_out_read_playback_reference(ref_buff, s_afe_chunksize);
            for (int i = 0; i < s_afe_chunksize; ++i) {
                afe_buff[3 * i]     = mic_buff[2 * i];
                afe_buff[3 * i + 1] = mic_buff[2 * i + 1];
                afe_buff[3 * i + 2] = ref_buff[i];
            }
            /* 喂给 AFE，最后一路必须是播放 reference。 */
            int64_t start = esp_timer_get_time();
            s_afe_iface->feed(s_afe_data, afe_buff);
            uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
            if (elapsed > feed_max_us) feed_max_us = elapsed;
            if (elapsed > (uint32_t)s_afe_chunksize * 1000000u / 16000u) ++feed_overruns;
            ++feed_count;
            if (esp_timer_get_time() - stats_at >= 5000000) {
                ESP_LOGI(TAG, "AFE stats: frames=%lu feed_max_us=%lu over_budget=%lu budget_us=%u",
                         (unsigned long)feed_count, (unsigned long)feed_max_us,
                         (unsigned long)feed_overruns, (unsigned)s_afe_chunksize * 1000000u / 16000u);
                stats_at = esp_timer_get_time();
                feed_count = feed_max_us = feed_overruns = 0;
            }
            /* A backlogged RX queue may never block. Yield one tick per 64 ms
             * input frame so CPU0 idle/system work is not starved indefinitely. */
            vTaskDelay(1);
        } else {
            /* 数据未就绪，短延时重试 */
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    free(mic_buff);
    free(ref_buff);
    free(afe_buff);
    ESP_LOGI(TAG, "feed_Task exited");
    vTaskDelete(NULL);
}

/* ===========================================================================
 * detect_Task — AFE fetch → 唤醒词检测
 * ========================================================================= */

static void detect_Task(void *arg)
{
    (void)arg;

    /* fetch 输出缓冲区（AFE 可能输出多通道，回合单通道给 audio_in） */
    int16_t *fetch_buff = malloc(s_afe_chunksize * sizeof(int16_t));
    if (fetch_buff == NULL) {
        ESP_LOGE(TAG, "detect: malloc failed");
        s_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "detect_Task started (core %d)", xPortGetCoreID());
    ESP_LOGI(TAG, "Playback barge-in guard: energy>=%d, sustained=480 ms, startup_guard=%u ms",
             BARGE_IN_MIN_ENERGY, BARGE_IN_GUARD_MS);

    while (s_task_running) {
        /* 从 AFE 取回处理结果 */
        afe_fetch_result_t *res = s_afe_iface->fetch(s_afe_data);

        if (res == NULL || res->ret_value == ESP_FAIL) {
            barge_in_guard_reset(&s_barge_guard);
            ESP_LOGW(TAG, "AFE fetch failed or EOF");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (atomic_exchange(&s_barge_reset_requested, false)) {
            barge_in_guard_reset(&s_barge_guard);
            s_barge_pending = false;
        }

        /* Only validated sustained speech can request cancellation. Weak VAD
         * residuals are logged without muting, dropping or pausing playback. */
        if (audio_out_is_streaming() && app_state_get() == APP_STATE_PLAYING) {
            if (!s_playback_seen) {
                s_playback_seen = true;
                s_playback_started_ms = esp_log_timestamp();
            }
            const uint32_t now = esp_log_timestamp();
            const uint32_t playback_age = now - (uint32_t)s_playback_started_ms;
            const int samples = res->data && res->data_size > 0
                                ? res->data_size / sizeof(int16_t) : 0;
            const int32_t processed_energy = afe_frame_energy(res->data, samples);
            bool confirmed = barge_in_guard_feed(&s_barge_guard,
                res->vad_state == VAD_SPEECH, processed_energy, samples, playback_age);
            if (!s_barge_pending && confirmed) {
                if (event_bus_publish(EV_AUDIO_BARGE_IN, NULL, 0) == pdTRUE) {
                    s_barge_pending = true;
                    ESP_LOGI(TAG, "Barge-in confirmed: energy=%ld min=%ld max=%ld sustained=480 ms",
                             (long)processed_energy, (long)s_barge_guard.min_energy,
                             (long)s_barge_guard.max_energy);
                }
            } else if (!s_barge_pending && res->vad_state == VAD_SPEECH &&
                       processed_energy >= BARGE_IN_DIAGNOSTIC_ENERGY &&
                       processed_energy < BARGE_IN_MIN_ENERGY &&
                       now - s_barge_reject_log_ms >= 2000) {
                s_barge_reject_log_ms = now;
                ESP_LOGI(TAG, "Weak playback VAD ignored: energy=%ld threshold=%d; playback continues",
                         (long)processed_energy, BARGE_IN_MIN_ENERGY);
            }
        } else {
            barge_in_guard_reset(&s_barge_guard);
            s_playback_seen = false;
            s_playback_started_ms = 0;
            /* Keep the latch until the cancelled response is acknowledged.
             * Old binary frames can arrive after audio_out_stop(); clearing
             * it here would retrigger barge-in for the same utterance. */
        }

        /* VAD state: 0=silence, 1=speech (via AFE 内置 VAD) */
        // ESP_LOGD(TAG, "vad=%d wake=%d", res->vad_state, res->wakeup_state);

        /* ── 唤醒词检测 ── */
        if (res->wakeup_state == WAKENET_DETECTED) {
            if (s_wake_state == WAKE_STATE_IDLE) {
                s_wake_state = WAKE_STATE_DETECTED;
                ESP_LOGI(TAG, ">>> WakeWord DETECTED! model=%d word=%d <<<",
                         res->wakenet_model_index, res->wake_word_index);

                /* 回调通知 app_main */
                if (s_wake_cb) {
                    s_wake_cb(res->wake_word_index, res->wakenet_model_index, s_wake_cb_arg);
                }

                /* 发布事件总线消息（与 EVENT_BUS_* 兼容宏对齐） */
                event_bus_publish(EV_AUDIO_WAKE_DETECTED, NULL, 0);
            }
            /* 检测到后不再重复触发，直到 reset() */
        }

        /* ── PCM 写入 ring buffer（供 audio_in 消费）───────────
         * AFE fetch 返回的 res->data 是 16kHz 单通道 int16_t PCM。
         * 直接写入 ring buffer，audio_in_task 读取时无需再转换。
         * 如果 AFE 返回多通道（res->channel > 1），取第 0 通道。
         */
        app_state_t current_state = app_state_get();
        if (s_ringbuf != NULL && res->data != NULL &&
            (current_state == APP_STATE_LISTENING || current_state == APP_STATE_PLAYING ||
             current_state == APP_STATE_CANCELLING)) {
            int16_t *pcm_to_buf = res->data;
            xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
            /* During playback retain only ~0.5 s before the detected barge-in.
             * While cancelling, retain up to ten seconds. Drop OLDEST, never
             * keep the first echo frames from several seconds ago. */
            size_t free_bytes = xRingbufferGetCurFreeSize(s_ringbuf);
            size_t retain = current_state == APP_STATE_PLAYING ? 16384 : WAKE_WORD_RB_TOTAL_SIZE;
            while (WAKE_WORD_RB_TOTAL_SIZE - free_bytes > retain ||
                   free_bytes < s_fetch_chunksize * sizeof(int16_t) + 16) {
                size_t bytes;
                void *old = xRingbufferReceive(s_ringbuf, &bytes, 0);
                if (!old) break;
                vRingbufferReturnItem(s_ringbuf, old);
                free_bytes = xRingbufferGetCurFreeSize(s_ringbuf);
            }
            (void)xRingbufferSend(s_ringbuf, pcm_to_buf, s_fetch_chunksize * sizeof(int16_t), 0);
            xSemaphoreGive(s_audio_buffer_lock);
        }
    }

    free(fetch_buff);
    ESP_LOGI(TAG, "detect_Task exited");
    vTaskDelete(NULL);
}

/* ===========================================================================
 * 公共 API
 * ========================================================================= */

esp_err_t wake_word_init(void)
{
    if (s_task_running) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WakeWord (esp-sr)...");

    /* ── 1. 板级初始化（I²S 总线、ES7210）────────────────── */
    esp_err_t err = audio_out_init(ES8311_I2C_ADDR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_board_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ── 2. 加载模型文件（从 flash partition "model"）─────── */
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init failed — no model partition? "
                      "run `idf.py menuconfig → ESP Speech Recognition → Select wake words`");
        return ESP_FAIL;
    }
    for (int i = 0; i < models->num; i++) {
        if (strstr(models->model_name[i], ESP_WN_PREFIX) != NULL) {
            ESP_LOGI(TAG, "WakeNet model found: %s", models->model_name[i]);
        }
    }

    /* ── 3. AFE 配置 ─────────────────────────────────────── */
    afe_config_t *afe_cfg = afe_config_init("MMR", models,
                                             AFE_TYPE_FD, AFE_MODE_LOW_COST);
    if (afe_cfg == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }

    /* Full-duplex AEC: the MMR last channel is the digital playback reference. */
    afe_cfg->aec_init = true;
    afe_cfg->aec_mode = AEC_MODE_FD_LOW_COST;
    afe_cfg->aec_filter_length = 4;
    afe_cfg->output_playback_channel = false;
    ESP_LOGI(TAG, "AFE configured for full-duplex AEC (MMR, filter=%d)",
             afe_cfg->aec_filter_length);

    /* 可选：打印/覆盖唤醒词模型名称 */
    if (afe_cfg->wakenet_model_name) {
        ESP_LOGI(TAG, "WakeNet model: %s", afe_cfg->wakenet_model_name);
    }
    if (afe_cfg->wakenet_model_name_2) {
        ESP_LOGI(TAG, "WakeNet model 2: %s", afe_cfg->wakenet_model_name_2);
    }

    /* ── 4. 创建 AFE handle ──────────────────────────────── */
    s_afe_iface = esp_afe_handle_from_config(afe_cfg);
    if (s_afe_iface == NULL) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(afe_cfg);
        return ESP_FAIL;
    }

    s_afe_data = s_afe_iface->create_from_config(afe_cfg);
    if (s_afe_data == NULL) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        afe_config_free(afe_cfg);
        return ESP_FAIL;
    }
    afe_config_free(afe_cfg);   /* 配置对象可释放，实例已拷贝 */

    /* ── 5. 获取帧大小 ───────────────────────────────────── */
    s_afe_chunksize = s_afe_iface->get_feed_chunksize(s_afe_data);
    s_fetch_chunksize = s_afe_iface->get_fetch_chunksize(s_afe_data);
    ESP_LOGI(TAG, "AFE chunksize=%d samples (%.1f ms)", s_afe_chunksize,
             s_afe_chunksize * 1000.0f / 16000.0f);

    /* ── 6. 创建 ring buffer ─────────────────────────────── */
    s_ringbuf = xRingbufferCreateWithCaps(WAKE_WORD_RB_TOTAL_SIZE, RINGBUF_TYPE_NOSPLIT,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_audio_buffer_lock = xSemaphoreCreateMutex();
    if (s_ringbuf == NULL || s_audio_buffer_lock == NULL) {
        ESP_LOGE(TAG, "xRingbufferCreate failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Ring buffer: %d items × %d samples = %d bytes",
             WAKE_WORD_RB_ITEM_COUNT, WAKE_WORD_RB_ITEM_SIZE,
             (int)WAKE_WORD_RB_TOTAL_SIZE);

    /* ── 7. 启动 feed_Task + detect_Task ────────────────── */
    s_task_running = true;
    BaseType_t t0 = xTaskCreatePinnedToCore(
        feed_Task, "ww_feed",
        WAKE_WORD_FEED_STACK, NULL, 5, &s_feed_task_h, 0);
    BaseType_t t1 = xTaskCreatePinnedToCore(
        detect_Task, "ww_detect",
        WAKE_WORD_DETECT_STACK, NULL, 5, &s_detect_task_h, 1);

    if (t0 != pdPASS || t1 != pdPASS) {
        ESP_LOGE(TAG, "task creation failed (feed=%d detect=%d)", t0, t1);
        s_task_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "WakeWord initialized — feed (core0) + detect (core1) running");
    return ESP_OK;
}

void wake_word_deinit(void)
{
    if (!s_task_running) return;

    ESP_LOGI(TAG, "Stopping WakeWord...");
    s_task_running = false;

    /* 等待任务退出 */
    if (s_feed_task_h) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        s_feed_task_h = NULL;
    }
    if (s_detect_task_h) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        s_detect_task_h = NULL;
    }

    /* 释放 AFE */
    if (s_afe_iface && s_afe_data) {
        s_afe_iface->destroy(s_afe_data);
        s_afe_data   = NULL;
        s_afe_iface  = NULL;
    }

    /* 释放 ring buffer */
    if (s_ringbuf) {
        vRingbufferDeleteWithCaps(s_ringbuf);
        s_ringbuf = NULL;
    }

    s_wake_state = WAKE_STATE_IDLE;
    ESP_LOGI(TAG, "WakeWord deinitialized");
}

RingbufHandle_t wake_word_get_ringbuf(void)
{
    return s_ringbuf;
}

int wake_word_get_chunksize(void)
{
    return s_fetch_chunksize > 0 ? s_fetch_chunksize : 512;
}

wake_word_state_t wake_word_get_state(void)
{
    return s_wake_state;
}

void wake_word_set_callback(wake_word_detected_cb_t cb, void *user_data)
{
    s_wake_cb     = cb;
    s_wake_cb_arg = user_data;
}

void wake_word_reset(void)
{
    if (s_wake_state != WAKE_STATE_IDLE) {
        ESP_LOGI(TAG, "wake_word_reset: DETECTED → IDLE");
        s_wake_state = WAKE_STATE_IDLE;
    }
}

void wake_word_reset_barge_in(void)
{
    atomic_store(&s_barge_reset_requested, true);
}

void wake_word_clear_audio_buffer(void)
{
    if (!s_ringbuf || !s_audio_buffer_lock) return;
    xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    size_t bytes;
    void *old;
    while ((old = xRingbufferReceive(s_ringbuf, &bytes, 0)) != NULL)
        vRingbufferReturnItem(s_ringbuf, old);
    xSemaphoreGive(s_audio_buffer_lock);
}

esp_err_t wake_word_start(void)
{
    if (s_task_running) {
        ESP_LOGD(TAG, "wake_word already running");
        return ESP_OK;
    }
    /* 尚未初始化 → 完整初始化（首次调用） */
    if (s_afe_iface == NULL) {
        return wake_word_init();
    }
    /* 已初始化但任务停止 → 重启任务 */
    s_task_running = true;
    BaseType_t t0 = xTaskCreatePinnedToCore(
        feed_Task, "ww_feed",
        WAKE_WORD_FEED_STACK, NULL, 5, &s_feed_task_h, 0);
    BaseType_t t1 = xTaskCreatePinnedToCore(
        detect_Task, "ww_detect",
        WAKE_WORD_DETECT_STACK, NULL, 5, &s_detect_task_h, 1);
    if (t0 != pdPASS || t1 != pdPASS) {
        ESP_LOGE(TAG, "task restart failed (feed=%d detect=%d)", t0, t1);
        s_task_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "WakeWord tasks restarted (AFE preserved)");
    return ESP_OK;
}

void wake_word_stop(void)
{
    if (!s_task_running) {
        ESP_LOGD(TAG, "wake_word already stopped");
        return;
    }
    ESP_LOGI(TAG, "Stopping WakeWord tasks (preserving AFE)...");
    s_task_running = false;

    /* 等待任务退出 */
    if (s_feed_task_h) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        s_feed_task_h = NULL;
    }
    if (s_detect_task_h) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        s_detect_task_h = NULL;
    }

    s_wake_state = WAKE_STATE_IDLE;
    ESP_LOGI(TAG, "WakeWord tasks stopped (AFE handle preserved for restart)");
}

uint32_t wake_word_get_feed_stack_hiwat(void)
{
    return s_feed_stack_hiwat;
}

uint32_t wake_word_get_detect_stack_hiwat(void)
{
    return s_detect_stack_hiwat;
}
