/* Korvo-2 V3.1 audio: bounded mono PCM FIFO -> stereo I2S -> ES8311.
 * One WS/decode producer serializes begin/write/end/stop. The output task owns
 * pacing; its mutex protects FIFO state and hardware writes. Microphone reads
 * use the shared RX clock, which remains running during cancellation. */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2s_std.h"
#define AUDIO_OUT_SAMPLE_RATE 16000
#define AUDIO_OUT_BIT_WIDTH 16
#define AUDIO_OUT_CHANNELS 1
#define ES8311_I2C_ADDR_DEFAULT 0x10

typedef enum {
    AUDIO_OUT_STATE_NONE=0, AUDIO_OUT_STATE_IDLE, AUDIO_OUT_STATE_PLAYING, AUDIO_OUT_STATE_ERROR
} audio_out_state_t;
typedef enum {
    AUDIO_OUT_EVENT_INIT_OK=0, AUDIO_OUT_EVENT_INIT_FAIL, AUDIO_OUT_EVENT_UNDERFLOW, AUDIO_OUT_EVENT_ERROR
} audio_out_event_type_t;
typedef void (*audio_out_event_cb_t)(audio_out_event_type_t ev, void *user_data);

esp_err_t audio_out_init(uint8_t i2c_addr);
esp_err_t audio_out_deinit(void);
esp_err_t audio_out_read_microphones(int16_t *pcm, size_t bytes);
/* Software reference follows submitted I2S blocks, including silence. Compensates
 * the configured DMA horizon; physical/acoustic alignment needs board testing. */
void audio_out_read_playback_reference(int16_t *pcm, size_t samples);
/* true begins an idempotent stream; false seals it and drains a short final reply
 * even if its PCM never reached the prefill threshold. */
void audio_out_set_streaming(bool active);
/* Audible playback (including a temporary starvation); used by barge-in guard. */
bool audio_out_is_streaming(void);
bool audio_out_is_drained(void);
/* Enqueue 16 kHz mono signed 16-bit PCM. Length is bytes. Bounded backpressure. */
esp_err_t audio_out_write(const int16_t *pcm_data, size_t pcm_len);
/* Decode into a reusable buffer, then enqueue. Return decoded sample count. */
int audio_out_play_opus(const uint8_t *opus_data, size_t opus_len);
/* Discard queued PCM, ramp to silence and overwrite the TX DMA horizon. */
esp_err_t audio_out_stop(void);
esp_err_t audio_out_clear_queue(void);
audio_out_state_t audio_out_get_state(void);
void audio_out_set_event_cb(audio_out_event_cb_t cb, void *arg);
void audio_out_dump_stats(void);
#endif
