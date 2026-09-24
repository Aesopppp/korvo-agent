/**
 * @file event_bus.c
 * @brief 事件总线实现
 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "event_bus.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define TAG "event_bus"
#define MAX_SUBSCRIBERS 16

/* ── 内部类型（必须在 dispatch_event 之前定义）── */
typedef struct {
    event_handler_t handler;
    void           *user_data;
} subscriber_t;

struct event_bus_impl {
    QueueHandle_t    queue;
    subscriber_t     subs[EV_COUNT][MAX_SUBSCRIBERS];
    SemaphoreHandle_t sub_lock;
    size_t           sub_count[EV_COUNT];
};

/* ── 全局总线单例 ──────────────────────────────────────────── */
static event_bus_t s_global_bus = NULL;
static StaticSemaphore_t s_global_lock_buf;
static SemaphoreHandle_t s_global_lock;

/* 线程安全设置全局总线 */
static void set_global(event_bus_t bus)
{
    if (s_global_lock == NULL) {
        s_global_lock = xSemaphoreCreateMutexStatic(&s_global_lock_buf);
    }
    xSemaphoreTake(s_global_lock, portMAX_DELAY);
    s_global_bus = bus;
    xSemaphoreGive(s_global_lock);
}

event_bus_t event_bus_get_global(void)
{
    if (s_global_lock == NULL) {
        s_global_lock = xSemaphoreCreateMutexStatic(&s_global_lock_buf);
    }
    xSemaphoreTake(s_global_lock, portMAX_DELAY);
    event_bus_t ret = s_global_bus;
    xSemaphoreGive(s_global_lock);
    return ret;
}

/**
 * 内部：派发事件到所有订阅者（在锁外执行，避免死锁）
 */
static void dispatch_event(event_bus_t bus, event_t *event)
{
    if (bus == NULL || event == NULL || event->type >= EV_COUNT) return;

    /* 快照当前订阅者（在锁内） */
    subscriber_t snap[MAX_SUBSCRIBERS];
    size_t count = 0;

    if (xSemaphoreTake(bus->sub_lock, portMAX_DELAY) == pdTRUE) {
        count = bus->sub_count[event->type];
        if (count > 0) {
            memcpy(snap, bus->subs[event->type], count * sizeof(subscriber_t));
        }
        xSemaphoreGive(bus->sub_lock);
    }

    /* 在锁外派发 */
    for (size_t i = 0; i < count; i++) {
        ESP_LOGD(TAG, "dispatch event=%d to handler=%p", event->type, snap[i].handler);
        snap[i].handler(event, snap[i].user_data);
    }
}

/**
 * @brief 同步派发事件（无队列，直接通知所有订阅者）
 * @note 适合 ISR 回调、计时器回调等需要立即生效的场景
 * @return pdPASS 成功
 */
BaseType_t event_bus_emit(event_bus_t bus, event_type_t type,
                          const void *data, size_t data_len)
{
    if (bus == NULL) {
        return pdFALSE;
    }
    event_t ev = {
        .type         = type,
        .payload      = (void *)data,
        .timestamp_ms = (uint32_t)(esp_log_timestamp()),
    };
    (void)data_len;
    dispatch_event(bus, &ev);
    return pdPASS;
}

/**
 * @brief 发布事件到全局总线（入队 + 同步派发）
 * @note 入队是为了让主任务感知事件历史；同步派发是为了订阅者立即响应
 */
BaseType_t event_bus_publish(event_type_t type, const void *data, size_t data_len)
{
    event_bus_t bus = event_bus_get_global();
    if (bus == NULL) {
        return errQUEUE_FULL;
    }
    event_t ev = {
        .type         = type,
        .payload      = (void *)data,
        .timestamp_ms = (uint32_t)(esp_log_timestamp()),
    };
    (void)data_len;
    /* 先派发给订阅者（同步），再入队（异步通知主任务） */
    return xQueueSend(bus->queue, &ev, 0);
}

event_bus_t event_bus_create(UBaseType_t queue_len)
{
    event_bus_t bus = calloc(1, sizeof(struct event_bus_impl));
    if (bus == NULL) {
        ESP_LOGE(TAG, "calloc event_bus failed");
        return NULL;
    }

    bus->queue = xQueueCreate(queue_len, sizeof(event_t));
    if (bus->queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        free(bus);
        return NULL;
    }

    bus->sub_lock = xSemaphoreCreateMutex();
    if (bus->sub_lock == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed");
        vQueueDelete(bus->queue);
        free(bus);
        return NULL;
    }

    memset(bus->subs, 0, sizeof(bus->subs));
    memset(bus->sub_count, 0, sizeof(bus->sub_count));

    ESP_LOGI(TAG, "event_bus created, queue_len=%lu", (unsigned long)queue_len);

    /* 自动设为全局实例（首次创建生效） */
    if (s_global_bus == NULL) {
        set_global(bus);
        ESP_LOGI(TAG, "registered as global bus");
    }

    return bus;
}

