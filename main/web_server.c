/*
 * Web Server Module Implementation
 * REST API endpoints + SPIFFS static file serving for ESP32-S3-A10 camera.
 *
 * Endpoints:
 *   GET  /api/status       — device status JSON
 *   GET  /api/config       — current config JSON
 *   POST /api/config       — partial config update (auth required)
 *   GET  /api/capabilities — board capability flags
 *   GET  /api/capture      — single JPEG frame
 *   GET  /api/scan         — WiFi AP scan
 *   POST /api/reset        — reset config to defaults (auth required)
 *   POST /api/reboot       — reboot device (auth required)
 *   GET  /metrics          — Prometheus-format metrics
 *   OPTIONS any-path     — CORS preflight
 *   GET    any-path     — SPIFFS static files
 */

#include "web_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "camera_driver.h"
#include "mjpeg_streamer.h"
#include "motion_detect.h"
#include "lwip/sockets.h"  /* setsockopt / TCP_NODELAY in on_session_open */
#include "event_bus.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

#include "health_monitor.h"

#ifdef CONFIG_MIBEECAM_ENABLE_WS
#define WS_MAX_CLIENTS 5
#define WS_IDLE_TIMEOUT_S 60

typedef struct {
    int sockfd;
    httpd_handle_t server;
    int64_t last_active;
} ws_client_t;

static ws_client_t s_ws_clients[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_ws_mutex = NULL;

static void ws_clients_init(void) {
    if (s_ws_mutex == NULL) {
        s_ws_mutex = xSemaphoreCreateMutex();
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        s_ws_clients[i].sockfd = -1;
        s_ws_clients[i].server = NULL;
        s_ws_clients[i].last_active = 0;
    }
}

static int ws_add_client(int sockfd, httpd_handle_t server) {
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].sockfd == -1) {
            s_ws_clients[i].sockfd = sockfd;
            s_ws_clients[i].server = server;
            s_ws_clients[i].last_active = esp_timer_get_time();
            xSemaphoreGive(s_ws_mutex);
            return i;
        }
    }
    xSemaphoreGive(s_ws_mutex);
    return -1;
}

static void ws_remove_client(int sockfd) {
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].sockfd == sockfd) {
            s_ws_clients[i].sockfd = -1;
            s_ws_clients[i].server = NULL;
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

static void ws_touch_client(int sockfd) {
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].sockfd == sockfd) {
            s_ws_clients[i].last_active = esp_timer_get_time();
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}
#endif
/* ------------------------------------------------------------------ */
/*  JSON / HTTP helpers                                                */
/* ------------------------------------------------------------------ */

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-Password");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
}

static esp_err_t json_ok(httpd_req_t *req, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "JSON alloc failed", 17);
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    if (data) {
        cJSON_AddItemToObject(root, "data", data);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "JSON print failed", 17);
        return ESP_FAIL;
    }
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t json_error(httpd_req_t *req, const char *msg, int status)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "JSON alloc failed", 17);
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", msg);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "JSON print failed", 17);
        return ESP_FAIL;
    }
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, (status == HTTPD_401_UNAUTHORIZED) ? "401 Unauthorized" :
                           (status == HTTPD_404_NOT_FOUND) ? "404 Not Found" :
                           (status == HTTPD_500_INTERNAL_SERVER_ERROR) ? "500 Internal Server Error" :
                           "400 Bad Request");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_FAIL;
}

/**
 * @brief Read the full request body into a malloc'd buffer.
 * @param req   HTTP request.
 * @param out   Output pointer (caller must free).
 * @param out_len  Output length.
 * @return ESP_OK on success.
 */
