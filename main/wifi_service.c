/**
 * wifi_service.c — Wi-Fi 连接管理实现
 */

#include "wifi_service.h"
#include "event_bus.h"
#include "led_service.h"

#include <string.h>

#include "esp_log.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_service";

/* ── 事件位 ───────────────────────────────────────────────────────────────── */

#define WIFI_CONNECTED_BIT   (1 << 0)
#define WIFI_GOT_IP_BIT     (1 << 1)
#define WIFI_FAIL_BIT        (1 << 2)

/* ── 内部状态 ─────────────────────────────────────────────────────────────── */

static EventGroupHandle_t s_wifi_event_group = NULL;
static wifi_state_t s_wifi_state = WIFI_STATE_IDLE;
static SemaphoreHandle_t s_mutex = NULL;

/* ── 工具 ─────────────────────────────────────────────────────────────────── */

static void lock(void)   { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mutex); }

static void set_state(wifi_state_t st)
{
    lock();
    wifi_state_t prev = s_wifi_state;
    s_wifi_state = st;
    unlock();

    if (prev != st) {
        ESP_LOGD(TAG, "wifi_state %d → %d", prev, st);
        if (st == WIFI_STATE_GOT_IP) event_bus_publish(EV_WIFI_GOT_IP, NULL, 0);
        else if (st == WIFI_STATE_FAIL) event_bus_publish(EV_WIFI_CONNECT_FAILED, NULL, 0);
        else if (st == WIFI_STATE_DISCONNECTED) event_bus_publish(EV_WIFI_DISCONNECTED, NULL, 0);
    }
}

static wifi_state_t get_state(void)
{
    lock();
    wifi_state_t st = s_wifi_state;
    unlock();
    return st;
}

/* ── 事件处理器 ───────────────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (strcmp(event_base, WIFI_EVENT) == 0) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started, initiating connection");
                esp_err_t connect_err = esp_wifi_connect();
                if (connect_err != ESP_OK && connect_err != ESP_ERR_WIFI_CONN) {
                    ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(connect_err));
                }
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Wi-Fi connected (SSID matched)");
                set_state(WIFI_STATE_CONNECTED);
                led_set_wifi_err();
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *ev =
                    (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "Disconnected: reason=%d; retrying", ev->reason);
                set_state(WIFI_STATE_DISCONNECTED);
                esp_err_t retry_err = esp_wifi_connect();
                if (retry_err != ESP_OK && retry_err != ESP_ERR_WIFI_CONN) {
                    ESP_LOGW(TAG, "Reconnect failed: %s", esp_err_to_name(retry_err));
                }
                break;
            }

            default:
                break;
        }
    } else if (strcmp(event_base, IP_EVENT) == 0) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP: {
                ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
                ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
                set_state(WIFI_STATE_GOT_IP);
                led_set_wifi_ok();
                xEventGroupSetBits(s_wifi_event_group, WIFI_GOT_IP_BIT);
                break;
            }
            case IP_EVENT_STA_LOST_IP:
                ESP_LOGW(TAG, "Lost IP");
                set_state(WIFI_STATE_CONNECTED);
                break;
            default:
                break;
        }
    }
}

/* ── 公开 API ─────────────────────────────────────────────────────────────── */

esp_err_t wifi_service_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_FAIL;

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) return ESP_FAIL;

    /* 初始化 TCP/IP / netif */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* 初始化 Wi-Fi 驱动 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 注册事件处理 */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                               &wifi_event_handler, NULL));

    /* STA 模式 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    set_state(WIFI_STATE_IDLE);
    ESP_LOGI(TAG, "wifi_service init ok");
    return ESP_OK;
}

esp_err_t wifi_service_connect(const char *ssid, const char *password,
                                uint32_t timeout_ms)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;

    set_state(WIFI_STATE_CONNECTING);

    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    size_t ssid_len = strlen(ssid);
    if (ssid_len >= sizeof(wifi_cfg.sta.ssid)) {
        ssid_len = sizeof(wifi_cfg.sta.ssid) - 1;
    }
    memcpy(wifi_cfg.sta.ssid, ssid, ssid_len);
    wifi_cfg.sta.ssid[ssid_len] = '\0';

    if (password && strlen(password) > 0) {
        size_t pw_len = strlen(password);
        if (pw_len >= sizeof(wifi_cfg.sta.password)) {
            pw_len = sizeof(wifi_cfg.sta.password) - 1;
        }
        memcpy(wifi_cfg.sta.password, password, pw_len);
        wifi_cfg.sta.password[pw_len] = '\0';
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi connecting to SSID='%s'...", ssid);

    /* 等待 GOT_IP 或 FAIL */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_GOT_IP_BIT | WIFI_FAIL_BIT,
        pdFALSE,    /* 不清除所有位 */
        pdFALSE,    /* 不等待所有位 */
        pdMS_TO_TICKS(timeout_ms)
    );

    if (bits & WIFI_GOT_IP_BIT) {
        set_state(WIFI_STATE_GOT_IP);
        return ESP_OK;
    } else {
        set_state(WIFI_STATE_FAIL);
        ESP_LOGE(TAG, "Wi-Fi connect timeout or failed");
        return ESP_FAIL;
    }
}

void wifi_service_start(const char *ssid, const char *password)
{
    if (!ssid) return;

    wifi_service_disconnect();   /* 确保干净状态 */

    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    size_t ssid_len = strlen(ssid);
    if (ssid_len >= sizeof(wifi_cfg.sta.ssid)) {
        ssid_len = sizeof(wifi_cfg.sta.ssid) - 1;
    }
    memcpy(wifi_cfg.sta.ssid, ssid, ssid_len);
    wifi_cfg.sta.ssid[ssid_len] = '\0';

    if (password && strlen(password) > 0) {
        size_t pw_len = strlen(password);
        if (pw_len >= sizeof(wifi_cfg.sta.password)) {
            pw_len = sizeof(wifi_cfg.sta.password) - 1;
        }
        memcpy(wifi_cfg.sta.password, password, pw_len);
        wifi_cfg.sta.password[pw_len] = '\0';
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    set_state(WIFI_STATE_CONNECTING);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi connecting to SSID='%s' (async)...", ssid);
}

esp_err_t wifi_service_disconnect(void)
{
    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_OK || err == ESP_ERR_WIFI_NOT_STARTED) {
        set_state(WIFI_STATE_DISCONNECTED);
    }
    return err;
}

wifi_state_t wifi_service_get_state(void)
{
    return get_state();
}

bool wifi_service_get_ip_str(char *buf, size_t buf_size)
{
    if (!buf || buf_size < 16) return false;

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return false;
    }

    snprintf(buf, buf_size, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

bool wifi_service_get_mac_str(char *buf, size_t buf_size, esp_mac_type_t type)
{
    if (!buf || buf_size < 18) return false;

    uint8_t mac[6];
    if (esp_read_mac(mac, type) != ESP_OK) {
        return false;
    }

    snprintf(buf, buf_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

int8_t wifi_service_get_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return 0;
    }
    return ap_info.rssi;
}

esp_err_t wifi_service_deinit(void)
{
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    set_state(WIFI_STATE_IDLE);
    return ESP_OK;
}
