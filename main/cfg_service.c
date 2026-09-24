/**
 * cfg_service.c — NVS 配置管理实现
 */

#include "cfg_service.h"
#include "board_config.h"
#include "event_bus.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg_service";

/* ── 内部常量 ─────────────────────────────────────────────────────────────── */

#define NVS_PARTITION     "nvs"
#define NVS_NAMESPACE     "korvo_cfg"
#define MAX_STR_LEN       256

/* ── 键名字符串映射表 ─────────────────────────────────────────────────────── */

static const char *s_key_names[] = {
    "wifi_ssid",
    "wifi_pass",
    "acos_token",
    "ws_url",
    "device_id",
    "room_id",
    "model",
    "vad_enable",
    "barge_in_enable",
    "led_mode",
    "volume",
    "paired",
    "last_err",
    "",
};

const char *cfg_key_to_str(cfg_key_t key)
{
    if (key >= 0 && key < CFG_KEY_MAX) {
        return s_key_names[key];
    }
    return "unknown";
}

/* ── NVS 句柄 ─────────────────────────────────────────────────────────────── */

static nvs_handle_t s_nvs_handle = 0;
static bool s_inited = false;

/* ── 内部工具 ─────────────────────────────────────────────────────────────── */

static esp_err_t ensure_open(void)
{
    if (s_inited) return ESP_OK;
    return nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
}

/* ── 公开 API ─────────────────────────────────────────────────────────────── */

esp_err_t cfg_service_init(void)
{
    esp_err_t err = nvs_flash_init_partition(NVS_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, re-initializing...");
        ESP_ERROR_CHECK(nvs_flash_erase_partition(NVS_PARTITION));
        err = nvs_flash_init_partition(NVS_PARTITION);
    }
    ESP_ERROR_CHECK(err);

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 首次启动：用编译期默认值填充空白配置 */
    s_inited = true;
    char tmp[256] = {0};
    if (nvs_get_str(s_nvs_handle, "wifi_ssid", tmp, &(size_t){sizeof(tmp)}) == ESP_ERR_NVS_NOT_FOUND) {
        cfg_service_set_str(CFG_KEY_WIFI_SSID, DEFAULT_WIFI_SSID);
        cfg_service_set_str(CFG_KEY_WIFI_PASSWORD, DEFAULT_WIFI_PASSWORD);
        ESP_LOGI(TAG, "NVS: WiFi defaults written (SSID='%s')", DEFAULT_WIFI_SSID);
    }
    if (nvs_get_str(s_nvs_handle, "acos_token", tmp, &(size_t){sizeof(tmp)}) == ESP_ERR_NVS_NOT_FOUND) {
        cfg_service_set_str(CFG_KEY_ACOS_TOKEN, DEFAULT_ACOS_TOKEN);
        ESP_LOGI(TAG, "NVS: ACOS token default written");
    }


    ESP_LOGI(TAG, "cfg_service init ok (partition='%s')", NVS_PARTITION);
    return ESP_OK;
}

size_t cfg_service_get_str(cfg_key_t key, char *out, size_t out_size, const char *def)
{
    if (!out || out_size == 0) return 0;
    out[0] = '\0';

    if (!s_inited) return 0;

    const char *nvs_key = cfg_key_to_str(key);
    size_t required_size = out_size;

    esp_err_t err = nvs_get_str(s_nvs_handle, nvs_key, out, &required_size);
    if (err == ESP_OK) {
        return required_size > 0 ? required_size - 1 : 0;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (def) {
            strncpy(out, def, out_size - 1);
            out[out_size - 1] = '\0';
            return strlen(out);
        }
    }
    (void)err;
    return 0;
}

esp_err_t cfg_service_set_str(cfg_key_t key, const char *value)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!value) return ESP_ERR_INVALID_ARG;

    const char *nvs_key = cfg_key_to_str(key);
    esp_err_t err = nvs_set_str(s_nvs_handle, nvs_key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str('%s') failed: %s", nvs_key, esp_err_to_name(err));
        return err;
    }

    err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(TAG, "cfg '%s' = '%s'", nvs_key, value);
    return ESP_OK;
}