static esp_err_t read_body(httpd_req_t *req, char **out, int *out_len)
{
    int total = req->content_len;
    if (total <= 0) {
        *out = NULL;
        *out_len = 0;
        return ESP_OK;
    }

    char *buf = malloc(total + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    buf[total] = '\0';

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += ret;
    }

    *out = buf;
    *out_len = total;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Authentication helpers                                             */
/* ------------------------------------------------------------------ */


static bool check_auth(httpd_req_t *req)
{
    const char *stored_pass = config_get_web_password();
    /* If no password is set, allow access (first-time setup) */
    if (!stored_pass || stored_pass[0] == '\0') {
        return true;
    }

    /* Check X-Password header */
    char password[128];
    size_t password_len = sizeof(password) - 1;
    esp_err_t ret = httpd_req_get_hdr_value_str(req, "X-Password", password, password_len);
    if (ret != ESP_OK) {
        return false;
    }
    password[password_len] = '\0';

    return strcmp(password, stored_pass) == 0;
}

static esp_err_t require_auth(httpd_req_t *req)
{
    const char *stored_pass = config_get_web_password();

    /* State A: No password set — only POST /api/config with web_password field allowed */
    if (!stored_pass || stored_pass[0] == '\0') {
        return json_error(req, "SET_PASSWORD_FIRST", HTTPD_401_UNAUTHORIZED);
    }

    /* State B: Password is set — require X-Password header to match */
    char password[128];
    size_t password_len = sizeof(password) - 1;
    esp_err_t ret = httpd_req_get_hdr_value_str(req, "X-Password", password, password_len);
    if (ret != ESP_OK) {
        return json_error(req, "unauthorized", HTTPD_401_UNAUTHORIZED);
    }
    password[password_len] = '\0';

    if (strcmp(password, stored_pass) != 0) {
        return json_error(req, "unauthorized", HTTPD_401_UNAUTHORIZED);
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Resolution helper                                                  */
/* ------------------------------------------------------------------ */

/* 家族统一刻度（契约 v1.3 §5）：value = framesize_t（10-15） */
static const char *res_to_str(uint8_t res)
{
    switch (res) {
        case 10: return "VGA";
        case 11: return "SVGA";
        case 12: return "XGA";
        case 13: return "HD";
        case 14: return "SXGA";
        case 15: return "UXGA";
        default: return "Unknown";
    }
}

/* ------------------------------------------------------------------ */
/*  GET /api/status                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t handler_api_status(httpd_req_t *req)
{
    const cam_config_t *cfg = config_get();
    wifi_state_t ws = wifi_get_state();

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device_name",
        cfg->device_name[0] ? cfg->device_name : "MiBeeCam");
    cJSON_AddStringToObject(data, "wifi_ssid", cfg->wifi_ssid);

    const char *state_str = "unknown";
    switch (ws) {
        case WIFI_STATE_AP:              state_str = "ap"; break;
        case WIFI_STATE_STA_CONNECTING:  state_str = "connecting"; break;
        case WIFI_STATE_STA_CONNECTED:   state_str = "connected"; break;
        case WIFI_STATE_STA_DISCONNECTED: state_str = "disconnected"; break;
        case WIFI_STATE_STA_FAILED:      state_str = "failed"; break;
    }
    cJSON_AddStringToObject(data, "wifi_state", state_str);
    cJSON_AddStringToObject(data, "ip", wifi_get_ip_str());

    if (ws == WIFI_STATE_STA_CONNECTED) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            cJSON_AddNumberToObject(data, "wifi_rssi", ap_info.rssi);
            cJSON_AddNumberToObject(data, "wifi_channel", ap_info.primary);
            /* 当前实连 SSID（区别于上面的配置值 wifi_ssid）— 契约字段，SPA 顶栏/WiFi 页用 */
            cJSON_AddStringToObject(data, "current_ssid", (const char *)ap_info.ssid);
        }
    } else {
        cJSON_AddStringToObject(data, "current_ssid", "");
    }

#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
    cJSON_AddNumberToObject(data, "active_ssid_index", wifi_get_current_ssid_index());
    cJSON_AddStringToObject(data, "wifi_net",
        wifi_get_current_ssid_index() ? "secondary" : "primary");
#endif

    cJSON_AddStringToObject(data, "camera", camera_get_sensor_name());
    cJSON_AddStringToObject(data, "resolution", res_to_str(cfg->cam_framesize));

    cJSON_AddNumberToObject(data, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(data, "firmware_version", "0.3.0");

    float temp = get_chip_temp();
    cJSON_AddNumberToObject(data, "chip_temp", temp);

    size_t baseline_free = 0, baseline_min = 0;
    health_get_baselines(&baseline_free, &baseline_min);
    size_t current_free = esp_get_free_heap_size();
    size_t current_min = esp_get_minimum_free_heap_size();
    int heap_delta = (int)current_free - (int)baseline_free;
    /* 契约 v1.0 字段名: free_heap / min_heap（heap_baseline/heap_delta 为本板扩展） */
    cJSON_AddNumberToObject(data, "free_heap", current_free);
    cJSON_AddNumberToObject(data, "min_heap", current_min);
    cJSON_AddNumberToObject(data, "heap_baseline", baseline_free);
    cJSON_AddNumberToObject(data, "heap_delta", heap_delta);

    cJSON_AddNumberToObject(data, "stream_clients", mjpeg_streamer_get_client_count());
    cJSON_AddNumberToObject(data, "stream_clients_max", 1);  /* 2026-09-03: 硬单流（DRAM 限制），与 mjpeg_streamer MAX_STREAM_CLIENTS 同步改 */

    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  GET /api/config                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t handler_api_config_get(httpd_req_t *req)
{
    const cam_config_t *cfg = config_get();

    /* 契约字段名（config-contract §3）；敏感字段掩码 "****"（§3 敏感清单） */
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device_name", cfg->device_name);
    cJSON_AddStringToObject(data, "wifi_ssid", cfg->wifi_ssid);
    cJSON_AddStringToObject(data, "wifi_pass", cfg->wifi_pass[0] ? "****" : "");
    cJSON_AddStringToObject(data, "wifi_ssid_2", cfg->wifi_ssid_2);
    cJSON_AddStringToObject(data, "wifi_pass_2", cfg->wifi_pass_2[0] ? "****" : "");
    cJSON_AddStringToObject(data, "timezone", cfg->timezone);
    cJSON_AddStringToObject(data, "web_password", cfg->web_password[0] ? "****" : "");
    cJSON_AddNumberToObject(data, "cam_framesize", (double)cfg->cam_framesize);
    cJSON_AddNumberToObject(data, "cam_fps", (double)cfg->cam_fps);
    cJSON_AddNumberToObject(data, "cam_quality", (double)cfg->cam_quality);
    cJSON_AddNumberToObject(data, "cam_vflip", (double)cfg->cam_vflip);
    cJSON_AddNumberToObject(data, "cam_hmirror", (double)cfg->cam_hmirror);
    cJSON_AddNumberToObject(data, "xclk_freq_mhz", (double)cfg->xclk_freq_mhz);
    cJSON_AddNumberToObject(data, "onvif_enable", (double)cfg->onvif_enable);
    /* motion 家族超集（契约 §3.2）*/
    cJSON_AddNumberToObject(data, "motion_enabled", (double)cfg->motion_enabled);
    cJSON_AddNumberToObject(data, "motion_sensitivity", (double)cfg->motion_sensitivity);
    cJSON_AddNumberToObject(data, "motion_cooldown_s", (double)cfg->motion_cooldown_s);
    cJSON_AddNumberToObject(data, "motion_active_interval_s", (double)cfg->motion_active_interval_s);
    /* webhook 组（契约 §3.2）*/
    cJSON_AddNumberToObject(data, "alert_webhook_enabled", (double)cfg->alert_webhook_enabled);
    cJSON_AddStringToObject(data, "alert_webhook_url", cfg->alert_webhook_url);
    cJSON_AddStringToObject(data, "webhook_secret", cfg->webhook_secret[0] ? "****" : "");
    cJSON_AddNumberToObject(data, "ws_enable", (double)cfg->ws_enable);
    /* 板级扩展（契约 §7：server_url 可选）*/
    cJSON_AddStringToObject(data, "server_url", cfg->server_url);
    cJSON_AddStringToObject(data, "mdns_hostname", cfg->mdns_hostname);
    cJSON_AddNumberToObject(data, "schema_version", (double)CONFIG_SCHEMA_VERSION);

    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  POST /api/config                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t handler_api_config_post(httpd_req_t *req)
{
    char *body = NULL;
    int body_len = 0;
    if (read_body(req, &body, &body_len) != ESP_OK) {
        return json_error(req, "Failed to read body", HTTPD_400_BAD_REQUEST);
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return json_error(req, "Invalid JSON", HTTPD_400_BAD_REQUEST);
    }

    /* Auth state machine */
    const char *stored_pass = config_get_web_password();
    bool password_empty = !stored_pass || stored_pass[0] == '\0';

    if (password_empty) {
        /* State A: only allow if body contains web_password field */
        cJSON *pw = cJSON_GetObjectItem(root, "web_password");
        if (!pw || !cJSON_IsString(pw) || !pw->valuestring[0]) {
            cJSON_Delete(root);
            return json_error(req, "SET_PASSWORD_FIRST", HTTPD_401_UNAUTHORIZED);
        }
        /* Fall through — config_save will save the password */
    } else {
        /* State B: require X-Password header */
        if (!check_auth(req)) {
            cJSON_Delete(root);
            return json_error(req, "unauthorized", HTTPD_401_UNAUTHORIZED);
        }
    }

    /* Snapshot old config for change detection */
    const cam_config_t *old_cfg = config_get();
    uint8_t old_framesize = old_cfg->cam_framesize;
    uint8_t old_fps = old_cfg->cam_fps;
    uint8_t old_quality = old_cfg->cam_quality;
    uint8_t old_vflip = old_cfg->cam_vflip;
    uint8_t old_hmirror = old_cfg->cam_hmirror;
    uint8_t old_xclk = old_cfg->xclk_freq_mhz;
    char old_wifi_ssid[33];
    char old_wifi_pass[65];
    strncpy(old_wifi_ssid, old_cfg->wifi_ssid, sizeof(old_wifi_ssid) - 1);
    old_wifi_ssid[sizeof(old_wifi_ssid) - 1] = '\0';
    strncpy(old_wifi_pass, old_cfg->wifi_pass, sizeof(old_wifi_pass) - 1);
    old_wifi_pass[sizeof(old_wifi_pass) - 1] = '\0';

    /* Copy current config, apply partial updates */
    cam_config_t new_cfg;
    memcpy(&new_cfg, config_get(), sizeof(cam_config_t));

    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "wifi_ssid")) && cJSON_IsString(item))
        strncpy(new_cfg.wifi_ssid, item->valuestring, sizeof(new_cfg.wifi_ssid) - 1);
    if ((item = cJSON_GetObjectItem(root, "wifi_pass")) && cJSON_IsString(item)) {
        if (strcmp(item->valuestring, "****") != 0)
            strncpy(new_cfg.wifi_pass, item->valuestring, sizeof(new_cfg.wifi_pass) - 1);
    }
    if ((item = cJSON_GetObjectItem(root, "server_url")) && cJSON_IsString(item))
        strncpy(new_cfg.server_url, item->valuestring, sizeof(new_cfg.server_url) - 1);
    if ((item = cJSON_GetObjectItem(root, "device_name")) && cJSON_IsString(item))
        strncpy(new_cfg.device_name, item->valuestring, sizeof(new_cfg.device_name) - 1);
    if ((item = cJSON_GetObjectItem(root, "cam_framesize")) && cJSON_IsNumber(item)) {
        int val = (int)item->valuedouble;
        /* 三层上限校验（同 /api/camera；本板实际由 board 层钳在 VGA，
         * 合法域 = supported_resolutions = {10}，其余一律 400） */
        if (val < (int)FRAMESIZE_VGA || val > (int)camera_get_effective_max_res()) {
            cJSON_Delete(root);
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "cam_framesize out of range (%d-%d, source: %s)",
                     (int)FRAMESIZE_VGA, (int)camera_get_effective_max_res(),
                     camera_res_cap_source());
            return json_error(req, msg, HTTPD_400_BAD_REQUEST);
        }
        new_cfg.cam_framesize = (uint8_t)val;
    }
    /* 契约 §4：cam_fps 1-30（此前无校验裸收，v1.3 修复） */
    if ((item = cJSON_GetObjectItem(root, "cam_fps")) && cJSON_IsNumber(item)) {
        int val = (int)item->valuedouble;
        if (val < 1 || val > 30) {
            cJSON_Delete(root);
            return json_error(req, "cam_fps out of range (1-30)", HTTPD_400_BAD_REQUEST);
        }
        new_cfg.cam_fps = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(root, "cam_quality")) && cJSON_IsNumber(item)) {
        int val = (int)item->valuedouble;
        if (val < CAMERA_QUALITY_MIN || val > CAMERA_QUALITY_MAX) {
            char msg[80];
            snprintf(msg, sizeof(msg), "cam_quality out of range (%d-%d)",
                     CAMERA_QUALITY_MIN, CAMERA_QUALITY_MAX);
            cJSON_Delete(root);
            return json_error(req, msg, HTTPD_400_BAD_REQUEST);
        }
        new_cfg.cam_quality = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(root, "cam_vflip")) && cJSON_IsNumber(item)) {
        int val = (int)item->valuedouble;
        if (val != 0 && val != 1) {
            cJSON_Delete(root);
            return json_error(req, "cam_vflip must be 0 or 1", HTTPD_400_BAD_REQUEST);
        }
        new_cfg.cam_vflip = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(root, "cam_hmirror")) && cJSON_IsNumber(item)) {
        int val = (int)item->valuedouble;
        if (val != 0 && val != 1) {
            cJSON_Delete(root);
            return json_error(req, "cam_hmirror must be 0 or 1", HTTPD_400_BAD_REQUEST);
        }
        new_cfg.cam_hmirror = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(root, "xclk_freq_mhz")) && cJSON_IsNumber(item)) {
        int val = (int)item->valuedouble;
        if (val != 10 && val != 16 && val != 20) {
            cJSON_Delete(root);
            return json_error(req, "xclk_freq_mhz must be 10, 16 or 20", HTTPD_400_BAD_REQUEST);
        }
        new_cfg.xclk_freq_mhz = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(root, "timezone")) && cJSON_IsString(item)) {
        if (strlen(item->valuestring) >= sizeof(new_cfg.timezone)) {
            cJSON_Delete(root);
            return json_error(req, "timezone too long (max 47)", HTTPD_400_BAD_REQUEST);
        }
        strncpy(new_cfg.timezone, item->valuestring, sizeof(new_cfg.timezone) - 1);
        new_cfg.timezone[sizeof(new_cfg.timezone) - 1] = '\0';
    }
    /* motion 家族超集（契约 §4 校验矩阵） */
    if ((item = cJSON_GetObjectItem(root, "motion_enabled")) && cJSON_IsNumber(item)) {
        int val = (int)item->valuedouble;
        if (val != 0 && val != 1) {
            cJSON_Delete(root);
            return json_error(req, "motion_enabled must be 0 or 1", HTTPD_400_BAD_REQUEST);
        }
        new_cfg.motion_enabled = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(root, "motion_sensitivity")) && cJSON_IsNumber(item)) {
        int val = (int)item->valuedouble;
        if (val < 0 || val > 100) {
            cJSON_Delete(root);
            return json_error(req, "motion_sensitivity out of range (0-100)", HTTPD_400_BAD_REQUEST);
        }
        new_cfg.motion_sensitivity = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(root, "motion_cooldown_s")) && cJSON_IsNumber(item)) {
        int val = (int)item->valuedouble;
        if (val < 1 || val > 300) {
            cJSON_Delete(root);
            return json_error(req, "motion_cooldown_s out of range (1-300)", HTTPD_400_BAD_REQUEST);
        }
        new_cfg.motion_cooldown_s = (uint16_t)val;
    }
    if ((item = cJSON_GetObjectItem(root, "motion_active_interval_s")) && cJSON_IsNumber(item)) {
        int val = (int)item->valuedouble;
        if (val < 1 || val > 30) {
            cJSON_Delete(root);
            return json_error(req, "motion_active_interval_s out of range (1-30)", HTTPD_400_BAD_REQUEST);
        }
        new_cfg.motion_active_interval_s = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(root, "wifi_ssid_2")) && cJSON_IsString(item))
        strncpy(new_cfg.wifi_ssid_2, item->valuestring, sizeof(new_cfg.wifi_ssid_2) - 1);
    if ((item = cJSON_GetObjectItem(root, "wifi_pass_2")) && cJSON_IsString(item)) {
        if (strcmp(item->valuestring, "****") != 0)
            strncpy(new_cfg.wifi_pass_2, item->valuestring, sizeof(new_cfg.wifi_pass_2) - 1);
    }
    if ((item = cJSON_GetObjectItem(root, "mdns_hostname")) && cJSON_IsString(item))
        strncpy(new_cfg.mdns_hostname, item->valuestring, sizeof(new_cfg.mdns_hostname) - 1);
    /* webhook 组（契约 §3.2） */
    if ((item = cJSON_GetObjectItem(root, "alert_webhook_enabled")) && cJSON_IsNumber(item)) {
        int val = (int)item->valuedouble;
        if (val != 0 && val != 1) {
            cJSON_Delete(root);
            return json_error(req, "alert_webhook_enabled must be 0 or 1", HTTPD_400_BAD_REQUEST);
        }
        new_cfg.alert_webhook_enabled = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(root, "alert_webhook_url")) && cJSON_IsString(item))
        strncpy(new_cfg.alert_webhook_url, item->valuestring, sizeof(new_cfg.alert_webhook_url) - 1);
    if ((item = cJSON_GetObjectItem(root, "webhook_secret")) && cJSON_IsString(item)) {
        if (strcmp(item->valuestring, "****") != 0)
            strncpy(new_cfg.webhook_secret, item->valuestring, sizeof(new_cfg.webhook_secret) - 1);
    }
    if ((item = cJSON_GetObjectItem(root, "onvif_enable")) && cJSON_IsNumber(item)) {
        int val = (int)item->valuedouble;
        if (val != 0 && val != 1) {
            cJSON_Delete(root);
            return json_error(req, "onvif_enable must be 0 or 1", HTTPD_400_BAD_REQUEST);
        }
        new_cfg.onvif_enable = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(root, "ws_enable")) && cJSON_IsNumber(item)) {
        int val = (int)item->valuedouble;
        if (val != 0 && val != 1) {
            cJSON_Delete(root);
            return json_error(req, "ws_enable must be 0 or 1", HTTPD_400_BAD_REQUEST);
        }
        new_cfg.ws_enable = (uint8_t)val;
    }
    /* 契约 v1.1：拒绝空/过短密码 */
    if ((item = cJSON_GetObjectItem(root, "web_password")) && cJSON_IsString(item)) {
        if (strlen(item->valuestring) < 6) {
            cJSON_Delete(root);
            return json_error(req, "web_password must be at least 6 characters", HTTPD_400_BAD_REQUEST);
        }
        strncpy(new_cfg.web_password, item->valuestring, sizeof(new_cfg.web_password) - 1);
    }

    cJSON_Delete(root);

    /* Apply timezone immediately */
    if (new_cfg.timezone[0] != '\0') {
        setenv("TZ", new_cfg.timezone, 1);
        tzset();
    }

    esp_err_t err = config_save(&new_cfg);
    if (err != ESP_OK) {
        return json_error(req, "Failed to save config", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    /* --- Camera settings: save + reboot to apply --- */
    /* 2026-09-03/04 实测：热重配（camera_deinit+init）在 MJPEG/motion/广播
     * 生产者并发取帧时导致设备级静默死亡（fb_count=1 DRAM，无 PSRAM）。
     * /api/camera 已改为重启应用；本端点此前仍走热重配（2026-09-04 发现的
     * 漏网之鱼）——现与 /api/camera 对齐：保存 + 应答 + 1s 后重启。
     * 重启是干净的锤子，零竞态（姐妹板 PSRAM fb_count=2 不受此限）。 */
    bool camera_changed = (new_cfg.cam_framesize != old_framesize ||
                           new_cfg.cam_fps != old_fps ||
                           new_cfg.cam_quality != old_quality ||
                           new_cfg.cam_vflip != old_vflip ||
                           new_cfg.cam_hmirror != old_hmirror ||
                           new_cfg.xclk_freq_mhz != old_xclk);

    /* --- WiFi change detection --- */
    bool wifi_changed = (strcmp(new_cfg.wifi_ssid, old_wifi_ssid) != 0 ||
                         strcmp(new_cfg.wifi_pass, old_wifi_pass) != 0);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "message",
        camera_changed ? "Camera settings saved (rebooting to apply)"
        : wifi_changed ? "WiFi settings changed, reboot required"
        : "Config updated");
    cJSON_AddBoolToObject(resp, "rebooting", camera_changed);
    esp_err_t send_ret = json_ok(req, resp);
    if (camera_changed) {
        ESP_LOGW(TAG, "Camera config changed via /api/config — rebooting to apply");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    return send_ret;
}

/* ------------------------------------------------------------------ */
/*  POST /api/reset                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t handler_reset(httpd_req_t *req)
{
    esp_err_t ret = require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_err_t err = config_reset();
    if (err != ESP_OK) {
        return json_error(req, "Failed to reset config", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "message", "Config reset to defaults");
    return json_ok(req, resp);
}

/* ------------------------------------------------------------------ */
/*  POST /api/reboot  (device reboot)                                  */
/* ------------------------------------------------------------------ */

static esp_err_t handler_reboot(httpd_req_t *req)
{
    esp_err_t ret = require_auth(req);
    if (ret != ESP_OK) {
        return ret;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "message", "Rebooting...");
    json_ok(req, resp);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /api/capabilities                                              */
/* ------------------------------------------------------------------ */

static esp_err_t handler_capabilities(httpd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    /* 契约 v1.0：12 个布尔能力位 + api_version/wifi_scan（见 docs/api-contract.md） */
    cJSON_AddStringToObject(data, "api_version", "1.3");
#ifdef CONFIG_MIBEECAM_ENABLE_WIFI_SCAN
    cJSON_AddBoolToObject(data, "wifi_scan", true);
#else
    cJSON_AddBoolToObject(data, "wifi_scan", false);
#endif
    cJSON_AddBoolToObject(data, "ai",        false);
    cJSON_AddBoolToObject(data, "sd",        false);
    cJSON_AddBoolToObject(data, "audio",     false);
    cJSON_AddBoolToObject(data, "ota",       false);
    cJSON_AddBoolToObject(data, "mic",       false);
    cJSON_AddBoolToObject(data, "flash_led", false);
    cJSON_AddBoolToObject(data, "recording", false);
    cJSON_AddBoolToObject(data, "timelapse", false);
#ifdef CONFIG_MIBEECAM_ENABLE_ONVIF
    cJSON_AddBoolToObject(data, "onvif",     true);
#else
    cJSON_AddBoolToObject(data, "onvif",     false);
#endif
    cJSON_AddBoolToObject(data, "rtsp",      false);
#ifdef CONFIG_MIBEECAM_ENABLE_WS
    cJSON_AddBoolToObject(data, "websocket", true);
#else
    cJSON_AddBoolToObject(data, "websocket", false);
#endif
#ifdef CONFIG_MIBEECAM_ENABLE_MDNS
    cJSON_AddBoolToObject(data, "mdns",      true);
#else
    cJSON_AddBoolToObject(data, "mdns",      false);
#endif

    return json_ok(req, data);
}

#ifdef CONFIG_MIBEECAM_ENABLE_WIFI_SCAN
/* ------------------------------------------------------------------ */
/*  GET /api/scan                                                      */
/* ------------------------------------------------------------------ */

static esp_err_t handler_scan(httpd_req_t *req)
{
    // Allocate scan results (max 20 networks)
    #define MAX_SCAN_RESULTS 20
    wifi_ap_record_t ap_records[MAX_SCAN_RESULTS];
    uint16_t found = 0;

    esp_err_t ret = wifi_scan(ap_records, MAX_SCAN_RESULTS, &found);
    if (ret != ESP_OK) {
        return json_error(req, esp_err_to_name(ret), HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    // Sort by RSSI descending (simple bubble sort for small arrays)
    for (int i = 0; i < found - 1; i++) {
        for (int j = i + 1; j < found; j++) {
            if (ap_records[j].rssi > ap_records[i].rssi) {
                wifi_ap_record_t tmp = ap_records[i];
                ap_records[i] = ap_records[j];
                ap_records[j] = tmp;
            }
        }
    }

    // Build JSON array (no BSSID for privacy)
    cJSON *networks = cJSON_CreateArray();
    for (int i = 0; i < found; i++) {
        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", (char *)ap_records[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", ap_records[i].rssi);
        cJSON_AddNumberToObject(net, "auth", ap_records[i].authmode);
        cJSON_AddNumberToObject(net, "channel", ap_records[i].primary);
        cJSON_AddItemToArray(networks, net);
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "networks", networks);
    return json_ok(req, data);
}
#endif

/* ------------------------------------------------------------------ */
/*  GET /metrics  (Prometheus format)                                  */
/* ------------------------------------------------------------------ */

static esp_err_t handler_metrics(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "text/plain; version=0.0.4; charset=utf-8");

    size_t free_heap = esp_get_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t min_heap = esp_get_minimum_free_heap_size();
    float temp = get_chip_temp();
    wifi_state_t ws = wifi_get_state();
    int stream_client_count = mjpeg_streamer_get_client_count();
    bool camera_init = camera_is_initialized();
    const char *ip_str = wifi_get_ip_str();
    size_t baseline_free = 0, baseline_min = 0;
    health_get_baselines(&baseline_free, &baseline_min);
    int heap_delta = (int)free_heap - (int)baseline_free;

    char buf[2048];
    int len = snprintf(buf, sizeof(buf),
        "# HELP esp_free_heap_bytes Free heap memory in bytes\n"
        "# TYPE esp_free_heap_bytes gauge\n"
        "esp_free_heap_bytes %u\n"
        "# HELP esp_free_psram_bytes Free PSRAM memory in bytes\n"
        "# TYPE esp_free_psram_bytes gauge\n"
        "esp_free_psram_bytes %u\n"
        "# HELP esp_chip_temp_celsius Chip temperature in Celsius\n"
        "# TYPE esp_chip_temp_celsius gauge\n"
        "esp_chip_temp_celsius %.1f\n"
        "# HELP wifi_state Current WiFi state (0=ap,1=connecting,2=connected,3=disconnected)\n"
        "# TYPE wifi_state gauge\n"
        "wifi_state %d\n"
        "# HELP esp_min_heap_bytes Minimum free heap ever recorded in bytes\n"
        "# TYPE esp_min_heap_bytes gauge\n"
        "esp_min_heap_bytes %u\n"
        "# HELP esp_uptime_seconds System uptime in seconds\n"
        "# TYPE esp_uptime_seconds counter\n"
        "esp_uptime_seconds %llu\n"
        "# HELP stream_clients Number of MJPEG streaming clients\n"
        "# TYPE stream_clients gauge\n"
        "stream_clients %d\n"
        "# HELP camera_initialized Camera initialization status (0=not_init,1=init)\n"
        "# TYPE camera_initialized gauge\n"
        "camera_initialized %d\n"
        "# HELP wifi_ip WiFi IP address\n"
        "# TYPE wifi_ip info\n"
        "wifi_ip{ip=\"%s\"} 1\n"
        "# HELP mibeecam_heap_baseline_bytes Heap baseline at init in bytes\n"
        "# TYPE mibeecam_heap_baseline_bytes gauge\n"
        "mibeecam_heap_baseline_bytes %u\n"
        "# HELP mibeecam_heap_delta_bytes Heap delta from baseline in bytes\n"
        "# TYPE mibeecam_heap_delta_bytes gauge\n"
        "mibeecam_heap_delta_bytes %d\n",
        (unsigned)free_heap, (unsigned)free_psram, temp,
        (int)ws, (unsigned)min_heap, (unsigned long long)esp_timer_get_time() / 1000000, stream_client_count, camera_init ? 1 : 0, ip_str,
        (unsigned)baseline_free, heap_delta);

    return httpd_resp_send(req, buf, len);
}

/* ------------------------------------------------------------------ */
/*  GET /api/capture                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t handler_capture(httpd_req_t *req)
{
    set_cors_headers(req);

    camera_fb_t *fb = NULL;
    esp_err_t err = camera_capture(&fb);
    if (err != ESP_OK || fb == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    esp_err_t ret = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    camera_return_fb(fb);
    return ret;
}

#ifdef CONFIG_MIBEECAM_ENABLE_WS
static esp_err_t ws_handler(httpd_req_t *req)
{
    int sockfd = httpd_req_to_sockfd(req);

    /* Detect initial WS handshake (called after server completes WS upgrade)
     * req->method == HTTP_GET (set by HTTP parser) for initial handshake call
     * req->method == 0 (init_req default) for subsequent WS frame calls */
    if (req->method == HTTP_GET) {
        int slot = ws_add_client(sockfd, req->handle);
        if (slot < 0) {
            ESP_LOGW(TAG, "WS client table full, rejecting");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "WS client connected: fd=%d slot=%d", sockfd, slot);
        return ESP_OK;
    }

    /* Receive WS frame — probe with 0 to get frame info */
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WS recv failed: %s, removing fd=%d", esp_err_to_name(ret), sockfd);
        ws_remove_client(sockfd);
        return ret;
    }

    /* Read and discard the full payload */
    if (ws_pkt.len > 0) {
        uint8_t *buf = malloc(ws_pkt.len);
        if (buf) {
            ws_pkt.payload = buf;
            httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
            free(buf);
        }
    }

    ws_touch_client(sockfd);
    ESP_LOGD(TAG, "WS frame from fd=%d (dropped)", sockfd);
    return ESP_OK;
}
#endif
/* ------------------------------------------------------------------ */
/*  OPTIONS / *  - CORS preflight                                       */
/* ------------------------------------------------------------------ */

