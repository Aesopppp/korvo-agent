/* Korvo-2: shared full-duplex I2S with ES8311 and ES7210. */
#include "audio_out.h"
#include "board_config.h"
#include "event_bus.h"
#include "opus_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

#define PLAYBACK_REF_CAP_SAMPLES 16384
static i2s_chan_handle_t tx, rx;
static esp_codec_dev_handle_t speaker, microphone;
static const audio_codec_data_if_t *data_if;
static const audio_codec_ctrl_if_t *dac_ctrl, *adc_ctrl;
static const audio_codec_if_t *dac_if, *adc_if;
static const audio_codec_gpio_if_t *gpio_if;
static opus_dec_handle_t decoder;
static SemaphoreHandle_t output_lock;
static audio_out_state_t state;
static audio_out_event_cb_t callback;
static void *callback_arg;
static int16_t *playback_ref;
static size_t playback_ref_read;
static size_t playback_ref_write;
static size_t playback_ref_count;
static SemaphoreHandle_t playback_ref_lock;
static volatile bool playback_streaming;

static void playback_ref_clear(void)
{
    if (!playback_ref_lock) return;
    xSemaphoreTake(playback_ref_lock, portMAX_DELAY);
    playback_ref_read = playback_ref_write = playback_ref_count = 0;
    xSemaphoreGive(playback_ref_lock);
}

static void playback_ref_push(const int16_t *pcm, size_t samples)
{
    if (!playback_ref || !playback_ref_lock || !pcm) return;
    xSemaphoreTake(playback_ref_lock, portMAX_DELAY);
    for (size_t i = 0; i < samples; ++i) {
        /* Keep the newest reference if the producer briefly outruns AFE. */
        if (playback_ref_count == PLAYBACK_REF_CAP_SAMPLES) {
            playback_ref_read = (playback_ref_read + 1) % PLAYBACK_REF_CAP_SAMPLES;
            playback_ref_count--;
        }
        playback_ref[playback_ref_write] = pcm[i];
        playback_ref_write = (playback_ref_write + 1) % PLAYBACK_REF_CAP_SAMPLES;
        playback_ref_count++;
    }
    xSemaphoreGive(playback_ref_lock);
}

