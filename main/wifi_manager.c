/*
 * WiFi 管理模块实现
 * 功能：状态机驱动的 WiFi AP/STA 管理，自动重连，回调通知
 */

#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "wifi_manager";

// --- Static state ---
static wifi_state_t s_state = WIFI_STATE_AP;
static char s_ip_str[16] = "0.0.0.0";
static wifi_state_callback_t s_callback = NULL;
static void *s_user_data = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

// Event handler instances (needed for unregister)
static esp_event_handler_instance_t s_wifi_handler = NULL;
static esp_event_handler_instance_t s_ip_handler = NULL;

// STA reconnect state
static int s_retry_count = 0;
#define MAX_RETRY 5
#define RETRY_DELAY_S 5

// --- Forward declarations ---
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void notify_state(wifi_state_t new_state);

// --- Helper ---
static void set_state(wifi_state_t new_state)
{
    if (s_state != new_state) {
        ESP_LOGI(TAG, "State: %d -> %d", s_state, new_state);
        s_state = new_state;
        notify_state(new_state);
    }
}

static void notify_state(wifi_state_t new_state)
{
    if (s_callback) {
        s_callback(new_state, s_user_data);
    }
}

// --- Event handler ---
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started, connecting...");
            set_state(WIFI_STATE_STA_CONNECTING);
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA connected to AP, waiting for IP...");
            set_state(WIFI_STATE_STA_CONNECTING);
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            s_retry_count++;
            if (s_retry_count < MAX_RETRY) {
                ESP_LOGW(TAG, "STA disconnected, retry %d/%d in %ds",
                         s_retry_count, MAX_RETRY, RETRY_DELAY_S);
                set_state(WIFI_STATE_STA_DISCONNECTED);
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_S * 1000));
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "STA disconnected, max retries (%d) reached, giving up", MAX_RETRY);
                set_state(WIFI_STATE_STA_DISCONNECTED);
            }
            break;
        }

        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP started (SSID: MiBeeCam)");
            set_state(WIFI_STATE_AP);
            break;

        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "AP stopped");
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "STA got IP: %s", s_ip_str);
            s_retry_count = 0;
            set_state(WIFI_STATE_STA_CONNECTED);
        }
    }
}

// --- Public API ---

esp_err_t wifi_init(void)
{
    esp_err_t ret;

    ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        ESP_LOGE(TAG, "Failed to create AP netif");
        return ESP_FAIL;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "Failed to create STA netif");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, &s_wifi_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, NULL, &s_ip_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    s_state = WIFI_STATE_AP;
    s_retry_count = 0;
    memset(s_ip_str, 0, sizeof(s_ip_str));
    strcpy(s_ip_str, "0.0.0.0");

    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_start_ap(void)
{
    esp_err_t ret;

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "MiBeeCam",
            .ssid_len = 8,
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };

    ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AP mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AP config: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start AP: %s", esp_err_to_name(ret));
        return ret;
    }

    // State set by WIFI_EVENT_AP_START handler
    ESP_LOGI(TAG, "AP starting: SSID=MiBeeCam");
    return ESP_OK;
}

esp_err_t wifi_start_sta(const char *ssid, const char *pass)
{
    if (!ssid || !pass) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "",
            .password = "",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);

    ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set STA mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set STA config: %s", esp_err_to_name(ret));
        return ret;
    }

    s_retry_count = 0;
    memset(s_ip_str, 0, sizeof(s_ip_str));
    strcpy(s_ip_str, "0.0.0.0");

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start STA: %s", esp_err_to_name(ret));
        return ret;
    }

    // State set by WIFI_EVENT_STA_START handler -> CONNECTING
    ESP_LOGI(TAG, "STA starting, connecting to %s", ssid);
    return ESP_OK;
}

wifi_state_t wifi_get_state(void)
{
    return s_state;
}

const char *wifi_get_ip_str(void)
{
    return s_ip_str;
}

esp_err_t wifi_register_callback(wifi_state_callback_t cb, void *user_data)
{
    s_callback = cb;
    s_user_data = user_data;
    return ESP_OK;
}

esp_err_t wifi_stop(void)
{
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "Failed to stop WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    memset(s_ip_str, 0, sizeof(s_ip_str));
    strcpy(s_ip_str, "0.0.0.0");
    s_retry_count = 0;
    set_state(WIFI_STATE_AP);

    ESP_LOGI(TAG, "WiFi stopped");
    return ESP_OK;
}

esp_err_t wifi_manager_deinit(void)
{
    esp_err_t ret;

    // Unregister event handlers
    if (s_wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler = NULL;
    }
    if (s_ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_handler = NULL;
    }

    ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop during deinit: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_deinit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Destroy netifs
    if (s_sta_netif) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }

    ret = esp_event_loop_delete_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "event loop delete: %s", esp_err_to_name(ret));
    }

    s_callback = NULL;
    s_user_data = NULL;
    s_state = WIFI_STATE_AP;
    s_retry_count = 0;
    memset(s_ip_str, 0, sizeof(s_ip_str));
    strcpy(s_ip_str, "0.0.0.0");

    ESP_LOGI(TAG, "WiFi manager deinitialized");
    return ESP_OK;
}