static esp_err_t handler_options(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------------ */
/*  GET / *  - SPIFFS static file serving                               */
/* ------------------------------------------------------------------ */

static const char *get_content_type(const char *path)
{
    if (strstr(path, ".html")) return "text/html; charset=utf-8";
    if (strstr(path, ".css"))  return "text/css";
    if (strstr(path, ".js"))   return "application/javascript";
    if (strstr(path, ".png"))  return "image/png";
    if (strstr(path, ".ico"))  return "image/x-icon";
    if (strstr(path, ".svg"))  return "image/svg+xml";
    if (strstr(path, ".json")) return "application/json";
    return "application/octet-stream";
}

static esp_err_t handler_static(httpd_req_t *req)
{
    set_cors_headers(req);

    /* Map "/" to "/index.html" */
    const char *uri = req->uri;
    char filepath[1040];
    if (strcmp(uri, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "/spiffs/index.html");
    } else {
        snprintf(filepath, sizeof(filepath), "/spiffs%s", uri);
    }

    /* Security: reject paths with ".." */
    if (strstr(filepath, "..") != NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Serving static file: %s", filepath);

    FILE *f = fopen(filepath, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, get_content_type(filepath));
    /* UI 随固件更新：静态资源一律 no-cache，避免"新 HTML 配旧 JS"的一小时窗口
     * （2026-09-03 crossOrigin 修复后浏览器仍用旧缓存 app.js 才暴露） */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, must-revalidate");

    char buf[1024];
    size_t total = 0;
    while (1) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        total += n;
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);

    /* End chunked response */
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGD(TAG, "Served %u bytes from %s", (unsigned)total, filepath);
    return ESP_OK;
}

#ifdef CONFIG_MIBEECAM_ENABLE_WS
esp_err_t ws_broadcast_text(const char *msg, size_t len)
{
    if (msg == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    httpd_handle_t server = NULL;
    int sent_count = 0;

    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)msg,
        .len = len,
        .final = true,
    };

    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i].sockfd != -1) {
            if (server == NULL) {
                server = s_ws_clients[i].server;
            }
            int64_t now = esp_timer_get_time();
            int64_t idle = now - s_ws_clients[i].last_active;
            if (idle > WS_IDLE_TIMEOUT_S * 1000000LL) {
                ESP_LOGI(TAG, "WS client idle timeout: fd=%d", s_ws_clients[i].sockfd);
                if (server) {
                    httpd_ws_frame_t close_pkt = { .type = HTTPD_WS_TYPE_CLOSE, .final = true };
                    httpd_ws_send_frame_async(server, s_ws_clients[i].sockfd, &close_pkt);
                }
                s_ws_clients[i].sockfd = -1;
                continue;
            }

            if (server) {
                esp_err_t send_ret = httpd_ws_send_frame_async(server, s_ws_clients[i].sockfd, &ws_pkt);
                if (send_ret == ESP_OK) {
                    sent_count++;
                } else {
                    ESP_LOGW(TAG, "WS send failed to fd=%d: %s, removing",
                             s_ws_clients[i].sockfd, esp_err_to_name(send_ret));
                    s_ws_clients[i].sockfd = -1;
                }
            }
        }
    }

    xSemaphoreGive(s_ws_mutex);
    ESP_LOGD(TAG, "WS broadcast to %d clients", sent_count);
    return ESP_OK;
}

