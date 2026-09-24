/**
 * cfg_service.h — NVS 配置管理
 *
 * 职责
 * - 键值存储（Wi-Fi SSID/Password、ACOS Token/Device ID、运行参数）
 * - 首次配网（AP 模式提供 Web 配网页面，由 app_main 启动内置 HTTP server）
 * - 默认值兜底
 *
 * 存储后端：NVS（ESP-IDF nvs_flash）
 *
 * 依赖
 *   - nvs_flash.h
 */

#ifndef CFG_SERVICE_H
#define CFG_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 配置键名 ────────────────────────────────────────────────────────────── */

typedef enum {
    /* Wi-Fi */
    CFG_KEY_WIFI_SSID     = 0,
    CFG_KEY_WIFI_PASSWORD,
    /* ACOS */
    CFG_KEY_ACOS_TOKEN,
    CFG_KEY_WS_URL,
    CFG_KEY_DEVICE_ID,
    CFG_KEY_ROOM_ID,
    CFG_KEY_MODEL,
    /* 运行参数 */
    CFG_KEY_VAD_ENABLE,
    CFG_KEY_BARGE_IN_ENABLE,
    CFG_KEY_LED_MODE,
    CFG_KEY_VOLUME,
    /* 系统 */
    CFG_KEY_PAIRED,
    CFG_KEY_LAST_ERROR,
    CFG_KEY_MAX,
} cfg_key_t;

/* ── LED 模式 ────────────────────────────────────────────────────────────── */

typedef enum {
    LED_MODE_NORMAL = 0,
    LED_MODE_NIGHT  = 1,   /* 低亮度 */
    LED_MODE_OFF    = 2,   /* 关闭 LED */
} led_mode_t;

/* ── 公开 API ─────────────────────────────────────────────────────────────── */

/**
 * @brief 初始化 NVS
 *
 * 必须在所有其他服务之前调用。
 */
esp_err_t cfg_service_init(void);

/**
 * @brief 读取字符串配置
 *
 * @param key       配置键
 * @param out       输出缓冲区
 * @param out_size  缓冲区大小
 * @param def       默认值（当 key 不存在时使用）
 * @return          实际写入 out 的字符数（不含 '\0'）
 */
size_t cfg_service_get_str(cfg_key_t key, char *out, size_t out_size,
                            const char *def);

/**
 * @brief 写入字符串配置
 */
esp_err_t cfg_service_set_str(cfg_key_t key, const char *value);

/**
 * @brief 读取整数配置
 */
int cfg_service_get_int(cfg_key_t key, int def);

/**
 * @brief 写入整数配置
 */
esp_err_t cfg_service_set_int(cfg_key_t key, int value);

/**
 * @brief 读取布尔配置
 */
bool cfg_service_get_bool(cfg_key_t key, bool def);

/**
 * @brief 写入布尔配置
 */
esp_err_t cfg_service_set_bool(cfg_key_t key, bool value);

/**
 * @brief 删除指定配置
 */
esp_err_t cfg_service_erase(cfg_key_t key);

/**
 * @brief 清除所有配置（NVS 全擦）
 */
esp_err_t cfg_service_erase_all(void);

/**
 * @brief 检查是否已完成首次配网
 */
bool cfg_service_is_paired(void);

/**
 * @brief 标记配网完成
 */
esp_err_t cfg_service_mark_paired(void);

/**
 * @brief 获取 Wi-Fi 凭据（优先 NVS，无则用编译期默认值）
 *
 * @param ssid_out   输出 SSID 缓冲区
 * @param ssid_size  缓冲区大小
 * @param pass_out   输出密码缓冲区（可为 NULL）
 * @param pass_size  密码缓冲区大小
 */
void cfg_get_wifi_credential(char *ssid_out, size_t ssid_size,
                             char *pass_out, size_t pass_size);

/**
 * @brief 获取 ACOS 服务器 host + token
 *
 * host 从 ws_url NVS 字段解析（去掉 wss:// 前缀和路径）
 * token 直接读 NVS，无则返回空字符串。
 *
 * @param host_out   输出 host 缓冲区
 * @param host_size  host 缓冲区大小
 * @param token_out  输出 token 缓冲区
 * @param token_size token 缓冲区大小
 */
void cfg_get_acos_server(char *host_out, size_t host_size,
                         char *token_out, size_t token_size);

/**
 * @brief 将键名转换为字符串（用于日志）
 */
const char *cfg_key_to_str(cfg_key_t key);

/**
 * @brief 反初始化
 */
esp_err_t cfg_service_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* CFG_SERVICE_H */
