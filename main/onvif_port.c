/*
 * MiBee Cam v0.1 — ONVIF 板级适配层（onvif-c 组件接缝）
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 唯一的板级差异面：把固件的 wifi/config/device_id 符号接进
 * onvif_c_config_t 回调（组件本身零板级 include）。luatos 板无
 * CSI/事件能力 → events_enabled = NULL（/onvif/events_service 不注册、
 * XAddr 不广播）。身份串沿旧 onvif_service.c GetDeviceInformation 原值
 * （MiBee / MiBeeCam / ESP32-S3；固件号随家族统一 v0.1.0，旧值 "1.0"）。
 *
 * 行为变化（旧 onvif_discovery.c → 组件）：WS-Discovery 的 Endpoint
 * Address 与 MessageID 由"静态 urn:uuid:mibeecam + 每报文 esp_random
 * 随机 UUID"改为 device_id.c 的稳定 eFuse-MAC UUID（f472b01e-…）——
 * NVR 侧设备身份跨重启稳定，与 SOAP 服务（本就用 device_get_uuid）
 * 一致；\r\n 夹带的报文统一为组件的金测试基线字节。
 */

#include "onvif_port.h"
#include "onvif_c.h"
#include "wifi_manager.h"
#include "config_manager.h"
#include "time_sync.h"
#include <sys/time.h>
#include <time.h>
#include "device_id.h"
#include "web_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "onvif_port";

static const char *port_serial(void)
{
    return device_get_serial();
}

static const char *port_uuid(void)
{
    return device_get_uuid();
}

static const char *port_ip(void)
{
    return wifi_get_ip_str();
}

/* MJPEG 流端口（本板无 RTSP，:81 为 web_server 流服务器监听口） */
#define STREAM_HTTP_PORT 81

/* 板级流地址：回调时机构建，IP 断连窗口内退化为 0.0.0.0 形态——与组件
 * 快照默认值同一路径，无需静态缓冲。 */
static const char *port_stream_uri(void)
{
    static char uri[48];
    const char *ip = wifi_get_ip_str();
    snprintf(uri, sizeof(uri), "http://%s:%d/stream",
             ip ? ip : "0.0.0.0", STREAM_HTTP_PORT);
    return uri;
}

static uint8_t port_frame_rate(void)
{
    return config_get()->cam_fps;   /* 契约 §3.1 cam_fps 消费者（旧 GetProfiles 硬编码 15，现随配置） */
}

/* ---- 时间配置接缝（issue #43：apply-or-fault，绝不静默） ---- */

static bool port_time_ntp_active(void)
{
    return time_is_synced();   /* 如实：墙钟有效才报 NTP */
}

static const char *port_time_tz(void)
{
    return config_get_timezone();
}

static esp_err_t port_time_apply(const onvif_c_time_req_t *req)
{
    if (req->has_tz && req->tz[0]) {
        setenv("TZ", req->tz, 1);
        tzset();
    }
    if (req->ntp_mode) {
        return time_sync_restart(config_get_timezone());
    }
    if (req->has_utc) {
        struct timeval tv = { .tv_sec = (time_t)req->utc_epoch, .tv_usec = 0 };
        if (settimeofday(&tv, NULL) != 0) return ESP_FAIL;
        ESP_LOGI(TAG, "manual time set to %lld", (long long)req->utc_epoch);
    }
    return ESP_OK;
}

static esp_err_t port_ntp_set(const char *const *servers, size_t n)
{
    esp_err_t err = config_set_ntp_servers(n > 0 ? servers[0] : "", n > 1 ? servers[1] : "");
    if (err != ESP_OK) return err;
    return time_sync_restart(config_get_timezone());
}

esp_err_t onvif_port_start(void)
{
    /* 运行时开关（原 Step 13.7 的 config 门：onvif_enable，本板默认 1；
     * 编译期门 CONFIG_MIBEECAM_ENABLE_ONVIF 由调用方保留） */
    if (!config_get()->onvif_enable) {
        ESP_LOGI(TAG, "ONVIF disabled by config");
        return ESP_OK;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    static char hostname[32];   /* cfg 浅拷贝持有指针，须与 cfg 同寿 */
    snprintf(hostname, sizeof(hostname), "mibeecam-%02x%02x",
             mac[4], mac[5]);

    const onvif_c_config_t cfg = {
        .manufacturer     = "MiBee",
        .model            = "MiBeeCam",
        .hardware_id      = "ESP32-S3",
        .firmware_version = esp_app_get_description()->version,
        .serial           = port_serial,
        .uuid             = port_uuid,
        .ip               = port_ip,
        .stream_uri       = port_stream_uri,
        .frame_rate       = port_frame_rate,
        .events_enabled   = NULL,   /* 本板无事件能力（无 CSI） */
        .time_ntp_active  = port_time_ntp_active,
        .time_tz          = port_time_tz,
        .time_apply       = port_time_apply,
        .ntp_set          = port_ntp_set,
        .http_port        = 80,
        .wdt_watch_discovery = true,
        .mdns_hostname    = hostname,
        .mdns_instance    = "MiBee Cam",
    };

    httpd_handle_t server = web_server_get_handle();
    if (!server) {
        ESP_LOGW(TAG, "Web server not available, ONVIF skipped");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = onvif_c_start(server, &cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ONVIF services started (onvif-c component)");
    }
    return err;
}