static void event_to_json(const event_t *event, char *out_buf, size_t buf_len)
{
    /* 契约 v1.0 统一事件格式: {"type","timestamp","data"} */
    const char *type_str = "unknown";
    switch (event->type) {
        case EVENT_MOTION_DETECTED: type_str = "motion_started"; break;
        case EVENT_MOTION_END:      type_str = "motion_cleared"; break;
        case EVENT_WIFI_STATE_CHANGED: type_str = "wifi_state_changed"; break;
        case EVENT_WIFI_SWITCHED_SSID: type_str = "wifi_switched_ssid"; break;
        case EVENT_STREAM_CLIENT_CONNECTED: type_str = "stream_client_connected"; break;
        case EVENT_STREAM_CLIENT_DISCONNECTED: type_str = "stream_client_disconnected"; break;
        case EVENT_HEALTH_WARNING: type_str = "health_warning"; break;
        case EVENT_UPLOAD_SUCCESS: type_str = "upload_success"; break;
        case EVENT_UPLOAD_FAILED: type_str = "upload_failed"; break;
        default: break;
    }
    snprintf(out_buf, buf_len,
             "{\"type\":\"%s\",\"timestamp\":%lld,\"data\":{}}",
             type_str, (long long)(event->timestamp / 1000));
}

static void ws_event_handler(const event_t *event, void *user_data)
{
    (void)user_data;
    char json_buf[256];
    event_to_json(event, json_buf, sizeof(json_buf));
    ws_broadcast_text(json_buf, strlen(json_buf));
}
#endif