int cfg_service_get_int(cfg_key_t key, int def)
{
    if (!s_inited) return def;

    const char *nvs_key = cfg_key_to_str(key);
    int32_t val = def;

    esp_err_t err = nvs_get_i32(s_nvs_handle, nvs_key, &val);
    if (err == ESP_OK) {
        return (int)val;
    }
    return def;
}

esp_err_t cfg_service_set_int(cfg_key_t key, int value)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    const char *nvs_key = cfg_key_to_str(key);
    esp_err_t err = nvs_set_i32(s_nvs_handle, nvs_key, (int32_t)value);
    if (err != ESP_OK) return err;

    return nvs_commit(s_nvs_handle);
}

bool cfg_service_get_bool(cfg_key_t key, bool def)
{
    if (!s_inited) return def;

    const char *nvs_key = cfg_key_to_str(key);
    int32_t val = def ? 1 : 0;

    esp_err_t err = nvs_get_i32(s_nvs_handle, nvs_key, &val);
    if (err == ESP_OK) {
        return val != 0;
    }
    return def;
}

esp_err_t cfg_service_set_bool(cfg_key_t key, bool value)
{
    return cfg_service_set_int(key, value ? 1 : 0);
}

esp_err_t cfg_service_erase(cfg_key_t key)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    const char *nvs_key = cfg_key_to_str(key);
    esp_err_t err = nvs_erase_key(s_nvs_handle, nvs_key);
    if (err != ESP_OK) return err;

    return nvs_commit(s_nvs_handle);
}

esp_err_t cfg_service_erase_all(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    esp_err_t err = nvs_erase_all(s_nvs_handle);
    if (err != ESP_OK) return err;

    return nvs_commit(s_nvs_handle);
}

bool cfg_service_is_paired(void)
{
    return cfg_service_get_bool(CFG_KEY_PAIRED, false);
}

esp_err_t cfg_service_mark_paired(void)
{
    ESP_LOGI(TAG, "Marking device as paired");
    return cfg_service_set_bool(CFG_KEY_PAIRED, true);
}

/* ── Wi-Fi 凭据（NVS + 编译期默认值兜底） ─────────────────────────────────── */

void cfg_get_wifi_credential(char *ssid_out, size_t ssid_size,
                             char *pass_out, size_t pass_size)
{
    if (ssid_out && ssid_size > 0) {
        cfg_service_get_str(CFG_KEY_WIFI_SSID, ssid_out, ssid_size,
                           DEFAULT_WIFI_SSID);
    }
    if (pass_out && pass_size > 0) {
        cfg_service_get_str(CFG_KEY_WIFI_PASSWORD, pass_out, pass_size,
                           DEFAULT_WIFI_PASSWORD);
    }
}

/* ── ACOS 服务器信息（host 从 ws_url 解析） ───────────────────────────────── */

void cfg_get_acos_server(char *host_out, size_t host_size,
                         char *token_out, size_t token_size)
{
    /* 优先用 ws_url，否则用编译期默认值 */
    char url_buf[256] = {0};
    cfg_service_get_str(CFG_KEY_WS_URL, url_buf, sizeof(url_buf), ACOS_WS_URL);

    /* 解析 wss://host[:port]/path → host 部分 */
    if (host_out && host_size > 0) {
        const char *p = url_buf;
        /* 跳过协议前缀 */
        if (strncmp(p, "wss://", 6) == 0)      p += 6;
        else if (strncmp(p, "ws://", 5) == 0)  p += 5;

        /* 找到第一个 / 或 : 或字符串末尾即为 host 结束 */
        const char *end = p;
        while (*end && *end != '/' && *end != ':') end++;

        size_t len = (size_t)(end - p);
        if (len >= host_size) len = host_size - 1;
        memcpy(host_out, p, len);
        host_out[len] = '\0';
    }

    /* Token 直接读 NVS */
    if (token_out && token_size > 0) {
        cfg_service_get_str(CFG_KEY_ACOS_TOKEN, token_out, token_size, DEFAULT_ACOS_TOKEN);
    }
}

esp_err_t cfg_service_deinit(void)
{
    if (s_inited) {
        nvs_close(s_nvs_handle);
        s_inited = false;
    }
    return ESP_OK;
}
