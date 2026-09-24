/* Korvo-2: shared full-duplex I2S with ES8311 and ES7210. */
#include "audio_out.h"
#include "board_config.h"
#include "event_bus.h"
#include "opus_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pcm_policy.h"
#include <stdatomic.h>
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
static atomic_int state;
static audio_out_event_cb_t callback;
static void *callback_arg;
static int16_t *playback_ref;
static size_t playback_ref_read;
static size_t playback_ref_write;
static size_t playback_ref_count;
static SemaphoreHandle_t playback_ref_lock;
static atomic_bool playback_streaming;
static TaskHandle_t playback_task;
static atomic_bool task_exit, task_exited;
static int16_t pcm_ring[PCM_CAPACITY], decode_pcm[1920];
static size_t pcm_read, pcm_write, pcm_count;
static bool stream_open, stream_eof, stream_drained = true, output_started;
static unsigned drain_blocks, fade_samples;
static int16_t last_sample;
static uint32_t underruns, supplied_samples, decoded_packets, decode_max_us, write_max_us;
static int64_t stats_at;
static void playback_worker(void *arg);


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
    channel.dma_frame_num=PCM_BLOCK;
    channel.dma_desc_num=PCM_DMA_BLOCKS;
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
    if ((err=esp_codec_dev_set_out_vol(speaker,40))!=ESP_OK) goto fail;
    if ((err=esp_codec_dev_set_in_gain(microphone,30.0f))!=ESP_OK) goto fail;
    output_lock=xSemaphoreCreateMutex(); decoder=opus_dec_create(NULL);
    playback_ref_lock = xSemaphoreCreateMutex();
    playback_ref = calloc(PLAYBACK_REF_CAP_SAMPLES, sizeof(int16_t));
    if (!output_lock||!decoder||!playback_ref_lock||!playback_ref) {err=ESP_ERR_NO_MEM;goto fail;}
    playback_streaming = false;
    state=AUDIO_OUT_STATE_IDLE;
    pcm_read=pcm_write=pcm_count=0;
    stream_open=stream_eof=output_started=false;
    stream_drained=true; last_sample=0;
    atomic_store(&task_exit, false);
    atomic_store(&task_exited, false);
    if (xTaskCreatePinnedToCore(playback_worker, "pcm_output", 4096, NULL, 6,
                                &playback_task, 1) != pdPASS) {
        err=ESP_ERR_NO_MEM; goto fail;
    }
    ESP_LOGI("audio_out", "PCM output: 150 ms prefill, 500 ms capacity, DMA=60 ms, core=1 priority=6");
    event_bus_publish(EV_AUDIO_OUT_READY,NULL,0);
    if (callback) callback(AUDIO_OUT_EVENT_INIT_OK,callback_arg);
    return ESP_OK;
fail:
    audio_out_deinit(); state=AUDIO_OUT_STATE_ERROR; return err;
}

