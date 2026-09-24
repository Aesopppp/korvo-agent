#include "ws_service.h"
#include "audio_out.h"
#include "proto_service.h"
#include "event_bus.h"
#include "cfg_service.h"
#include "board_config.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static esp_websocket_client_handle_t client;
static atomic_int state=WS_STATE_DISCONNECTED;
static char uri[256];
static uint8_t *frame;
static size_t frame_size, received;
static int opcode;
/* Set after response.cancel so late binary frames from the old response are
 * discarded instead of restarting the speaker and retriggering barge-in. */
static atomic_bool response_cancelled = false;

static void handle_frame(void)
{
    if (opcode==1) {
        frame[received]=0;
        proto_msg_t msg;
        if (proto_parse_json((char *)frame,&msg)!=PROTO_OK) return;
        if (msg.evt==PROTO_EVT_PROXY_CONNECTED) {
            state=WS_STATE_CONNECTED;
            event_bus_publish(EV_WS_AUTH_OK,NULL,0);
        } else if (msg.evt==PROTO_EVT_SPEECH_STARTED) {
            /* A new user turn has started; a following binary response is
             * allowed after the previous cancellation. */
            atomic_store(&response_cancelled, false);
        } else if (msg.evt==PROTO_EVT_RESPONSE_DONE) {
            atomic_store(&response_cancelled, false);
            audio_out_set_streaming(false);
            event_bus_publish(EV_AUDIO_OUT_DONE,NULL,0);
        } else if (msg.evt==PROTO_EVT_ERROR) {
            state=WS_STATE_ERROR;
            event_bus_publish(EV_WS_ERROR,NULL,0);
        }
        proto_dispatch_data((char *)frame,received);
    } else if (opcode==2) {
        if (atomic_load(&response_cancelled)) {
            ESP_LOGD("ws_service", "Dropping late Opus frame after response.cancel");
            return;
        }
        audio_out_set_streaming(true);
        if (audio_out_play_opus(frame,received)<0)
            ESP_LOGW("ws_service","Opus playback failed");
    }
}

static void on_ws(void *arg,esp_event_base_t base,int32_t id,void *event)
{
    if (id==WEBSOCKET_EVENT_CONNECTED) {
        atomic_store(&response_cancelled, false);
        state=WS_STATE_AUTHING;
        event_bus_publish(EV_WS_CONNECTED,NULL,0);
        char token[256],auth[512];
        cfg_service_get_str(CFG_KEY_ACOS_TOKEN,token,sizeof(token),"");
        if (!token[0]) {ESP_LOGE("ws_service","Configure ACOS token in NVS");return;}
        size_t len=proto_build_auth_frame(auth,sizeof(auth),token);
        if (!len||esp_websocket_client_send_text(client,auth,len,pdMS_TO_TICKS(1000))!=(int)len)
            event_bus_publish(EV_WS_ERROR,NULL,0);
    } else if (id==WEBSOCKET_EVENT_DISCONNECTED) {
        atomic_store(&response_cancelled, false);
        audio_out_set_streaming(false);
        state=WS_STATE_DISCONNECTED;
        free(frame);frame=NULL;frame_size=received=0;
        event_bus_publish(EV_WS_DISCONNECTED,NULL,0);
    } else if (id==WEBSOCKET_EVENT_ERROR) {
        atomic_store(&response_cancelled, false);
        audio_out_set_streaming(false);
        state=WS_STATE_ERROR;event_bus_publish(EV_WS_ERROR,NULL,0);
    } else if (id==WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *data=event;
        if (data->op_code!=1&&data->op_code!=2) return;
        if (data->payload_len<=0||data->payload_len>65536||data->data_len<0||data->payload_offset<0) return;
        if (data->payload_offset==0) {
            free(frame);frame=NULL;received=0;
            frame_size=data->payload_len;opcode=data->op_code;
            frame=malloc(frame_size+1);
        }
        if (!frame||(size_t)data->payload_offset!=received||received+(size_t)data->data_len>frame_size) return;
        memcpy(frame+received,data->data_ptr,data->data_len);received+=data->data_len;
        if (received==frame_size) {handle_frame();free(frame);frame=NULL;}
    }
}

esp_err_t ws_service_init(void)
{
    if (client) return ESP_OK;
    cfg_service_get_str(CFG_KEY_WS_URL,uri,sizeof(uri),ACOS_WS_URL);
    esp_websocket_client_config_t cfg={.uri=uri,.reconnect_timeout_ms=5000,
        .crt_bundle_attach=esp_crt_bundle_attach};
    client=esp_websocket_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    esp_err_t err=esp_websocket_register_events(client,WEBSOCKET_EVENT_ANY,on_ws,NULL);
    if (err!=ESP_OK) {esp_websocket_client_destroy(client);client=NULL;}
    return err;
}
esp_err_t ws_service_connect(void)
{
    if (!client) return ESP_ERR_INVALID_STATE;
    if (state==WS_STATE_CONNECTING||state==WS_STATE_AUTHING||state==WS_STATE_CONNECTED) return ESP_OK;
    state=WS_STATE_CONNECTING;
    esp_err_t err=esp_websocket_client_start(client);
    if (err!=ESP_OK) state=WS_STATE_ERROR;
    return err;
}
esp_err_t ws_service_disconnect(void)
{
    if (!client) return ESP_ERR_INVALID_STATE;
    esp_err_t err=esp_websocket_client_stop(client);state=WS_STATE_DISCONNECTED;return err;
}
esp_err_t ws_service_deinit(void)
{
    if (client) {esp_websocket_client_stop(client);esp_websocket_client_destroy(client);client=NULL;}
    free(frame);frame=NULL;state=WS_STATE_DISCONNECTED;return ESP_OK;
}
ws_conn_state_t ws_service_get_state(void) {return state;}
esp_websocket_client_handle_t ws_service_get_client(void) {return client;}
esp_err_t ws_service_send_json(const char *text)
{
    if (!client||state!=WS_STATE_CONNECTED) return ESP_ERR_INVALID_STATE;
    if (!text) return ESP_ERR_INVALID_ARG;
    size_t len=strlen(text);
    return esp_websocket_client_send_text(client,text,len,pdMS_TO_TICKS(1000))==(int)len?ESP_OK:ESP_FAIL;
}
esp_err_t ws_service_send_opus_frame(const uint8_t *data,size_t len)
{
    if (!data||!len||len>65536) return ESP_ERR_INVALID_ARG;
    if (state!=WS_STATE_CONNECTED) return ESP_ERR_INVALID_STATE;
    size_t capacity=4*((len+2)/3)+1,written;
    char *encoded=malloc(capacity),*json=malloc(capacity+256);
    if (!encoded||!json) {free(encoded);free(json);return ESP_ERR_NO_MEM;}
    esp_err_t err=ESP_FAIL;
    if (mbedtls_base64_encode((unsigned char *)encoded,capacity,&written,data,len)==0) {
        encoded[written]=0;
        if (proto_build_input_audio_append(json,capacity+256,encoded,NULL)) err=ws_service_send_json(json);
    }
    free(encoded);free(json);return err;
}

esp_err_t ws_service_cancel_response(void)
{
    atomic_store(&response_cancelled, true);
    return ws_service_send_json("{\"type\":\"response.cancel\"}");
}