esp_err_t audio_out_init(uint8_t i2c_addr)
{
    if (speaker) return ESP_OK;
    i2c_master_bus_handle_t bus;
    esp_err_t err=i2c_master_get_bus_handle(BSP_I2C_PORT,&bus);
    if (err!=ESP_OK) return err;
    i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(BSP_I2S_PORT,I2S_ROLE_MASTER);
    channel.auto_clear=true;
    if ((err=i2s_new_channel(&channel,&tx,&rx))!=ESP_OK) return err;
    i2s_std_config_t config={
        .clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_OUT_SAMPLE_RATE),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),
        .gpio_cfg={.mclk=BSP_I2S_MCLK_PIN,.bclk=BSP_I2S_BCLK_PIN,
                   .ws=BSP_I2S_WS_PIN,.dout=BSP_I2S_DOUT_PIN,.din=BSP_I2S_DIN_PIN},
    };
    if ((err=i2s_channel_init_std_mode(tx,&config))!=ESP_OK) goto fail;
    if ((err=i2s_channel_init_std_mode(rx,&config))!=ESP_OK) goto fail;
    audio_codec_i2s_cfg_t data_cfg={.port=BSP_I2S_PORT,.tx_handle=tx,.rx_handle=rx};
    data_if=audio_codec_new_i2s_data(&data_cfg);
    /* codec_dev expects the 8-bit address, board_config stores 7-bit addresses. */
    audio_codec_i2c_cfg_t ctrl_cfg={.port=BSP_I2C_PORT,.addr=i2c_addr<<1,.bus_handle=bus};
    dac_ctrl=audio_codec_new_i2c_ctrl(&ctrl_cfg);
    ctrl_cfg.addr=ES7210_I2C_ADDR<<1;
    adc_ctrl=audio_codec_new_i2c_ctrl(&ctrl_cfg);
    gpio_if=audio_codec_new_gpio();
    if (!data_if||!dac_ctrl||!adc_ctrl||!gpio_if) {err=ESP_ERR_NO_MEM;goto fail;}
    es8311_codec_cfg_t dac_cfg={.ctrl_if=dac_ctrl,.gpio_if=gpio_if,
        .codec_mode=ESP_CODEC_DEV_WORK_MODE_DAC,.pa_pin=BSP_PA_EN_PIN,.use_mclk=true};
    es7210_codec_cfg_t adc_cfg={.ctrl_if=adc_ctrl,.mic_selected=ES7210_SEL_MIC1|ES7210_SEL_MIC2};
    dac_if=es8311_codec_new(&dac_cfg); adc_if=es7210_codec_new(&adc_cfg);
    if (!dac_if||!adc_if) {err=ESP_ERR_NO_MEM;goto fail;}
    esp_codec_dev_cfg_t dev_cfg={.dev_type=ESP_CODEC_DEV_TYPE_OUT,.codec_if=dac_if,.data_if=data_if};
    speaker=esp_codec_dev_new(&dev_cfg);
    dev_cfg.dev_type=ESP_CODEC_DEV_TYPE_IN; dev_cfg.codec_if=adc_if;
    microphone=esp_codec_dev_new(&dev_cfg);
    if (!speaker||!microphone) {err=ESP_ERR_NO_MEM;goto fail;}
    esp_codec_dev_sample_info_t sample={.sample_rate=AUDIO_OUT_SAMPLE_RATE,.channel=2,.bits_per_sample=16};
    if ((err=esp_codec_dev_open(speaker,&sample))!=ESP_OK) goto fail;
    if ((err=esp_codec_dev_open(microphone,&sample))!=ESP_OK) goto fail;
    if ((err=esp_codec_dev_set_out_vol(speaker,80))!=ESP_OK) goto fail;
    if ((err=esp_codec_dev_set_in_gain(microphone,30.0f))!=ESP_OK) goto fail;
    output_lock=xSemaphoreCreateMutex(); decoder=opus_dec_create(NULL);
    playback_ref_lock = xSemaphoreCreateMutex();
    playback_ref = calloc(PLAYBACK_REF_CAP_SAMPLES, sizeof(int16_t));
    if (!output_lock||!decoder||!playback_ref_lock||!playback_ref) {err=ESP_ERR_NO_MEM;goto fail;}
    playback_streaming = false;
    state=AUDIO_OUT_STATE_IDLE;
    event_bus_publish(EV_AUDIO_OUT_READY,NULL,0);
    if (callback) callback(AUDIO_OUT_EVENT_INIT_OK,callback_arg);
    return ESP_OK;
fail:
    audio_out_deinit(); state=AUDIO_OUT_STATE_ERROR; return err;
}

esp_err_t audio_out_deinit(void)
{
    if (decoder) {opus_dec_destroy(decoder);decoder=NULL;}
    if (speaker) {esp_codec_dev_close(speaker);esp_codec_dev_delete(speaker);speaker=NULL;}
    if (microphone) {esp_codec_dev_close(microphone);esp_codec_dev_delete(microphone);microphone=NULL;}
    if (dac_if) {audio_codec_delete_codec_if(dac_if);dac_if=NULL;}
    if (adc_if) {audio_codec_delete_codec_if(adc_if);adc_if=NULL;}
    if (dac_ctrl) {audio_codec_delete_ctrl_if(dac_ctrl);dac_ctrl=NULL;}
    if (adc_ctrl) {audio_codec_delete_ctrl_if(adc_ctrl);adc_ctrl=NULL;}
    if (gpio_if) {audio_codec_delete_gpio_if(gpio_if);gpio_if=NULL;}
    if (data_if) {audio_codec_delete_data_if(data_if);data_if=NULL;}
    if (tx) {i2s_channel_disable(tx);i2s_del_channel(tx);tx=NULL;}
    if (rx) {i2s_channel_disable(rx);i2s_del_channel(rx);rx=NULL;}
    if (output_lock) {vSemaphoreDelete(output_lock);output_lock=NULL;}
    if (playback_ref_lock) {vSemaphoreDelete(playback_ref_lock);playback_ref_lock=NULL;}
    free(playback_ref); playback_ref=NULL;
    playback_ref_read = playback_ref_write = playback_ref_count = 0;
    playback_streaming = false;
    state=AUDIO_OUT_STATE_NONE; return ESP_OK;
}

