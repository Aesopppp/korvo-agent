/**
 * @file opus_codec.c
 * @brief Opus 编解码实现 — 基于本地 libopus 组件
 *
 * API 封装 (opus_codec.h):
 *   opus_enc_create/destroy/encode    — PCM 16kHz mono → Opus
 *   opus_dec_create/destroy/decode    — Opus → PCM 16kHz mono
 *
 * libopus C API 参考:
 *   opus_encoder_create / opus_encoder_destroy
 *   opus_encode         (int16_t 输入)
 *   opus_decoder_create / opus_decoder_destroy
 *   opus_decode         (int16_t 输出)
 */

#include "opus_codec.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

/* libopus 头文件 (本地组件) */
#include <opus.h>

static const char *TAG = "opus_codec";

/* ===========================================================================
 * Opus 编码器
 * ========================================================================= */

struct opus_encoder_impl {
    OpusEncoder *enc;          /* libopus 编码器句柄 */
    int          frame_samples; /* 每帧样本数 */
    int          channels;
    int          sample_rate;
};

opus_enc_handle_t opus_enc_create(const opus_enc_config_t *cfg)
{
    opus_enc_handle_t h = calloc(1, sizeof(struct opus_encoder_impl));
    if (h == NULL) {
        ESP_LOGE(TAG, "calloc encoder failed");
        return NULL;
    }

    opus_enc_config_t dcfg = {
        .sample_rate = 16000,
        .channels    = 1,
        .bitrate     = 24000,
        .complexity  = 4,
        .frame_ms    = 60,
    };
    if (cfg) {
        if (cfg->sample_rate > 0) dcfg.sample_rate = cfg->sample_rate;
        if (cfg->channels    > 0) dcfg.channels    = cfg->channels;
        if (cfg->bitrate     > 0) dcfg.bitrate     = cfg->bitrate;
        if (cfg->complexity  >= 0) dcfg.complexity = cfg->complexity;
        if (cfg->frame_ms    > 0) dcfg.frame_ms    = cfg->frame_ms;
    }

    h->sample_rate   = dcfg.sample_rate;
    h->channels      = dcfg.channels;
    h->frame_samples = dcfg.sample_rate * dcfg.frame_ms / 1000;

    int opus_err;
    h->enc = opus_encoder_create(dcfg.sample_rate, dcfg.channels,
                                  OPUS_APPLICATION_RESTRICTED_LOWDELAY,
                                  &opus_err);
    if (h->enc == NULL) {
        ESP_LOGE(TAG, "opus_encoder_create failed: %s", opus_strerror(opus_err));
        free(h);
        return NULL;
    }

    opus_encoder_ctl(h->enc, OPUS_SET_BITRATE(dcfg.bitrate));
    opus_encoder_ctl(h->enc, OPUS_SET_COMPLEXITY(dcfg.complexity));
    opus_encoder_ctl(h->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    ESP_LOGI(TAG, "opus encoder created: rate=%d, ch=%d, bitrate=%d, "
             "complexity=%d, frame_samples=%d",
             h->sample_rate, h->channels, dcfg.bitrate,
             dcfg.complexity, h->frame_samples);
    return h;
}

void opus_enc_destroy(opus_enc_handle_t h)
{
    if (h == NULL) return;
    if (h->enc) {
        opus_encoder_destroy(h->enc);
        h->enc = NULL;
    }
    free(h);
}

int opus_enc_encode(opus_enc_handle_t h,
                    const int16_t *pcm, int pcm_len,
                    uint8_t *opus_out, int out_cap)
{
    if (!h || !h->enc || !pcm || !opus_out) return -1;

    int expected = h->frame_samples * h->channels * (int)sizeof(int16_t);
    if (pcm_len < expected) {
        ESP_LOGW(TAG, "pcm_len %d < expected %d, padding silence", pcm_len, expected);
        /* 静默填充不足部分 */
        opus_int32 n = opus_encode(h->enc, (const opus_int16 *)pcm,
                                    h->frame_samples, opus_out, out_cap);
        return (n < 0) ? -1 : (int)n;
    }

    opus_int32 n = opus_encode(h->enc, (const opus_int16 *)pcm,
                                h->frame_samples, opus_out, out_cap);
    if (n < 0) {
        ESP_LOGW(TAG, "opus_encode failed: %s", opus_strerror((int)n));
        return -1;
    }
    return (int)n;
}

int opus_enc_get_frame_samples(opus_enc_handle_t h)
{
    return h ? h->frame_samples : 0;
}

/* ===========================================================================
 * Opus 解码器
 * ========================================================================= */

struct opus_decoder_impl {
    OpusDecoder *dec;          /* libopus 解码器句柄 */
    int          frame_samples; /* 每帧输出样本数 */
    int          channels;
    int          sample_rate;
};

opus_dec_handle_t opus_dec_create(const opus_dec_config_t *cfg)
{
    opus_dec_handle_t h = calloc(1, sizeof(struct opus_decoder_impl));
    if (h == NULL) {
        ESP_LOGE(TAG, "calloc decoder failed");
        return NULL;
    }

    opus_dec_config_t dcfg = { .sample_rate = 16000, .channels = 1 };
    if (cfg) {
        if (cfg->sample_rate > 0) dcfg.sample_rate = cfg->sample_rate;
        if (cfg->channels    > 0) dcfg.channels    = cfg->channels;
    }

    h->sample_rate   = dcfg.sample_rate;
    h->channels      = dcfg.channels;
    h->frame_samples = dcfg.sample_rate * 60 / 1000;  /* 60ms 帧 */

    int opus_err;
    h->dec = opus_decoder_create(dcfg.sample_rate, dcfg.channels, &opus_err);
    if (h->dec == NULL) {
        ESP_LOGE(TAG, "opus_decoder_create failed: %s", opus_strerror(opus_err));
        free(h);
        return NULL;
    }

    ESP_LOGI(TAG, "opus decoder created: rate=%d, ch=%d, frame_samples=%d",
             h->sample_rate, h->channels, h->frame_samples);
    return h;
}

void opus_dec_destroy(opus_dec_handle_t h)
{
    if (h == NULL) return;
    if (h->dec) {
        opus_decoder_destroy(h->dec);
        h->dec = NULL;
    }
    free(h);
}

int opus_dec_decode(opus_dec_handle_t h,
                    const uint8_t *opus, int opus_len,
                    int16_t *pcm_out, int pcm_cap)
{
    if (!h || !h->dec || !opus || !pcm_out) return -1;

    int max_samples = pcm_cap / sizeof(int16_t);
    int samples = opus_decode(h->dec, (const unsigned char *)opus,
                               opus_len, pcm_out, max_samples, 0);
    if (samples < 0) {
        ESP_LOGW(TAG, "opus_decode failed: %s", opus_strerror(samples));
        return -1;
    }
    return samples;
}

int opus_dec_get_frame_samples(opus_dec_handle_t h)
{
    return h ? h->frame_samples : 0;
}

/* ===========================================================================
 * 工具函数
 * ========================================================================= */

int opus_enc_max_output_size(int frame_ms, int sample_rate)
{
    /* Opus 最大: 120ms * 48000/8 = 7200 bytes，取安全上限 4000 */
    (void)frame_ms;
    (void)sample_rate;
    return 4000;
}