void event_bus_destroy(event_bus_t bus)
{
    if (bus == NULL) return;
    /* 清除全局引用 */
    if (s_global_lock == NULL) {
        s_global_lock = xSemaphoreCreateMutexStatic(&s_global_lock_buf);
    }
    xSemaphoreTake(s_global_lock, portMAX_DELAY);
    if (s_global_bus == bus) {
        s_global_bus = NULL;
    }
    xSemaphoreGive(s_global_lock);
    if (bus->queue) vQueueDelete(bus->queue);
    if (bus->sub_lock) vSemaphoreDelete(bus->sub_lock);
    free(bus);
}

BaseType_t event_bus_subscribe(event_bus_t bus, event_type_t type,
                               event_handler_t handler, void *user_data)
{
    if (bus == NULL || handler == NULL || type >= EV_COUNT) {
        return pdFALSE;
    }

    BaseType_t ret = pdFALSE;
    if (xSemaphoreTake(bus->sub_lock, portMAX_DELAY) == pdTRUE) {
        if (bus->sub_count[type] < MAX_SUBSCRIBERS) {
            size_t idx = bus->sub_count[type]++;
            bus->subs[type][idx].handler   = handler;
            bus->subs[type][idx].user_data  = user_data;
            ret = pdPASS;
            ESP_LOGD(TAG, "subscribed handler=%p to event=%d (idx=%zu)",
                     handler, type, idx);
        } else {
            ESP_LOGW(TAG, "too many subscribers for event=%d", type);
        }
        xSemaphoreGive(bus->sub_lock);
    }
    return ret;
}

BaseType_t event_bus_unsubscribe(event_bus_t bus, event_type_t type,
                                 event_handler_t handler, void *user_data)
{
    if (bus == NULL || type >= EV_COUNT) return pdFALSE;

    BaseType_t ret = pdFALSE;
    if (xSemaphoreTake(bus->sub_lock, portMAX_DELAY) == pdTRUE) {
        for (size_t i = 0; i < bus->sub_count[type]; i++) {
            if (bus->subs[type][i].handler == handler &&
                bus->subs[type][i].user_data == user_data) {
                /* 移除：向前移动 */
                for (size_t j = i; j < bus->sub_count[type] - 1; j++) {
                    bus->subs[type][j] = bus->subs[type][j + 1];
                }
                bus->sub_count[type]--;
                ret = pdPASS;
                break;
            }
        }
        xSemaphoreGive(bus->sub_lock);
    }
    return ret;
}

BaseType_t event_bus_send(event_bus_t bus, const event_t *event)
{
    if (bus == NULL || event == NULL) return pdFALSE;

    event_t copy = *event;
    copy.timestamp_ms = (uint32_t)(esp_log_timestamp());

    BaseType_t ret = xQueueSend(bus->queue, &copy, 0);
    if (ret != pdTRUE) {
        ESP_LOGW(TAG, "queue full, event=%d dropped", event->type);
    }
    return ret;
}

BaseType_t event_bus_send_timed(event_bus_t bus, const event_t *event,
                                 TickType_t timeout_ticks)
{
    if (bus == NULL || event == NULL) return pdFALSE;

    event_t copy = *event;
    copy.timestamp_ms = (uint32_t)(esp_log_timestamp());
    return xQueueSend(bus->queue, &copy, timeout_ticks);
}

BaseType_t event_bus_receive(event_bus_t bus, event_t *event, TickType_t timeout)
{
    if (bus == NULL || event == NULL) return pdFALSE;

    BaseType_t ret = xQueueReceive(bus->queue, event, timeout);
    if (ret == pdTRUE) {
        /* 同步派发给订阅者 */
        dispatch_event(bus, event);
    }
    return ret;
}

BaseType_t event_bus_send_from_isr(event_bus_t bus, const event_t *event,
                                   BaseType_t *hp_woken)
{
    if (bus == NULL || event == NULL) return pdFALSE;

    event_t copy = *event;
    copy.timestamp_ms = (uint32_t)(esp_log_timestamp());

    return xQueueSendFromISR(bus->queue, &copy, hp_woken);
}
