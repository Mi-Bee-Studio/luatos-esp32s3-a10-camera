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
#include "device_id.h"
#include "web_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_http_server.h"
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

/* 板级流地址：MJPEG :81（本板无 RTSP）。回调时机构建，IP 断连窗口内
 * 退化为 0.0.0.0 形态——与组件快照默认值同一路径，无需静态缓冲。 */
static const char *port_stream_uri(void)
{
    static char uri[48];
    const char *ip = wifi_get_ip_str();
    snprintf(uri, sizeof(uri), "http://%s:81/stream", ip ? ip : "0.0.0.0");
    return uri;
}

static uint8_t port_frame_rate(void)
{
    return config_get()->cam_fps;   /* 契约 §3.1 cam_fps 消费者（旧 GetProfiles 硬编码 15，现随配置） */
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
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "mibeecam-%02x%02x",
             mac[4], mac[5]);

    const onvif_c_config_t cfg = {
        .manufacturer     = "MiBee",
        .model            = "MiBeeCam",
        .hardware_id      = "ESP32-S3",
        .firmware_version = "v0.1.0",
        .serial           = port_serial,
        .uuid             = port_uuid,
        .ip               = port_ip,
        .stream_uri       = port_stream_uri,
        .frame_rate       = port_frame_rate,
        .events_enabled   = NULL,   /* 本板无事件能力（无 CSI） */
        .http_port        = 80,
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
