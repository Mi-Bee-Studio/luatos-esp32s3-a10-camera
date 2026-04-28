/*
 * Web Server Module Implementation
 * REST API endpoints + SPIFFS static file serving for ESP32-S3-A10 camera.
 *
 * Endpoints:
 *   GET  /api/status   — device status JSON
 *   GET  /api/config   — current config JSON
 *   POST /api/config   — partial config update (auth required)
 *   POST /api/reset    — reset config to defaults (auth required)
 *   GET  /metrics      — Prometheus-format metrics
 *   GET  /capture      — single JPEG frame
 *   OPTIONS / *         - CORS preflight
 *   GET    / *          - SPIFFS static files
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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

/* Extern — will be provided by health_monitor module (T11) */
extern float get_chip_temp(void);

/* ------------------------------------------------------------------ */
/*  JSON / HTTP helpers                                                */
/* ------------------------------------------------------------------ */

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
}

static esp_err_t json_ok(httpd_req_t *req, cJSON *root)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    char *out = cJSON_PrintUnformatted(root);
    esp_err_t ret = httpd_resp_send(req, out, strlen(out));
    free(out);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t json_error(httpd_req_t *req, const char *msg, int code)
{
    set_cors_headers(req);
    httpd_resp_set_status(req, "400 Bad Request");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg);
    httpd_resp_set_type(req, "application/json");
    char *out = cJSON_PrintUnformatted(root);
    esp_err_t ret = httpd_resp_send(req, out, strlen(out));
    free(out);
    cJSON_Delete(root);
    return ret;
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
/*  Resolution helper                                                  */
/* ------------------------------------------------------------------ */

static const char *res_to_str(uint8_t res)
{
    switch (res) {
        case 0: return "VGA";
        case 1: return "SVGA";
        case 2: return "XGA";
        case 3: return "UXGA";
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

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi_ssid", cfg->wifi_ssid);

    const char *state_str = "unknown";
    switch (ws) {
        case WIFI_STATE_AP:              state_str = "ap"; break;
        case WIFI_STATE_STA_CONNECTING:  state_str = "connecting"; break;
        case WIFI_STATE_STA_CONNECTED:   state_str = "connected"; break;
        case WIFI_STATE_STA_DISCONNECTED: state_str = "disconnected"; break;
    }
    cJSON_AddStringToObject(root, "wifi_state", state_str);
    cJSON_AddStringToObject(root, "ip", wifi_get_ip_str());

    if (ws == WIFI_STATE_STA_CONNECTED) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            cJSON_AddNumberToObject(root, "wifi_rssi", ap_info.rssi);
            cJSON_AddNumberToObject(root, "wifi_channel", ap_info.primary);
        }
    }

    cJSON_AddStringToObject(root, "camera", camera_get_sensor_name());
    cJSON_AddStringToObject(root, "resolution", res_to_str(cfg->resolution));

    cJSON_AddNumberToObject(root, "uptime", (double)(esp_timer_get_time() / 1000000));

    float temp = get_chip_temp();
    cJSON_AddNumberToObject(root, "chip_temp", temp);

    return json_ok(req, root);
}