/* Input length is bytes of mono PCM; hardware uses two slots. */
esp_err_t audio_out_write(const int16_t *pcm_data,size_t pcm_len)
{
    if (!speaker||!output_lock) return ESP_ERR_INVALID_STATE;
    if (!pcm_data||pcm_len%sizeof(int16_t)) return ESP_ERR_INVALID_ARG;
    int16_t stereo[256]; esp_err_t err=ESP_OK;
    xSemaphoreTake(output_lock,portMAX_DELAY); state=AUDIO_OUT_STATE_PLAYING;
    playback_ref_push(pcm_data, pcm_len / sizeof(int16_t));
    for (size_t pos=0;pos<pcm_len/2&&err==ESP_OK;) {
        size_t count=pcm_len/2-pos; if (count>128) count=128;
        for (size_t i=0;i<count;i++) stereo[2*i]=stereo[2*i+1]=pcm_data[pos+i];
        err=esp_codec_dev_write(speaker,stereo,count*4);pos+=count;
    }
    state = err == ESP_OK
                ? (playback_streaming ? AUDIO_OUT_STATE_PLAYING : AUDIO_OUT_STATE_IDLE)
                : AUDIO_OUT_STATE_ERROR;
    xSemaphoreGive(output_lock);return err;
}
esp_err_t audio_out_read_microphones(int16_t *pcm,size_t bytes)
{return microphone?esp_codec_dev_read(microphone,pcm,bytes):ESP_ERR_INVALID_STATE;}

void audio_out_read_playback_reference(int16_t *pcm, size_t samples)
{
    if (!pcm) return;
    if (!playback_ref || !playback_ref_lock) {
        memset(pcm, 0, samples * sizeof(int16_t));
        return;
    }
    xSemaphoreTake(playback_ref_lock, portMAX_DELAY);
    for (size_t i = 0; i < samples; ++i) {
        if (playback_ref_count > 0) {
            pcm[i] = playback_ref[playback_ref_read];
            playback_ref_read = (playback_ref_read + 1) % PLAYBACK_REF_CAP_SAMPLES;
            playback_ref_count--;
        } else {
            pcm[i] = 0;
        }
    }
    xSemaphoreGive(playback_ref_lock);
}

void audio_out_set_streaming(bool active)
{
    bool was_active = playback_streaming;
    playback_streaming = active;
    if (active) {
        state = AUDIO_OUT_STATE_PLAYING;
        if (!was_active) event_bus_publish(EV_AUDIO_OUT_START, NULL, 0);
    } else {
        state = AUDIO_OUT_STATE_IDLE;
        playback_ref_clear();
    }
}

bool audio_out_is_streaming(void)
{
    return playback_streaming;
}
int audio_out_play_opus(const uint8_t *data,size_t len)
{
    if (!decoder||!data||!len) return -1;
    int16_t *pcm=malloc(1920*sizeof(int16_t));if (!pcm) return -1;
    int samples=opus_dec_decode(decoder,data,len,pcm,1920*sizeof(int16_t));
    if (samples>0&&audio_out_write(pcm,samples*sizeof(int16_t))!=ESP_OK) samples=-1;
    free(pcm);return samples;
}
esp_err_t audio_out_stop(void)
{
    if (!output_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(output_lock,portMAX_DELAY);
    playback_streaming = false;
    playback_ref_clear();
    esp_err_t err=i2s_channel_disable(tx);
    if (err==ESP_OK) err=i2s_channel_enable(tx);
    state=AUDIO_OUT_STATE_IDLE;xSemaphoreGive(output_lock);return err;
}
esp_err_t audio_out_clear_queue(void) {return audio_out_stop();}
audio_out_state_t audio_out_get_state(void) {return state;}
void audio_out_set_event_cb(audio_out_event_cb_t cb,void *arg) {callback=cb;callback_arg=arg;}
void audio_out_dump_stats(void) {ESP_LOGI("audio_out","state=%d",state);}