/* ------------------------------------------------------------------ */
/*  Unified API endpoints (ported from esp32s3-n16r8-cam)             */
/* ------------------------------------------------------------------ */

/* GET /api/camera — current camera settings (契约 v1.0: 含 supported_resolutions) */
static esp_err_t handler_api_camera_get(httpd_req_t *req)
{
    const cam_config_t *cfg = config_get();
    cJSON *data = cJSON_CreateObject();
    if (!data)
        return json_error(req, "alloc failed", HTTPD_500_INTERNAL_SERVER_ERROR);

    cJSON_AddStringToObject(data, "resolution", res_to_str(cfg->cam_framesize));
    cJSON_AddNumberToObject(data, "cam_framesize", (double)cfg->cam_framesize);
    cJSON_AddNumberToObject(data, "cam_quality",   (double)cfg->cam_quality);
    /* 契约 v1.0 核心字段（本板持久化并在 camera_init 应用） */
    cJSON_AddNumberToObject(data, "cam_vflip",     (double)cfg->cam_vflip);
    cJSON_AddNumberToObject(data, "cam_hmirror",   (double)cfg->cam_hmirror);
    /* 契约扩展（2026-09-04）：画质滑杆边界由板端声明，前端据此钳制输入 */
    cJSON_AddNumberToObject(data, "quality_min",   (double)CAMERA_QUALITY_MIN);
    cJSON_AddNumberToObject(data, "quality_max",   (double)CAMERA_QUALITY_MAX);

    /* 三层分辨率上限（2026-09-04 家族统一）：sensor ∩ board ∩ memory。
     * value = 家族统一 framesize_t 刻度（契约 v1.3 §5）。
     * 板级常数 VGA 的实测依据（2026-09-03）：SVGA 使 cam_hal DMA 池
     * 61KB→96KB（800×600/5），空闲堆 45K→16K，暗光大帧再挤压 → 每秒
     * 分配失败、httpd 拒连、流 16s 断连螺旋（PIT-012）。"能出图"≠"能
     * 稳定带流"。详见 camera_driver.h CAMERA_RES_BOARD_MAX 注。
     * 本板 effective = VGA → supported_resolutions 只有 value 10 一项。 */
    {
        static const struct { const char *label; int value; } s_supported_res[] = {
            { "VGA (640x480)",    10 },
            { "SVGA (800x600)",   11 },
            { "XGA (1024x768)",   12 },
            { "HD (1280x720)",    13 },
            { "SXGA (1280x1024)", 14 },
            { "UXGA (1600x1200)", 15 },
        };
        int eff_max = (int)camera_get_effective_max_res();
        cJSON *res_arr = cJSON_CreateArray();
        for (size_t i = 0; i < sizeof(s_supported_res) / sizeof(s_supported_res[0]); i++) {
            if (s_supported_res[i].value > eff_max) break;
            cJSON *res_item = cJSON_CreateObject();
            cJSON_AddStringToObject(res_item, "label", s_supported_res[i].label);
            cJSON_AddNumberToObject(res_item, "value", s_supported_res[i].value);
            cJSON_AddItemToArray(res_arr, res_item);
        }
        cJSON_AddItemToObject(data, "supported_resolutions", res_arr);
        /* 契约扩展（2026-09-04）：上限被哪一层钳制（sensor/board/memory） */
        cJSON_AddStringToObject(data, "res_cap_source", camera_res_cap_source());
    }

    return json_ok(req, data);
}