esp_err_t audio_out_deinit(void)
{
    if (playback_task) {
        atomic_store(&task_exit, true);
        while (!atomic_load(&task_exited)) vTaskDelay(1);
        playback_task=NULL;
    }
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

/* All PCM state and hardware writes are serialized by output_lock.
 * The output task continuously clocks silence too; an empty network queue never
 * leaves stale DMA audio repeating. Reference is fed at output, not decode time.
 * Software reference still needs acoustic alignment validation on the board. */
static esp_err_t write_block(int16_t *mono)
{
    int16_t stereo[PCM_BLOCK * 2];
    for (size_t i=0;i<PCM_BLOCK;++i) stereo[2*i]=stereo[2*i+1]=mono[i];
    int64_t start=esp_timer_get_time();
    esp_err_t err=esp_codec_dev_write(speaker,stereo,sizeof(stereo));
    uint32_t elapsed=(uint32_t)(esp_timer_get_time()-start);
    if (elapsed>write_max_us) write_max_us=elapsed;
    if (err==ESP_OK) playback_ref_push(mono,PCM_BLOCK);
    return err;
}
static void ramp_to_zero(int16_t *mono, size_t from)
{
    int16_t previous=from ? mono[from-1] : last_sample;
    size_t n=PCM_BLOCK-from;
    for(size_t i=0;i<n;++i) mono[from+i]=pcm_scale(previous,n-i-1,n);
}
static void playback_worker(void *arg)
{
    (void)arg;
    while(!atomic_load(&task_exit)) {
        int16_t mono[PCM_BLOCK]={0};
        xSemaphoreTake(output_lock,portMAX_DELAY);
        if(stream_open && !output_started && pcm_can_start(pcm_count,stream_eof)) {
            output_started=true;
            fade_samples=80; /* 5 ms onset ramp */
            atomic_store(&playback_streaming,true);
            event_bus_publish(EV_AUDIO_OUT_START,NULL,0);
        }
        size_t n=output_started ? pcm_take_count(pcm_count) : 0;
        for(size_t i=0;i<n;++i) {
            mono[i]=pcm_ring[pcm_read]; pcm_read=(pcm_read+1)%PCM_CAPACITY;
            if(fade_samples) { mono[i]=pcm_scale(mono[i],80-fade_samples,80); --fade_samples; }
        }
        pcm_count-=n;
        supplied_samples+=n;
        if(output_started && n<PCM_BLOCK && !stream_eof) {
            ++underruns;
            ramp_to_zero(mono,n);
            output_started=false; /* refill before resuming, count one per starvation */
        } else if(stream_eof && !pcm_count && n<PCM_BLOCK && last_sample) {
            ramp_to_zero(mono,n);
        }
        esp_err_t err=write_block(mono);
        last_sample=mono[PCM_BLOCK-1];
        if(stream_eof && !pcm_count && n==0) {
            if(++drain_blocks>PCM_DMA_BLOCKS) {
                stream_drained=true; stream_open=false; output_started=false;
                atomic_store(&playback_streaming,false);
                state=AUDIO_OUT_STATE_IDLE;
            }
        } else drain_blocks=0;
        if(err!=ESP_OK) { state=AUDIO_OUT_STATE_ERROR; stream_drained=true; }
        int64_t now=esp_timer_get_time();
        bool report=stream_open && now-stats_at>=5000000;
        size_t buffered=pcm_count;
        uint32_t gaps=underruns, dec=decode_max_us, wr=write_max_us, samples=supplied_samples;
        if(report) { stats_at=now; decode_max_us=write_max_us=0; }
        xSemaphoreGive(output_lock);
        if(report) ESP_LOGI("audio_out", "Playback stats: buffered_ms=%u starvations=%lu decode_max_us=%lu write_max_us=%lu pcm_ms=%lu",
            (unsigned)(buffered/16),(unsigned long)gaps,(unsigned long)dec,(unsigned long)wr,(unsigned long)(samples/16));
        if(err!=ESP_OK) { event_bus_publish(EV_AUDIO_INPUT_ERROR,NULL,0); vTaskDelay(pdMS_TO_TICKS(20)); }
        /* Fairness between blocks even if DMA has several free descriptors. */
        vTaskDelay(1);
    }
    atomic_store(&task_exited,true);
    vTaskDelete(NULL);
}

/* Bounded producer backpressure. The WS worker owns the decoder and serializes
 * this call with cancellation. A packet is at most 120 ms of mono PCM. */
esp_err_t audio_out_write(const int16_t *pcm_data,size_t pcm_len)
{
    if(!speaker||!output_lock) return ESP_ERR_INVALID_STATE;
    if(!pcm_data||pcm_len%sizeof(int16_t)) return ESP_ERR_INVALID_ARG;
    size_t pos=0,total=pcm_len/sizeof(int16_t);
    int64_t deadline=esp_timer_get_time()+2000000;
    while(pos<total) {
        xSemaphoreTake(output_lock,portMAX_DELAY);
        if(!stream_open || stream_eof || state==AUDIO_OUT_STATE_ERROR) {
            xSemaphoreGive(output_lock); return ESP_ERR_INVALID_STATE;
        }
        while(pos<total && pcm_count<PCM_CAPACITY) {
            pcm_ring[pcm_write]=pcm_data[pos++];
            pcm_write=(pcm_write+1)%PCM_CAPACITY; ++pcm_count;
        }
        xSemaphoreGive(output_lock);
        if(pos<total) {
            if(esp_timer_get_time()>deadline) return ESP_ERR_TIMEOUT;
            vTaskDelay(1);
        }
    }
    return ESP_OK;
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
    size_t skip=pcm_reference_skip(playback_ref_count,samples);
    playback_ref_read=(playback_ref_read+skip)%PLAYBACK_REF_CAP_SAMPLES;
    playback_ref_count-=skip;
    size_t available=pcm_reference_available(playback_ref_count);
    size_t silence=samples>available?samples-available:0;
    for (size_t i = 0; i < samples; ++i) {
        if (i >= silence && playback_ref_count > PCM_DMA_SAMPLES) {
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
    xSemaphoreTake(output_lock,portMAX_DELAY);
    if(active && !stream_open) {
        pcm_read=pcm_write=pcm_count=0;
        stream_open=true; stream_eof=false; stream_drained=false; output_started=false;
        drain_blocks=0; underruns=supplied_samples=decoded_packets=decode_max_us=write_max_us=0;
        stats_at=esp_timer_get_time(); state=AUDIO_OUT_STATE_PLAYING;
    } else if(!active) {
        stream_eof=true; drain_blocks=0; /* flush even replies shorter than prefill */
    }
    xSemaphoreGive(output_lock);
}
bool audio_out_is_streaming(void) { return atomic_load(&playback_streaming); }
bool audio_out_is_drained(void)
{
    xSemaphoreTake(output_lock,portMAX_DELAY);
    bool done=stream_drained && state!=AUDIO_OUT_STATE_ERROR;
    xSemaphoreGive(output_lock);
    return done;
}
int audio_out_play_opus(const uint8_t *data,size_t len)
{
    if(!decoder||!data||!len) return -1;
    int64_t start=esp_timer_get_time();
    int samples=opus_dec_decode(decoder,data,len,decode_pcm,sizeof(decode_pcm));
    uint32_t elapsed=(uint32_t)(esp_timer_get_time()-start);
    xSemaphoreTake(output_lock,portMAX_DELAY);
    if(elapsed>decode_max_us) decode_max_us=elapsed;
    ++decoded_packets;
    xSemaphoreGive(output_lock);
    if(samples>0 && audio_out_write(decode_pcm,samples*sizeof(int16_t))!=ESP_OK) return -1;
    return samples;
}
esp_err_t audio_out_stop(void)
{
    if(!output_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(output_lock,portMAX_DELAY);
    pcm_read=pcm_write=pcm_count=0;
    stream_open=false; stream_eof=false; output_started=false;
    atomic_store(&playback_streaming,false);
    int16_t mono[PCM_BLOCK]={0};
    ramp_to_zero(mono,0);
    esp_err_t err=write_block(mono);
    memset(mono,0,sizeof(mono));
    /* Leave shared RX/TX clocks running. Flush the DMA horizon with zeros,
     * so no old answer can leak into the next turn after this returns. */
    for(unsigned i=0;i<=PCM_DMA_BLOCKS && err==ESP_OK;++i) err=write_block(mono);
    last_sample=0; stream_drained=true;
    state=err==ESP_OK?AUDIO_OUT_STATE_IDLE:AUDIO_OUT_STATE_ERROR;
    xSemaphoreGive(output_lock);
    return err;
}
esp_err_t audio_out_clear_queue(void) { return audio_out_stop(); }
audio_out_state_t audio_out_get_state(void) { return atomic_load(&state); }
void audio_out_set_event_cb(audio_out_event_cb_t cb,void *arg) {callback=cb;callback_arg=arg;}
void audio_out_dump_stats(void) {ESP_LOGI("audio_out","state=%d",atomic_load(&state));}
