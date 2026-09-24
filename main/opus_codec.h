/**
 * @file opus_codec.h
 * @brief Opus 音频编解码封装
 *
 * 依赖: espressif/esp_adf_misc (~1.0) → esp_opus
 * 编码: PCM 16kHz mono → Opus 60ms/frame, 24kbps
 * 解码: Opus 60ms/frame → PCM 16kHz mono
 */
#ifndef OPUS_CODEC_H
#define OPUS_CODEC_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * Opus 编码器
 * ========================================================================= */
typedef struct opus_encoder_impl *opus_enc_handle_t;

/**
 * @brief Opus 编码器配置
 */
typedef struct {
    int32_t  sample_rate;    /* 采样率 (默认 16000) */
    int      channels;       /* 声道数 (默认 1 = mono) */
    int      bitrate;        /* 比特率 (默认 24000 bps) */
    int      complexity;     /* 复杂度 0-10 (默认 4) */
    int      frame_ms;       /* 每帧时长 ms (默认 60) */
} opus_enc_config_t;

/**
 * @brief 创建 Opus 编码器
 * @param cfg 编码器配置，传 NULL 使用默认值
 * @return 句柄，失败返回 NULL
 */
opus_enc_handle_t opus_enc_create(const opus_enc_config_t *cfg);

/**
 * @brief 销毁编码器
 */
void opus_enc_destroy(opus_enc_handle_t h);

/**
 * @brief 编码一帧 PCM
 * @param h        编码器句柄
 * @param pcm      输入 PCM 缓冲 (16-bit, 16kHz, mono)
 * @param pcm_len  输入 PCM 字节数 (应为 frame_samples * 2)
 * @param opus_out 输出 Opus 缓冲
 * @param out_cap  输出缓冲容量 (字节)
 * @return 实际输出字节数，失败返回负值
 */
int opus_enc_encode(opus_enc_handle_t h,
                    const int16_t *pcm, int pcm_len,
                    uint8_t *opus_out, int out_cap);

/**
 * @brief 获取每帧 PCM 样本数 (由 bitrate/frame_ms 决定)
 */
int opus_enc_get_frame_samples(opus_enc_handle_t h);

/* ===========================================================================
 * Opus 解码器
 * ========================================================================= */
typedef struct opus_decoder_impl *opus_dec_handle_t;

/**
 * @brief Opus 解码器配置
 */
typedef struct {
    int32_t sample_rate;   /* 采样率 (默认 16000) */
    int     channels;      /* 声道数 (默认 1 = mono) */
} opus_dec_config_t;

/**
 * @brief 创建 Opus 解码器
 * @param cfg 配置，传 NULL 使用默认值
 * @return 句柄，失败返回 NULL
 */
opus_dec_handle_t opus_dec_create(const opus_dec_config_t *cfg);

/**
 * @brief 销毁解码器
 */
void opus_dec_destroy(opus_dec_handle_t h);

/**
 * @brief 解码 Opus 帧
 * @param h        解码器句柄
 * @param opus     输入 Opus 数据
 * @param opus_len 输入 Opus 字节数
 * @param pcm_out  输出 PCM 缓冲
 * @param pcm_cap  输出 PCM 缓冲容量 (字节)
 * @return 实际输出 PCM 样本数，失败返回负值
 */
int opus_dec_decode(opus_dec_handle_t h,
                    const uint8_t *opus, int opus_len,
                    int16_t *pcm_out, int pcm_cap);

/**
 * @brief 获取每帧解码输出样本数
 */
int opus_dec_get_frame_samples(opus_dec_handle_t h);

/* ===========================================================================
 * 工具函数
 * ========================================================================= */

/**
 * @brief 计算 Opus 编码输出最大长度
 * @param frame_ms 帧长 ms
 * @param sample_rate 采样率
 */
int opus_enc_max_output_size(int frame_ms, int sample_rate);

#ifdef __cplusplus
}
#endif

#endif /* OPUS_CODEC_H */