/* POST /api/camera — update camera settings (resolution + quality persisted, rest accepted) */
static esp_err_t handler_api_camera_post(httpd_req_t *req)
{
    esp_err_t auth = require_auth(req);
    if (auth != ESP_OK)
        return json_error(req, "UNAUTHORIZED", HTTPD_401_UNAUTHORIZED);

    char *body = NULL;
    int body_len = 0;
    if (read_body(req, &body, &body_len) != ESP_OK || !body)
        return json_error(req, "empty body", HTTPD_400_BAD_REQUEST);

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json)
        return json_error(req, "invalid JSON", HTTPD_400_BAD_REQUEST);

    const cam_config_t *cfg = config_get();
    cam_config_t newcfg = *cfg;

    cJSON *fs = cJSON_GetObjectItem(json, "cam_framesize");
    if (fs && cJSON_IsNumber(fs)) {
        int val = (int)fs->valuedouble;
        /* 三层上限校验（同 GET 端点；本板实际由 board 层钳在 VGA，
         * 家族刻度下合法域 = 10..effective，其余一律 400） */
        if (val < (int)FRAMESIZE_VGA || val > (int)camera_get_effective_max_res()) {
            cJSON_Delete(json);
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "cam_framesize out of range (%d-%d, source: %s)",
                     (int)FRAMESIZE_VGA, (int)camera_get_effective_max_res(),
                     camera_res_cap_source());
            return json_error(req, msg, HTTPD_400_BAD_REQUEST);
        }
        newcfg.cam_framesize = (uint8_t)val;
    }

    cJSON *q = cJSON_GetObjectItem(json, "cam_quality");
    if (q && cJSON_IsNumber(q)) {
        int val = (int)q->valuedouble;
        if (val < CAMERA_QUALITY_MIN || val > CAMERA_QUALITY_MAX) {
            char msg[80];
            snprintf(msg, sizeof(msg), "cam_quality out of range (%d-%d)",
                     CAMERA_QUALITY_MIN, CAMERA_QUALITY_MAX);
            cJSON_Delete(json);
            return json_error(req, msg, HTTPD_400_BAD_REQUEST);
        }
        newcfg.cam_quality = (uint8_t)val;
    }

    /* cam_vflip / cam_hmirror：OV2640 寄存器可调，本板持久化并在
     * camera_init（重启后）应用。cam_brightness / cam_contrast /
     * cam_saturation / cam_sharpness 仍不持久化（本板未开通传感器
     * 微调，前端按字段缺省隐藏）。 */
    cJSON *vf = cJSON_GetObjectItem(json, "cam_vflip");
    if (vf && cJSON_IsNumber(vf)) {
        int val = (int)vf->valuedouble;
        if (val != 0 && val != 1) {
            cJSON_Delete(json);
            return json_error(req, "cam_vflip must be 0 or 1", HTTPD_400_BAD_REQUEST);
        }
        newcfg.cam_vflip = (uint8_t)val;
    }
    cJSON *hm = cJSON_GetObjectItem(json, "cam_hmirror");
    if (hm && cJSON_IsNumber(hm)) {
        int val = (int)hm->valuedouble;
        if (val != 0 && val != 1) {
            cJSON_Delete(json);
            return json_error(req, "cam_hmirror must be 0 or 1", HTTPD_400_BAD_REQUEST);
        }
        newcfg.cam_hmirror = (uint8_t)val;
    }
    cJSON *xc = cJSON_GetObjectItem(json, "xclk_freq_mhz");
    if (xc && cJSON_IsNumber(xc)) {
        int val = (int)xc->valuedouble;
        if (val != 10 && val != 16 && val != 20) {
            cJSON_Delete(json);
            return json_error(req, "xclk_freq_mhz must be 10, 16 or 20", HTTPD_400_BAD_REQUEST);
        }
        newcfg.xclk_freq_mhz = (uint8_t)val;
    }

    cJSON_Delete(json);

    esp_err_t ret = config_save(&newcfg);
    if (ret != ESP_OK)
        return json_error(req, "save failed", HTTPD_500_INTERNAL_SERVER_ERROR);

    /* 2026-09-03 实测事故：热重配（camera_deinit+init）在 MJPEG/motion/广播
     * 生产者并发取帧时导致设备级静默死亡（fb_count=1 DRAM，无 PSRAM）。
     * 本板改为"保存 + 延迟重启应用"——重启是干净的锤子，零竞态。
     * 姐妹板（PSRAM, fb_count=2）不受此限，仍走热重配。 */
    {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "status", "saved (rebooting to apply)");
        cJSON_AddBoolToObject(data, "rebooting", true);
        esp_err_t send_ret = json_ok(req, data);
        ESP_LOGW(TAG, "Camera config saved — rebooting to apply (quality=%u res=%u)",
                 newcfg.cam_quality, newcfg.cam_framesize);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return send_ret;
    }
}

