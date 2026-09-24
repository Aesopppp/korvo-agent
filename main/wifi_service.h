/**
 * wifi_service.h — Wi-Fi 连接管理
 *
 * 职责
 * - STA 模式连接（首次配网 / 自动重连）
 * - 获取本地 IP / MAC 地址
 * - 发布 Wi-Fi 状态事件
 *
 * 依赖
 *   - esp_wifi (ESP-IDF)
 *   - event_bus.h
 */

#ifndef WIFI_SERVICE_H
#define WIFI_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_mac.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Wi-Fi 连接状态 ──────────────────────────────────────────────────────── */

typedef enum {
    WIFI_STATE_IDLE        = 0,
    WIFI_STATE_CONNECTING  = 1,
    WIFI_STATE_CONNECTED   = 2,
    WIFI_STATE_GOT_IP      = 3,
    WIFI_STATE_DISCONNECTED = 4,
    WIFI_STATE_FAIL        = 5,
} wifi_state_t;

/* ── 事件类型 ────────────────────────────────────────────────────────────── */

typedef enum {
    WIFI_EVENT_CONNECTED,
    WIFI_EVENT_GOT_IP,
    WIFI_EVENT_DISCONNECTED,
    WIFI_EVENT_FAIL,
} wifi_event_type_t;

/* ── 公开 API ─────────────────────────────────────────────────────────────── */

/**
 * @brief 初始化 Wi-Fi STA（必须先于其他服务调用）
 */
esp_err_t wifi_service_init(void);

/**
 * @brief 启动连接（同步等待 GOT_IP）
 *
 * @param ssid      SSID
 * @param password  密码（可为空字符串表示开放网络）
 * @param timeout_ms 连接超时（毫秒）
 *
 * @return ESP_OK   连接成功并获得 IP
 *         ESP_TIMEOUT 超时
 *         ESP_FAIL  连接失败
 */
esp_err_t wifi_service_connect(const char *ssid, const char *password,
                                uint32_t timeout_ms);

/**
 * @brief 断开 Wi-Fi
 */
esp_err_t wifi_service_disconnect(void);

/**
 * @brief 获取当前 Wi-Fi 状态
 */
wifi_state_t wifi_service_get_state(void);

/**
 * @brief 获取本地 IP 地址字符串（IPv4）
 *
 * @param buf       输出缓冲区
 * @param buf_size  缓冲区大小（建议 >= 16）
 * @return true     成功
 */
bool wifi_service_get_ip_str(char *buf, size_t buf_size);

/**
 * @brief 获取 MAC 地址字符串
 *
 * @param buf       输出缓冲区
 * @param buf_size  缓冲区大小（建议 >= 18）
 * @param type      ESP_MAC_WIFI_STA / ESP_MAC_WIFI_AP / ESP_MAC_BT 等
 */
bool wifi_service_get_mac_str(char *buf, size_t buf_size, esp_mac_type_t type);

/**
 * @brief 获取信号强度（dBm）
 *
 * @return rssi dBm 值，0 表示未连接
 */
int8_t wifi_service_get_rssi(void);

/**
 * @brief 反初始化
 */
esp_err_t wifi_service_deinit(void);

/**
 * @brief 启动 Wi-Fi 连接（异步，无超时；连接状态通过 event_bus 事件通知）
 *
 * 适合在 app_main / wifi_task 中调用，无需同步等待结果。
 * 连接成功/失败/GOT_IP/DISCONNECTED 事件通过 event_bus 发布。
 *
 * @param ssid      SSID
 * @param password  密码（可为空字符串表示开放网络）
 */
void wifi_service_start(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_SERVICE_H */