/* ------------------------------------------------------------------ */
/*  GET /api/config                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t handler_api_config_get(httpd_req_t *req)
{
    const cam_config_t *cfg = config_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi_ssid", cfg->wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_pass", cfg->wifi_pass[0] ? "****" : "");
    cJSON_AddStringToObject(root, "server_url", cfg->server_url);
    cJSON_AddStringToObject(root, "device_name", cfg->device_name);
    cJSON_AddNumberToObject(root, "resolution", cfg->resolution);
    cJSON_AddNumberToObject(root, "fps", cfg->fps);
    cJSON_AddNumberToObject(root, "jpeg_quality", cfg->jpeg_quality);
    cJSON_AddStringToObject(root, "timezone", cfg->timezone);
    cJSON_AddNumberToObject(root, "motion_threshold", cfg->motion_threshold);
    cJSON_AddNumberToObject(root, "motion_cooldown", cfg->motion_cooldown);

    return json_ok(req, root);
}

/* ------------------------------------------------------------------ */
/*  POST /api/config                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t handler_api_config_post(httpd_req_t *req)
{

    char *body = NULL;
    int body_len = 0;
    if (read_body(req, &body, &body_len) != ESP_OK) {
        return json_error(req, "Failed to read body", 400);
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return json_error(req, "Invalid JSON", 400);
    }

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
    if ((item = cJSON_GetObjectItem(root, "resolution")) && cJSON_IsNumber(item))
        new_cfg.resolution = (uint8_t)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "fps")) && cJSON_IsNumber(item))
        new_cfg.fps = (uint8_t)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "jpeg_quality")) && cJSON_IsNumber(item))
        new_cfg.jpeg_quality = (uint8_t)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "timezone")) && cJSON_IsString(item))
        strncpy(new_cfg.timezone, item->valuestring, sizeof(new_cfg.timezone) - 1);
    if ((item = cJSON_GetObjectItem(root, "motion_threshold")) && cJSON_IsNumber(item))
        new_cfg.motion_threshold = (uint8_t)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "motion_cooldown")) && cJSON_IsNumber(item))
        new_cfg.motion_cooldown = (uint8_t)item->valuedouble;

    cJSON_Delete(root);

    /* Apply timezone immediately */
    if (new_cfg.timezone[0] != '\0') {
        setenv("TZ", new_cfg.timezone, 1);
        tzset();
    }

    esp_err_t err = config_save(&new_cfg);
    if (err != ESP_OK) {
        return json_error(req, "Failed to save config", 400);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddStringToObject(resp, "message", "Config updated");
    return json_ok(req, resp);
}

/* ------------------------------------------------------------------ */
/*  POST /api/reset                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t handler_api_reset(httpd_req_t *req)
{

    esp_err_t err = config_reset();
    if (err != ESP_OK) {
        return json_error(req, "Failed to reset config", 400);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddStringToObject(resp, "message", "Config reset to defaults");
    return json_ok(req, resp);
}

/* ------------------------------------------------------------------ */
/*  POST /api/reboot  (device reboot)                                  */
/* ------------------------------------------------------------------ */

static esp_err_t handler_api_reboot(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddStringToObject(resp, "message", "Rebooting...");
    json_ok(req, resp);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

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

    char buf[1024];
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
        "wifi_ip{ip=\"%s\"} 1\n",
        (unsigned)free_heap, (unsigned)free_psram, temp,
        (int)ws, (unsigned)min_heap, (unsigned long long)esp_timer_get_time() / 1000000, stream_client_count, camera_init ? 1 : 0, ip_str);

    return httpd_resp_send(req, buf, len);
}

/* ------------------------------------------------------------------ */
/*  GET /capture                                                       */
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

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t web_server_start(uint16_t port)
{
    if (s_server) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 20;
    config.stack_size = 8192;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

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
    const httpd_uri_t api_reset = {
        .uri      = "/api/reset",
        .method   = HTTP_POST,
        .handler  = handler_api_reset,
        .user_ctx = NULL,
    };
    const httpd_uri_t api_reboot = {
        .uri      = "/api/reboot",
        .method   = HTTP_POST,
        .handler  = handler_api_reboot,
        .user_ctx = NULL,
    };
    const httpd_uri_t metrics = {
        .uri      = "/metrics",
        .method   = HTTP_GET,
        .handler  = handler_metrics,
        .user_ctx = NULL,
    };
    const httpd_uri_t capture = {
        .uri      = "/capture",
        .method   = HTTP_GET,
        .handler  = handler_capture,
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

    httpd_register_uri_handler(s_server, &api_status);
    httpd_register_uri_handler(s_server, &api_config_get);
    httpd_register_uri_handler(s_server, &api_config_post);
    httpd_register_uri_handler(s_server, &api_reset);
    httpd_register_uri_handler(s_server, &api_reboot);
    httpd_register_uri_handler(s_server, &metrics);
    httpd_register_uri_handler(s_server, &capture);
    /* Register MJPEG stream handler BEFORE wildcard to avoid interception */
    mjpeg_streamer_register(s_server);

    httpd_register_uri_handler(s_server, &options_any);
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