/* GET /api/auth — 校验 X-Password（契约 v1.0 核心端点） */
static esp_err_t handler_api_auth(httpd_req_t *req)
{
    const cam_config_t *cfg = config_get();

    if (cfg->web_password[0] == '\0') {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "auth", true);
        cJSON_AddBoolToObject(data, "password_set", false);
        return json_ok(req, data);
    }
    if (check_auth(req)) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "auth", true);
        cJSON_AddBoolToObject(data, "password_set", true);
        return json_ok(req, data);
    }
    return json_error(req, "unauthorized", HTTPD_401_UNAUTHORIZED);
}

/* POST /api/time — 手动设置系统时间（契约 v1.0 核心端点） */
static esp_err_t handler_api_time(httpd_req_t *req)
{
    esp_err_t auth = require_auth(req);
    if (auth != ESP_OK)
        return json_error(req, "UNAUTHORIZED", HTTPD_401_UNAUTHORIZED);

    char *body = NULL;
    int body_len = 0;
    if (read_body(req, &body, &body_len) != ESP_OK || !body)
        return json_error(req, "empty body", HTTPD_400_BAD_REQUEST);

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json)
        return json_error(req, "invalid JSON", HTTPD_400_BAD_REQUEST);

    cJSON *jy = cJSON_GetObjectItem(json, "year");
    cJSON *jmo = cJSON_GetObjectItem(json, "month");
    cJSON *jd = cJSON_GetObjectItem(json, "day");
    cJSON *jh = cJSON_GetObjectItem(json, "hour");
    cJSON *jmi = cJSON_GetObjectItem(json, "min");
    cJSON *js = cJSON_GetObjectItem(json, "sec");

    if (!cJSON_IsNumber(jy) || !cJSON_IsNumber(jmo) || !cJSON_IsNumber(jd) ||
        !cJSON_IsNumber(jh) || !cJSON_IsNumber(jmi) || !cJSON_IsNumber(js)) {
        cJSON_Delete(json);
        return json_error(req, "Missing time fields", HTTPD_400_BAD_REQUEST);
    }

    struct tm tm_now = {
        .tm_year = jy->valueint - 1900,
        .tm_mon = jmo->valueint - 1,
        .tm_mday = jd->valueint,
        .tm_hour = jh->valueint,
        .tm_min = jmi->valueint,
        .tm_sec = js->valueint,
    };
    time_t epoch = mktime(&tm_now);
    cJSON_Delete(json);

    if (epoch < (time_t)1577836800) {
        return json_error(req, "Invalid date", HTTPD_400_BAD_REQUEST);
    }
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    return json_ok(req, NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Set TCP_NODELAY + keepalive on every new HTTP connection.
 * NODELAY: disable Nagle's algorithm so small HTTP writes (headers, MJPEG
 * boundaries) aren't delayed by up to 1 RTT on marginal WiFi.
 * KEEPALIVE: detect dead connections in ~11s (5s idle + 3×2s probes),
 * freeing up limited server sockets for new clients. */
