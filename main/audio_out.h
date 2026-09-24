/**
 * audio_out.h — ES8311 DAC 放音驱动
 *
 * ══════════════════════════════════════════════════════════════════════════════
 * 数据通路
 * ══════════════════════════════════════════════════════════════════════════════
 *   ACOS WS (Opus) → opus_codec_decode() → audio_out_write() → I2S TX → ES8311 DAC → PA → 扬声器
 *
 * 两条输出路径（编译时二选一）：
 *   a) esp_codec_dev 路径：esp_codec_dev_new() → esp_codec_dev_open() → esp_codec_dev_write()
 *   b) 直接 I2S 路径：i2s_new_channel(TX) → i2s_channel_write()
 *
 * Barge-In 打断：
 *   ACOS 服务端发 speech_started → ws_service 发 EV_AUDIO_BARGE_IN → audio_out_stop()
 *   内部通过 disable+enable I2S channel 清空 TX FIFO，状态切回 IDLE
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * 引脚定义（ESP32-S3-Korvo V1.2）
 * ─────────────────────────────────────────────────────────────────────────────
 *   I2S0 TX:  GPIO16=BCLK   GPIO45=WS    GPIO48=SDOUT   (MCLK 由 ESP32 PLL 生成)
 *   ES8311:   I2C addr=0x10 (默认), 不使用 GPIO1/2（ES8311 内部处理）
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * 依赖说明
 * ─────────────────────────────────────────────────────────────────────────────
 *   board_config.h        (I2C_PORT_NUM / GPIO 定义 / I2S 引脚)
 *   event_bus.h           (EVENT_BUS_AUDIO_OUT_STOPPED 事件定义)
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * 重要限制：所有 API 均线程安全（内部使用 FreeRTOS 互斥锁保护共享状态）── */

#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 编译期配置 ── */

/** I2S TX 日志标签（ESP-IDF v5.4 新 API）*/
#define AUDIO_OUT_I2S_TX_TAG        "AUDIO_OUT_I2S"

/** ES8311 I2C 从机地址（默认，A0 引脚接地）*/
#define ES8311_I2C_ADDR_DEFAULT     0x10

/** PCM 输出参数：ACOS 服务端统一使用 */
#define AUDIO_OUT_SAMPLE_RATE       16000
#define AUDIO_OUT_BIT_WIDTH         16
#define AUDIO_OUT_CHANNELS          1       /* mono，ES8311 内部 TDM 接收 stereo 并混合为单声道输出 */

/* ── 放音状态机 ── */

typedef enum {
    AUDIO_OUT_STATE_NONE      = 0,  /* 未初始化 */
    AUDIO_OUT_STATE_IDLE      = 1,  /* 空闲，可接受新数据 */
    AUDIO_OUT_STATE_PLAYING   = 2,  /* 正在播放 */
    AUDIO_OUT_STATE_ERROR     = 3,  /* 出错 */
} audio_out_state_t;

/* ── 事件类型（通过 event_bus 发布）── */

typedef enum {
    AUDIO_OUT_EVENT_INIT_OK       = 0,
    AUDIO_OUT_EVENT_INIT_FAIL     = 1,
    AUDIO_OUT_EVENT_UNDERFLOW    = 2,  /* TX DMA 缓冲区下溢（TX FIFO 欠载）*/
    AUDIO_OUT_EVENT_ERROR         = 3,
} audio_out_event_type_t;

/* ── 回调类型 ── */

typedef void (*audio_out_event_cb_t)(audio_out_event_type_t ev, void *user_data);

/* ── 公开 API ── */

/**
 * @brief 初始化 ES8311 DAC + I2S TX 硬件
 *
 * 需要 board_config.h 中定义的 I2C_PORT_NUM 已由 app_main 初始化完成。
 * @param i2c_addr   ES8311 I2C 从机地址，默认 ES8311_I2C_ADDR_DEFAULT
 * @return ESP_OK    初始化成功
 *         ESP_FAIL  ES8311 芯片通信失败
 *         ESP_ERR_* 其他错误
 */
esp_err_t audio_out_init(uint8_t i2c_addr);
esp_err_t audio_out_read_microphones(int16_t *pcm, size_t bytes);

/**
 * @brief Read the digital playback reference used by ESP-SR AEC.
 *
 * The reference is the same PCM stream that is sent to ES8311. When no audio
 * is playing, the function returns silence.
 */
void audio_out_read_playback_reference(int16_t *pcm, size_t samples);

/** Mark the beginning/end of a streamed server response. */
void audio_out_set_streaming(bool active);
bool audio_out_is_streaming(void);

/**
 * @brief 释放放音硬件资源（删除 I2S channel、移除 I2C 设备）
 */
esp_err_t audio_out_deinit(void);

/**
 * @brief 写入 PCM 数据（16-bit PCM，mono，16kHz）
 * @param pcm_data   PCM 样本缓冲区（int16_t*）
 * @param pcm_len    PCM 字节数（必须为偶数）
 *
 * @note 写入后 I2S TX DMA 自动推送，无需手动启动。
 *       IS TX FIFO 使用 double-buffer，DMA 一次喂 < 10ms 数据即可无缝播放。
 */
esp_err_t audio_out_write(const int16_t *pcm_data, size_t pcm_len);

/**
 * @brief 停止播放（Barge-In 打断）
 *
 * 清空 TX DMA FIFO，使 I2S 总线回到 IDLE 状态，允许 ACOS 新回复立刻播放。
 * 对端 ws_service 发 EV_AUDIO_BARGE_IN 时调用此函数。
 */
esp_err_t audio_out_stop(void);

/**
 * @brief 清空播放队列（DMA FIFO + esp_codec_dev 内部 buffer）。
 *
 * Barge-In 时与 audio_out_stop() 一起调用，确保上一轮回复的残留音频不会继续播放。
 */
esp_err_t audio_out_clear_queue(void);

/**
 * @brief 查询当前放音状态
 */
audio_out_state_t audio_out_get_state(void);

/**
 * @brief 解码 Opus 并播放（单步 API）
 *
 * 等效于 opus_codec_decode() 后调用 audio_out_write()。
 *
 * @param opus_data  Opus 压缩数据
 * @param opus_len  Opus 数据字节数
 * @return >=0  解码并写入的 PCM 样本数
 *         <0   解码失败，返回负值
 */
int audio_out_play_opus(const uint8_t *opus_data, size_t opus_len);

/**
 * @brief 注册放音事件回调（如 UNDERFLOW 通知）
 *
 * @param cb         回调函数，NULL 表示注销
 * @param user_data  透传给回调的用户数据
 */
void audio_out_set_event_cb(audio_out_event_cb_t cb, void *user_data);

/* ── 调试 API（CONFIG_KORVO_AGENT_DEBUG_AUDIO_OUT 开启时编译）── */

#if CONFIG_KORVO_AGENT_DEBUG_AUDIO_OUT
/** 打印 I2S TX FIFO / DMA 状态（供调试观察 underflow）*/
void audio_out_dump_stats(void);
#endif

/* ── event_bus 事件宏（向后兼容）── */

#ifndef EVENT_BUS_AUDIO_OUT_STATE
#define EVENT_BUS_AUDIO_OUT_STATE        EV_AUDIO_OUT_STATE
#endif

#ifndef EVENT_BUS_AUDIO_OUT_STOPPED
#define EVENT_BUS_AUDIO_OUT_STOPPED      EV_AUDIO_OUT_STOPPED
#endif

#endif /* AUDIO_OUT_H */
