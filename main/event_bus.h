/**
 * @file event_bus.h
 * @brief 简化事件总线 — 用于跨服务事件分发
 */
#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 事件 ID（全局统一枚举，驱动 app_main 状态机 + LED 指示）
 */
typedef enum {
    EV_WIFI_GOT_IP          = 0,
    EV_WIFI_DISCONNECTED    = 1,
    EV_WIFI_CONNECT_FAILED  = 2,
    EV_WS_CONNECTED         = 3,
    EV_WS_DISCONNECTED      = 4,
    EV_WS_ERROR             = 5,
    EV_WS_AUTH_OK           = 6,
    EV_WS_SERVER_ERROR      = 7,
    EV_WS_CANCEL_DONE       = 8,
    EV_WS_SPEECH_STARTED    = 40,
    EV_WS_SPEECH_STOPPED    = 41,
    EV_WS_RESPONSE_DONE     = 42,
    EV_AUDIO_UPLOAD_FAILED  = 43,
    EV_AUDIO_WAKE_DETECTED  = 10,
    EV_AUDIO_SPEECH_START   = 13,
    EV_AUDIO_SPEECH_END     = 14,
    EV_AUDIO_SILENCE_TIMEOUT = 11,
    EV_AUDIO_INPUT_ERROR    = 12,
    EV_AUDIO_SPEAKING_STARTED = 20,
    EV_AUDIO_SPEAKING_DONE  = 21,
    EV_AUDIO_OUT_DONE       = 22,
    EV_AUDIO_OUT_READY      = 24,
    EV_AUDIO_OUT_START      = 25,
    EVENT_BUS_WIFI_STATE    = 30,
    EV_AUDIO_BARGE_IN       = 23,
    EV_SYSTEM_SHUTDOWN      = 100,
    EV_SYSTEM_ERROR         = 101,
} app_event_id_t;

#define EV_SYSTEM_WIFI_DISCONNECTED EV_WIFI_DISCONNECTED

typedef struct {
    app_event_id_t  id;
    void           *data;
    size_t          len;
} app_event_t;

/* Compatibility API used by service modules. */
typedef app_event_id_t event_type_t;
typedef struct {
    event_type_t type;
    void *payload;
    uint32_t timestamp_ms;
} event_t;
typedef struct event_bus_impl *event_bus_t;
typedef void (*event_handler_t)(event_t *event, void *user_data);
#define EV_COUNT 128

BaseType_t event_bus_publish(event_type_t type, const void *data, size_t len);

event_bus_t event_bus_create(UBaseType_t queue_len);
void event_bus_destroy(event_bus_t bus);
event_bus_t event_bus_get_global(void);
BaseType_t event_bus_emit(event_bus_t bus, event_type_t type, const void *data, size_t data_len);
BaseType_t event_bus_send(event_bus_t bus, const event_t *event);
BaseType_t event_bus_send_timed(event_bus_t bus, const event_t *event, TickType_t timeout_ticks);
BaseType_t event_bus_send_from_isr(event_bus_t bus, const event_t *event, BaseType_t *hp_woken);
BaseType_t event_bus_receive(event_bus_t bus, event_t *event, TickType_t timeout);
BaseType_t event_bus_subscribe(event_bus_t bus, event_type_t type, event_handler_t handler, void *user_data);
BaseType_t event_bus_unsubscribe(event_bus_t bus, event_type_t type, event_handler_t handler, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_BUS_H */
