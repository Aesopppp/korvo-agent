/**
 * @file led_service.c
 * @brief LED 服务实现
 */

#include "led_service.h"
#include <stdlib.h>
#include "esp_log.h"
#include "board_config.h"
#include "event_bus.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

static const char *TAG = "led_service";

/* ── 全局单例 ──────────────────────────────────────────────── */
static led_service_t s_led_service = NULL;

/* ── TCA9554 I2C 地址 (board_config.h 已定义) ─────────────── */
#ifndef TCA9554_I2C_ADDR
#define TCA9554_I2C_ADDR   0x20
#endif

/* ── 应用层模式宏（简化 app_main 调用） ─────────────────────── */

/**
 * Wi-Fi 连接成功: 绿常亮
 * @return ESP_OK on success
 */
esp_err_t led_set_wifi_ok(void)
{
    if (s_led_service == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(s_led_service, LED_GREEN, LED_PATTERN_ON);
    led_service_set_pattern(s_led_service, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

/**
 * Wi-Fi 连接失败: 红快闪
 */
esp_err_t led_set_wifi_err(void)
{
    if (s_led_service == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(s_led_service, LED_GREEN, LED_PATTERN_OFF);
    led_service_set_pattern(s_led_service, LED_RED,   LED_PATTERN_BLINK_FAST);
    return ESP_OK;
}

/**
 * WebSocket 已连接: 绿慢闪
 */
esp_err_t led_set_ws_ok(void)
{
    if (s_led_service == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(s_led_service, LED_GREEN, LED_PATTERN_BLINK_SLOW);
    led_service_set_pattern(s_led_service, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

/**
 * WebSocket 断开/错误: 红慢闪
 */
esp_err_t led_set_ws_err(void)
{
    if (s_led_service == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(s_led_service, LED_GREEN, LED_PATTERN_OFF);
    led_service_set_pattern(s_led_service, LED_RED,   LED_PATTERN_BLINK_SLOW);
    return ESP_OK;
}

/**
 * 等待唤醒词: 绿呼吸
 */
esp_err_t led_set_waiting_wake(void)
{
    if (s_led_service == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(s_led_service, LED_GREEN, LED_PATTERN_BREATHE);
    led_service_set_pattern(s_led_service, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

/**
 * 正在录音/说话: 绿快闪
 */
esp_err_t led_set_listening(void)
{
    if (s_led_service == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(s_led_service, LED_GREEN, LED_PATTERN_BLINK_FAST);
    led_service_set_pattern(s_led_service, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

/**
 * 放音中: 绿慢闪
 */
esp_err_t led_set_speaking(void)
{
    if (s_led_service == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(s_led_service, LED_GREEN, LED_PATTERN_BLINK_SLOW);
    led_service_set_pattern(s_led_service, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

/**
 * @brief 初始化 LED 服务（一键式，内部初始化 I2C + TCA9554 + LED service）
 * @return 全局 LED 服务句柄，失败返回 NULL
 */
led_service_t led_service_init(void)
{
    if (s_led_service != NULL) {
        return s_led_service;  // 已初始化
    }

    /* I2C 总线初始化 — ESP-IDF v5.4 全新 master driver */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA_PIN,
        .scl_io_num = BSP_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    tca9554_handle_t tca9554 = tca9554_new(bus_handle, TCA9554_I2C_ADDR);
    if (tca9554 == NULL) {
        ESP_LOGE(TAG, "tca9554_new failed; running I2C address diagnostics");
        ESP_LOGI(TAG, "I2C scan: SDA=GPIO%d level=%d, SCL=GPIO%d level=%d",
                 BSP_I2C_SDA_PIN, gpio_get_level(BSP_I2C_SDA_PIN),
                 BSP_I2C_SCL_PIN, gpio_get_level(BSP_I2C_SCL_PIN));
        unsigned found = 0, timeouts = 0, other_errors = 0;
        for (unsigned addr = 0x08; addr <= 0x77; ++addr) {
            esp_err_t probe = i2c_master_probe(bus_handle, addr, 20);
            if (probe == ESP_OK) {
                ESP_LOGI(TAG, "I2C ACK at 0x%02x (address response only)", addr);
                ++found;
            } else if (probe == ESP_ERR_TIMEOUT) {
                ++timeouts;
            } else if (probe != ESP_ERR_NOT_FOUND) {
                ++other_errors;
            }
        }
        ESP_LOGW(TAG, "I2C scan complete: %u responding addresses, %u timeouts, %u other errors",
                 found, timeouts, other_errors);
        i2c_del_master_bus(bus_handle);
        return NULL;
    }

    /* LED 服务 */
    s_led_service = led_service_create(tca9554);
    if (s_led_service == NULL) {
        ESP_LOGE(TAG, "led_service_create failed");
        tca9554_delete(tca9554);
        return NULL;
    }

    ESP_LOGI(TAG, "led_service initialized (TCA9554 0x%02X)", TCA9554_I2C_ADDR);
    return s_led_service;
}

/* TCA9554 引脚到 LED 索引的映射 */
static const uint8_t s_pin_map[LED_COUNT] = {
    [LED_GREEN] = TCA9554_PIN_LED_GREEN,
    [LED_RED]   = TCA9554_PIN_LED_RED,
};

/* 闪烁间隔 (ms) */
#define BLINK_FAST_INTERVAL_MS  100
#define BLINK_SLOW_INTERVAL_MS  500

struct led_service_impl {
    tca9554_handle_t  tca9554;
    led_state_t       leds[LED_COUNT];
    event_bus_t       bus;
};

led_service_t led_service_create(tca9554_handle_t tca9554)
{
    if (tca9554 == NULL) {
        ESP_LOGE(TAG, "tca9554 handle is NULL");
        return NULL;
    }

    led_service_t svc = calloc(1, sizeof(struct led_service_impl));
    if (svc == NULL) return NULL;

    svc->tca9554 = tca9554;

    /* 初始化所有 LED 为 OFF */
    for (int i = 0; i < LED_COUNT; i++) {
        svc->leds[i].pattern       = LED_PATTERN_OFF;
        svc->leds[i].current_state = false;
        svc->leds[i].enabled      = true;
        svc->leds[i].last_toggle_ms = 0;
    }

    /* TCA9554 默认值: 方向=输出, PA_SD=1 (不禁闭), LED=0 (熄灭) */
    uint8_t def_dir  = TCA9554_DEFAULT_DIR;
    uint8_t def_out  = TCA9554_DEFAULT_OUT;  /* PA_SD=1, LED OFF */
    esp_err_t ret = tca9554_init_defaults(tca9554, def_dir, def_out);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tca9554_init_defaults failed: %s", esp_err_to_name(ret));
        free(svc);
        return NULL;
    }

    ESP_LOGI(TAG, "led_service created");
    return svc;
}

void led_service_destroy(led_service_t svc)
{
    if (svc) {
        /* 确保所有 LED 熄灭 */
        tca9554_set_output(svc->tca9554, TCA9554_DEFAULT_OUT);
        free(svc);
    }
}

static void led_apply_state(led_service_t svc)
{
    uint8_t out = TCA9554_DEFAULT_OUT;  /* 保留 PA_SD */

    for (int i = 0; i < LED_COUNT; i++) {
        if (!svc->leds[i].enabled) continue;

        uint8_t pin = s_pin_map[i];
        if (svc->leds[i].current_state) {
            out |= (1 << pin);   /* 低电平点亮 (LED->GND) */
        } else {
            out &= ~(1 << pin);
        }
    }

    tca9554_set_output(svc->tca9554, out);
}

void led_service_poll(led_service_t svc, uint32_t now_ms)
{
    if (svc == NULL) return;

    bool changed = false;

    for (int i = 0; i < LED_COUNT; i++) {
        if (!svc->leds[i].enabled) continue;

        switch (svc->leds[i].pattern) {
            case LED_PATTERN_OFF:
                if (svc->leds[i].current_state != false) {
                    svc->leds[i].current_state = false;
                    changed = true;
                }
                break;

            case LED_PATTERN_ON:
                if (svc->leds[i].current_state != true) {
                    svc->leds[i].current_state = true;
                    changed = true;
                }
                break;

            case LED_PATTERN_BLINK_FAST:
                if ((int)(now_ms - svc->leds[i].last_toggle_ms) >= BLINK_FAST_INTERVAL_MS) {
                    svc->leds[i].current_state = !svc->leds[i].current_state;
                    svc->leds[i].last_toggle_ms = now_ms;
                    changed = true;
                }
                break;

            case LED_PATTERN_BLINK_SLOW:
                if ((int)(now_ms - svc->leds[i].last_toggle_ms) >= BLINK_SLOW_INTERVAL_MS) {
                    svc->leds[i].current_state = !svc->leds[i].current_state;
                    svc->leds[i].last_toggle_ms = now_ms;
                    changed = true;
                }
                break;

            case LED_PATTERN_BREATHE:
                /* 简化: 慢闪代替呼吸 */
                if ((int)(now_ms - svc->leds[i].last_toggle_ms) >= 800) {
                    svc->leds[i].current_state = !svc->leds[i].current_state;
                    svc->leds[i].last_toggle_ms = now_ms;
                    changed = true;
                }
                break;
        }
    }

    if (changed) {
        led_apply_state(svc);
    }
}

esp_err_t led_service_set_pattern(led_service_t svc, led_id_t id, led_pattern_t pattern)
{
    if (svc == NULL || id >= LED_COUNT) return ESP_ERR_INVALID_ARG;

    svc->leds[id].pattern = pattern;
    svc->leds[id].last_toggle_ms = 0;
    svc->leds[id].current_state = false;

    ESP_LOGD(TAG, "led[%d] set pattern=%d", id, pattern);

    /* 立即应用 */
    led_apply_state(svc);
    return ESP_OK;
}

esp_err_t led_service_idle(led_service_t svc)
{
    if (svc == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(svc, LED_GREEN, LED_PATTERN_ON);
    led_service_set_pattern(svc, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

esp_err_t led_service_recording(led_service_t svc)
{
    if (svc == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(svc, LED_GREEN, LED_PATTERN_BLINK_FAST);
    led_service_set_pattern(svc, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

esp_err_t led_service_playing(led_service_t svc)
{
    if (svc == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(svc, LED_GREEN, LED_PATTERN_BLINK_SLOW);
    led_service_set_pattern(svc, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

esp_err_t led_service_error(led_service_t svc)
{
    if (svc == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(svc, LED_GREEN, LED_PATTERN_OFF);
    led_service_set_pattern(svc, LED_RED,   LED_PATTERN_BLINK_FAST);
    return ESP_OK;
}

esp_err_t led_service_off(led_service_t svc)
{
    if (svc == NULL) return ESP_ERR_INVALID_STATE;
    led_service_set_pattern(svc, LED_GREEN, LED_PATTERN_OFF);
    led_service_set_pattern(svc, LED_RED,   LED_PATTERN_OFF);
    return ESP_OK;
}

/* 事件总线处理函数 */
static void on_event_bus(event_t *event, void *user_data)
{
    led_service_t svc = (led_service_t)user_data;
    if (svc == NULL) return;

    switch (event->type) {
        case EV_WS_AUTH_OK:
            led_service_idle(svc);
            break;
        case EV_AUDIO_SPEECH_START:
            led_service_recording(svc);
            break;
        case EV_AUDIO_SPEECH_END:
            led_service_idle(svc);
            break;
        case EV_WS_DISCONNECTED:
        case EV_SYSTEM_WIFI_DISCONNECTED:
            led_service_off(svc);
            break;
        case EV_AUDIO_OUT_START:
            led_service_playing(svc);
            break;
        case EV_AUDIO_BARGE_IN:
            led_service_recording(svc);
            break;
        case EV_SYSTEM_ERROR:
        case EV_WS_ERROR:
            led_service_error(svc);
            break;
        default:
            break;
    }
}

void led_service_register_events(led_service_t svc)
{
    if (svc == NULL || svc->bus == NULL) return;

    event_type_t events[] = {
        EV_WS_AUTH_OK, EV_WS_DISCONNECTED, EV_WS_ERROR,
        EV_SYSTEM_WIFI_DISCONNECTED,
        EV_AUDIO_SPEECH_START, EV_AUDIO_SPEECH_END,
        EV_AUDIO_OUT_START, EV_AUDIO_BARGE_IN,
        EV_SYSTEM_ERROR,
    };

    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
        event_bus_subscribe(svc->bus, events[i], on_event_bus, svc);
    }
}
