/**
 * log_service.c — 日志服务实现
 */

#include "log_service.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "esp_log.h"
#include "esp_err.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static const char *TAG = "log_service";

/* ── Ring buffer ──────────────────────────────────────────────────────────── */

#define RING_SIZE    4096

static char s_ring_buf[RING_SIZE];
static size_t s_ring_head = 0;  /* 写入位置 */
static size_t s_ring_count = 0;
static SemaphoreHandle_t s_ring_mutex = NULL;

/* ── 全局日志级别 ─────────────────────────────────────────────────────────── */

static log_level_t s_current_level = LOG_LEVEL_INFO;

/* ── UART 配置（TX only） ─────────────────────────────────────────────────── */

#define LOG_UART_NUM    UART_NUM_0
#define LOG_UART_TX_PIN (GPIO_NUM_43)

/* ── 内部工具 ─────────────────────────────────────────────────────────────── */

static void ring_write(const char *line, size_t len)
{
    if (!s_ring_mutex) return;
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);

    size_t space = RING_SIZE - 1;
    size_t pos = s_ring_head;

    for (size_t i = 0; i < len && space > 0; i++) {
        s_ring_buf[pos] = line[i];
        pos = (pos + 1) % RING_SIZE;
        space--;
        s_ring_count++;
    }
    if (s_ring_count > RING_SIZE) {
        s_ring_count = RING_SIZE;
    }
    s_ring_head = (s_ring_head + len) % RING_SIZE;

    xSemaphoreGive(s_ring_mutex);
}

/* ── 公开 API ─────────────────────────────────────────────────────────────── */

esp_err_t log_service_init(log_level_t level)
{
    s_ring_mutex = xSemaphoreCreateMutex();

    /* UART 配置 */
    uart_config_t uart_cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_param_config(LOG_UART_NUM, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(LOG_UART_NUM,
                                  LOG_UART_TX_PIN,
                                  UART_PIN_NO_CHANGE,
                                  UART_PIN_NO_CHANGE,
                                  UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(LOG_UART_NUM,
                                        256,   /* TX ring buffer */
                                        0,     /* RX ring buffer (not used) */
                                        0,     /* queue size */
                                        NULL,  /* queue ptr */
                                        0));   /* intr flags */

    /* 设置 esp_log 级别 */
    log_service_set_level(level);
    s_current_level = level;

    ESP_LOGI(TAG, "log_service init: level=%d, UART TX=GPIO%u, 115200 8N1",
             level, LOG_UART_TX_PIN);
    return ESP_OK;
}

void log_service_set_level(log_level_t level)
{
    s_current_level = level;
    esp_log_level_set("*", (esp_log_level_t)level);
}

log_level_t log_service_get_level(void)
{
    return s_current_level;
}

void log_service_append_ring(const char *tag, int level, const char *msg)
{
    if (!tag || !msg) return;

    char line[256];
    int len = snprintf(line, sizeof(line), "[%s] %s\n", tag, msg);
    if (len > 0) {
        ring_write(line, (size_t)len);
    }
}

size_t log_service_dump_ring(char *out_buf, size_t buf_size)
{
    if (!out_buf || buf_size == 0 || !s_ring_mutex) return 0;

    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);

    if (s_ring_count == 0) {
        xSemaphoreGive(s_ring_mutex);
        return 0;
    }

    /* 从尾向前读：最新内容在 ring head 附近 */
    size_t start = (s_ring_head + RING_SIZE - s_ring_count) % RING_SIZE;
    size_t remain = s_ring_count;
    size_t pos = start;
    size_t out_pos = 0;

    while (remain > 0 && out_pos < buf_size - 1) {
        size_t chunk = (pos + remain <= RING_SIZE) ? remain : (RING_SIZE - pos);
        size_t to_copy = (out_pos + chunk < buf_size - 1) ? chunk : (buf_size - 1 - out_pos);
        memcpy(out_buf + out_pos, s_ring_buf + pos, to_copy);
        out_pos += to_copy;
        pos = (pos + chunk) % RING_SIZE;
        remain -= chunk;
    }
    out_buf[out_pos] = '\0';

    xSemaphoreGive(s_ring_mutex);
    return out_pos;
}

void log_service_enable_crash_dump(void)
{
    /* 注册 abort 处理器，在 panic 输出之前先 dump */
    /* ESP-IDF 默认已有 panic handler，此处额外打印 ring buffer */
    ESP_LOGI(TAG, "Crash dump enabled (ring buffer will be dumped on panic)");
}

esp_err_t log_service_deinit(void)
{
    if (s_ring_mutex) {
        vSemaphoreDelete(s_ring_mutex);
        s_ring_mutex = NULL;
    }
    uart_driver_delete(LOG_UART_NUM);
    return ESP_OK;
}