static esp_err_t on_session_open(httpd_handle_t hd, int sockfd)
{
    int enable = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));

    int keepalive = 1;
    int keepidle  = 5;
    int keepintvl = 2;
    int keepcnt   = 3;
    setsockopt(sockfd, SOL_SOCKET,  SO_KEEPALIVE,  &keepalive, sizeof(keepalive));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE,  &keepidle,  sizeof(keepidle));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT,   &keepcnt,   sizeof(keepcnt));
    return ESP_OK;
}

esp_err_t web_server_start(uint16_t port)
{
    if (s_server) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 30;  /* was 20 — headroom for new unified API endpoints */
    config.stack_size = 8192;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.open_fn = on_session_open;  /* TCP_NODELAY + keepalive per socket */

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server on port %d", port);
        return ESP_FAIL;
    }

    /* API endpoints */
    const httpd_uri_t api_status = {
        .uri      = "/api/status",
        .method   = HTTP_GET,
        .handler  = handler_api_status,
        .user_ctx = NULL,
    };
    const httpd_uri_t api_config_get = {
        .uri      = "/api/config",
        .method   = HTTP_GET,
        .handler  = handler_api_config_get,
        .user_ctx = NULL,
    };
    const httpd_uri_t api_config_post = {
        .uri      = "/api/config",
        .method   = HTTP_POST,
        .handler  = handler_api_config_post,
        .user_ctx = NULL,
    };
    const httpd_uri_t api_capabilities = {
        .uri      = "/api/capabilities",
        .method   = HTTP_GET,
        .handler  = handler_capabilities,
        .user_ctx = NULL,
    };
    const httpd_uri_t api_capture = {
        .uri      = "/api/capture",
        .method   = HTTP_GET,
        .handler  = handler_capture,
        .user_ctx = NULL,
    };
    const httpd_uri_t api_reset = {
        .uri      = "/api/reset",
        .method   = HTTP_POST,
        .handler  = handler_reset,
        .user_ctx = NULL,
    };
    const httpd_uri_t api_reboot = {
        .uri      = "/api/reboot",
        .method   = HTTP_POST,
        .handler  = handler_reboot,
        .user_ctx = NULL,
    };
#ifdef CONFIG_MIBEECAM_ENABLE_WIFI_SCAN
    const httpd_uri_t api_scan = {
        .uri      = "/api/scan",
        .method   = HTTP_GET,
        .handler  = handler_scan,
        .user_ctx = NULL,
    };
#endif
    const httpd_uri_t metrics = {
        .uri      = "/metrics",
        .method   = HTTP_GET,
        .handler  = handler_metrics,
        .user_ctx = NULL,
    };
    const httpd_uri_t options_any = {
        .uri      = "/*",
        .method   = HTTP_OPTIONS,
        .handler  = handler_options,
        .user_ctx = NULL,
    };
    const httpd_uri_t static_any = {
        .uri      = "/*",
        .method   = HTTP_GET,
        .handler  = handler_static,
        .user_ctx = NULL,
    };
    const httpd_uri_t api_camera_get = {
        .uri      = "/api/camera",
        .method   = HTTP_GET,
        .handler  = handler_api_camera_get,
        .user_ctx = NULL,
    };
    const httpd_uri_t api_camera_post = {
        .uri      = "/api/camera",
        .method   = HTTP_POST,
        .handler  = handler_api_camera_post,
        .user_ctx = NULL,
    };
    const httpd_uri_t api_auth = {
        .uri      = "/api/auth",
        .method   = HTTP_GET,
        .handler  = handler_api_auth,
        .user_ctx = NULL,
    };
    const httpd_uri_t api_time = {
        .uri      = "/api/time",
        .method   = HTTP_POST,
        .handler  = handler_api_time,
        .user_ctx = NULL,
    };

    /* 通配符匹配按注册顺序生效：精确端点必须先于 GET 通配符静态兜底注册，
     * 否则 /api/camera、/ws 会被静态处理器吞掉返回 404（曾致统一 SPA 失效） */
    httpd_register_uri_handler(s_server, &api_status);
    httpd_register_uri_handler(s_server, &api_config_get);
    httpd_register_uri_handler(s_server, &api_config_post);
    httpd_register_uri_handler(s_server, &api_capabilities);
    httpd_register_uri_handler(s_server, &api_capture);
    httpd_register_uri_handler(s_server, &api_reset);
    httpd_register_uri_handler(s_server, &api_reboot);
#ifdef CONFIG_MIBEECAM_ENABLE_WIFI_SCAN
    httpd_register_uri_handler(s_server, &api_scan);
#endif
    httpd_register_uri_handler(s_server, &metrics);
    httpd_register_uri_handler(s_server, &api_camera_get);
    httpd_register_uri_handler(s_server, &api_camera_post);
    httpd_register_uri_handler(s_server, &api_auth);
    httpd_register_uri_handler(s_server, &api_time);
    httpd_register_uri_handler(s_server, &options_any);

#ifdef CONFIG_MIBEECAM_ENABLE_WS
    /* 契约 §3.2 WebSocket 组：ws_enable=0 时不注册 /ws（也不订阅事件，
     * 重启生效——能力位不变，仅运行时入口关闭） */
    if (config_get()->ws_enable) {
        ws_clients_init();
        httpd_uri_t uri_ws = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .is_websocket = true,
            .handle_ws_control_frames = true,
        };
        httpd_register_uri_handler(s_server, &uri_ws);

        event_bus_subscribe(EVENT_MOTION_DETECTED, ws_event_handler, NULL, NULL);
        event_bus_subscribe(EVENT_MOTION_END, ws_event_handler, NULL, NULL);
        event_bus_subscribe(EVENT_WIFI_STATE_CHANGED, ws_event_handler, NULL, NULL);
        event_bus_subscribe(EVENT_WIFI_SWITCHED_SSID, ws_event_handler, NULL, NULL);
        event_bus_subscribe(EVENT_STREAM_CLIENT_CONNECTED, ws_event_handler, NULL, NULL);
        event_bus_subscribe(EVENT_STREAM_CLIENT_DISCONNECTED, ws_event_handler, NULL, NULL);
        event_bus_subscribe(EVENT_HEALTH_WARNING, ws_event_handler, NULL, NULL);
        event_bus_subscribe(EVENT_UPLOAD_SUCCESS, ws_event_handler, NULL, NULL);
        event_bus_subscribe(EVENT_UPLOAD_FAILED, ws_event_handler, NULL, NULL);
        ESP_LOGI(TAG, "WS event subscriptions registered");
    } else {
        ESP_LOGI(TAG, "WS disabled in config (ws_enable=0), /ws not registered");
    }
#endif

    /* 静态兜底必须最后注册（见上方顺序说明） */
    httpd_register_uri_handler(s_server, &static_any);

    ESP_LOGI(TAG, "Web server started on port %d", port);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        mjpeg_streamer_stop();
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Web server stopped");
    }
}

httpd_handle_t web_server_get_handle(void)
{
    return s_server;
}
