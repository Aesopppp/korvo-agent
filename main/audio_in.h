/**
 * @file audio_in.h
 * @brief 音频采集服务 (P0 收音链路核心)
 *
 * 链路: ES7210 ADC → I2S0 DMA → audio_in 任务 → VAD 检测
 *       → PCM 缓冲 → (回调或队列输出)
 *
 * 采样: 16 kHz, 16-bit, mono (ES7210 单麦模式)
 * VAD:  能量阈值 + 静音超时
 */
#ifndef AUDIO_IN_H
#define AUDIO_IN_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 类型定义
 * ========================================================================= */

/** 音频 PCM 缓冲描述符 */
typedef struct {
    int16_t  *data;       /**< PCM 数据缓冲 (由 audio_in 分配/回收) */
    size_t    size;       /**< data 字节数 */
    uint32_t  timestamp_ms; /**< 采样结束时间戳 */
} pcm_buf_t;

/** VAD 状态 */
typedef enum {
    VAD_STATE_IDLE = 0,
    VAD_STATE_SPEECH,
    VAD_STATE_SILENCE,
} vad_state_t;

/** audio_in 配置 */
typedef struct {
    int             sample_rate;      /* 采样率 (默认 16000) */
    int             frame_ms;         /* 每帧时长 ms (默认 30) */
    int             mic_gain_db;      /* ES7210 MIC 增益 dB (默认 30) */
    int             vad_threshold;   /* VAD 能量阈值 (默认 500) */
    int             silence_timeout_ms; /* 静音超时 ms (默认 800) */
    int             min_utterance_ms;  /* 最小有效语音 ms (默认 300) */
    event_bus_t     bus;             /* 事件总线 (可 NULL) */
} audio_in_config_t;

/** audio_in 句柄 */
typedef struct audio_in_impl *audio_in_handle_t;

/* ===========================================================================
 * 回调 API (替代事件总线)
 * ========================================================================= */

/**
 * @brief PCM 数据就绪回调 (每帧触发)
 * @param user_data  用户上下文
 * @param pcm       PCM 缓冲 (只读，勿 free)
 * @param pcm_len   PCM 字节数
 * @param timestamp_ms 采样结束时间戳
 */
typedef void (*audio_in_pcm_cb_t)(void *user_data,
                                   const int16_t *pcm, size_t pcm_len,
                                   uint32_t timestamp_ms);

/**
 * @brief 一句话结束回调 (VAD 检测到句子边界)
 * @param user_data 用户上下文
 * @param pcm       完整句子 PCM (调用者负责 copy/free)
 * @param pcm_len   PCM 字节数
 */
typedef void (*audio_in_utterance_cb_t)(void *user_data,
                                         const int16_t *pcm, size_t pcm_len);

/* ===========================================================================
 * API
 * ========================================================================= */

/**
 * @brief 获取默认配置
 */
audio_in_config_t audio_in_default_config(void);

/**
 * @brief 创建音频采集服务
 * @param cfg  配置
 * @return 句柄，失败返回 NULL
 */
audio_in_handle_t audio_in_create(const audio_in_config_t *cfg);

/**
 * @brief 销毁服务 (会停止任务)
 */
void audio_in_destroy(audio_in_handle_t h);

/**
 * @brief 从 AFE ring buffer 启动采集（替代独立 I²S 初始化）
 *
 * 调用时机：wake_word 检测到唤醒词、WS 已连接后。
 * audio_in 自身不再初始化 I²S，直接从 wake_word 的 ring buffer 读取 PCM。
 *
 * @param h        audio_in 句柄（由 audio_in_create 创建）
 * @param ringbuf  wake_word_get_ringbuf() 返回的 ring buffer 句柄
 * @return ESP_OK 成功
 */
esp_err_t audio_in_start_from_ringbuf(audio_in_handle_t h, RingbufHandle_t ringbuf);

/**
 * @brief 停止采集（从 ring buffer 停止）
 */
esp_err_t audio_in_stop(audio_in_handle_t h);

/**
 * @brief 注册 PCM 帧回调
 */
esp_err_t audio_in_register_pcm_cb(audio_in_handle_t h,
                                   audio_in_pcm_cb_t cb, void *user_data);

/**
 * @brief 注册句子结束回调
 */
esp_err_t audio_in_register_utterance_cb(audio_in_handle_t h,
                                         audio_in_utterance_cb_t cb,
                                         void *user_data);

/**
 * @brief 获取当前 VAD 状态
 */
vad_state_t audio_in_get_vad_state(audio_in_handle_t h);

/**
 * @brief 获取采样任务栈高水位 (调试)
 */
uint32_t audio_in_get_stack_hiwat(audio_in_handle_t h);

/**
 * @brief 获取全局 audio_in 句柄（供 app_state.c 启动时使用）
 */
audio_in_handle_t audio_in_get_handle(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_IN_H */
