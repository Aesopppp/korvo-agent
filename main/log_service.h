/**
 * log_service.h — 日志服务（UART / SD 卡可选输出）
 *
 * 职责
 * - 重定向 ESP-IDF esp_log 到 UART0（115200 8N1）
 * - 可选写日志到 SD 卡（SDMMC）
 * - 崩溃寄存器dump
 * - Ring-buffer 循环日志（用于远程诊断抓取）
 *
 * 依赖
 *   - vfs_uart.c (ESP-IDF 内置)
 *   - event_bus.h（用于发布崩溃事件）
 */

#ifndef LOG_SERVICE_H
#define LOG_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 日志级别阈值 ─────────────────────────────────────────────────────────── */

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_NONE  = 4,
} log_level_t;

/* ── 公开 API ─────────────────────────────────────────────────────────────── */

/**
 * @brief 初始化日志服务
 *
 * - 配置 UART0：115200 8N1（TX only, TX=GPIO43）
 * - 设置 esp_log 级别阈值
 *
 * @param level  日志级别下限（低于此级别的不输出）
 */
esp_err_t log_service_init(log_level_t level);

/**
 * @brief 设置全局日志级别
 */
void log_service_set_level(log_level_t level);

/**
 * @brief 获取当前日志级别
 */
log_level_t log_service_get_level(void);

/**
 * @brief 追加一列日志到 ring buffer（用于诊断抓取）
 *
 * @param tag     日志标签
 * @param level   级别
 * @param msg     日志内容
 */
void log_service_append_ring(const char *tag, int level, const char *msg);

/**
 * @brief 导出 ring buffer 日志到缓冲区
 *
 * @param out_buf     输出缓冲区
 * @param buf_size    缓冲区大小
 * @return            实际写入的字节数
 */
size_t log_service_dump_ring(char *out_buf, size_t buf_size);

/**
 * @brief 初始化崩溃处理器（调用 abort() 前会 dump 寄存器）
 */
void log_service_enable_crash_dump(void);

/**
 * @brief 反初始化
 */
esp_err_t log_service_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* LOG_SERVICE_H */
