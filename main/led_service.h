/**
 * @file led_service.h
 * @brief LED 状态服务 — 通过 TCA9554 驱动绿/红 LED
 */
#ifndef LED_SERVICE_H
#define LED_SERVICE_H

#include <stdint.h>
#include "tca9554.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LED 索引 */
typedef enum {
    LED_GREEN = 0,
    LED_RED   = 1,
    LED_COUNT
} led_id_t;

/* LED 图案模式 */
typedef enum {
    LED_PATTERN_OFF = 0,
    LED_PATTERN_ON,        /* 常亮 */
    LED_PATTERN_BLINK_FAST, /* 快闪 100ms on/off */
    LED_PATTERN_BLINK_SLOW, /* 慢闪 500ms on/off */
    LED_PATTERN_BREATHE,    /* 呼吸灯 (PWM, via TCA9554 快速切换) */
} led_pattern_t;

/* LED 状态 */
typedef struct {
    led_pattern_t  pattern;
    uint32_t       last_toggle_ms;
    bool           current_state;   /* 当前物理状态 */
    bool           enabled;
} led_state_t;

/* LED 服务句柄 */
typedef struct led_service_impl *led_service_t;

/**
 * @brief 初始化 LED 服务（一键式，内部初始化 I2C 总线 + TCA9554 + LED service）
 *
 * 内部流程：i2c_driver_install → tca9554_create → led_service_create
 * @return 全局 LED 服务句柄，失败返回 NULL
 */
led_service_t led_service_init(void);

/**
 * @brief 创建 LED 服务 (需要先初始化 I2C)
 * @param tca9554 已初始化的 TCA9554 句柄
 * @return LED 服务句柄
 */
led_service_t led_service_create(tca9554_handle_t tca9554);

/* ── 简化调用层（app_main 直接使用，无需传入 svc 句柄）── */

esp_err_t led_set_wifi_ok(void);       /* Wi-Fi 连接成功: 绿常亮 */
esp_err_t led_set_wifi_err(void);      /* Wi-Fi 连接失败: 红快闪 */
esp_err_t led_set_ws_ok(void);         /* WebSocket 已连接: 绿慢闪 */
esp_err_t led_set_ws_err(void);         /* WebSocket 断开/错误: 红慢闪 */
esp_err_t led_set_waiting_wake(void);  /* 等待唤醒词: 绿呼吸 */
esp_err_t led_set_listening(void);      /* 正在录音: 绿快闪 */
esp_err_t led_set_speaking(void);       /* 放音中: 绿慢闪 */

/**
 * @brief 销毁 LED 服务
 */
void led_service_destroy(led_service_t svc);

/**
 * @brief 周期性调用 (在 app_main 循环或 timer 中调用)
 * @param svc  LED 服务句柄
 * @param now_ms 当前系统时间 (esp_log_timestamp)
 */
void led_service_poll(led_service_t svc, uint32_t now_ms);

/**
 * @brief 设置指定 LED 的图案
 */
esp_err_t led_service_set_pattern(led_service_t svc, led_id_t id, led_pattern_t pattern);

/**
 * @brief 快速设置常见状态
 */
esp_err_t led_service_idle(led_service_t svc);    /* 绿常亮 */
esp_err_t led_service_recording(led_service_t svc); /* 绿快闪 */
esp_err_t led_service_playing(led_service_t svc);  /* 绿慢闪 */
esp_err_t led_service_error(led_service_t svc);     /* 红快闪 */
esp_err_t led_service_off(led_service_t svc);       /* 全灭 */

/* 事件总线集成 */
void led_service_register_events(led_service_t svc);

#ifdef __cplusplus
}
#endif

#endif /* LED_SERVICE_H */
