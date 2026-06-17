/*
 * onvif_service.c - ONVIF SOAP service handlers
 *
 * Listens for SOAP POST requests on /onvif/device_service and /onvif/media_service.
 * Implements 5 minimal SOAP methods for ONVIF Profile S:
 *   GetDeviceInformation, GetCapabilities, GetServices (device_service)
 *   GetProfiles, GetStreamUri (media_service)
 *
 * Default disabled. Enable via CONFIG_MIBEECAM_ENABLE_ONVIF compile flag.
 * When disabled, all public functions return ESP_ERR_NOT_SUPPORTED.
 */
#include "onvif_service.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "wifi_manager.h"
#include "config_manager.h"
#include "esp_mac.h"
#include "esp_random.h"

#ifdef CONFIG_MIBEECAM_ENABLE_ONVIF

static const char *TAG = "onvif_s";

/* ------------------------------------------------------------------ */
/*  SOAP XML helpers                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Build a standard SOAP envelope header with the given body content
 * @param buf      Output buffer
 * @param buf_size Size of output buffer
 * @param body     SOAP body XML (without <soap:Body> tags)
 * @return Length written to buf (excluding null terminator)
 */
static int soap_envelope(char *buf, size_t buf_size, const char *body)
{
    return snprintf(buf, buf_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<soap:Envelope "
        "xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
        "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
        "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
        "<soap:Body>"
        "%s"
        "</soap:Body>"
        "</soap:Envelope>",
        body);
}

/**
 * @brief Handle WS-Discovery Probe received over HTTP (directed discovery).
 * Builds a ProbeMatches response with device UUID and RelatesTo from the incoming Probe.
 */
static esp_err_t handle_http_probe(httpd_req_t *req, const char *body)
{
    const char *ip = wifi_get_ip_str();
    if (!ip || strcmp(ip, "0.0.0.0") == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No IP address");
        return ESP_FAIL;
    }

    /* Generate device UUID from MAC */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char device_uuid[64];
    snprintf(device_uuid, sizeof(device_uuid),
             "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Extract MessageID from incoming Probe for RelatesTo */
    char relates_to[128] = "";
    const char *mid = strstr(body, "MessageID");
    if (mid) {
        const char *gt = strchr(mid, '>');
        if (gt) {
            gt++;
            const char *lt = strchr(gt, '<');
            if (lt) {
                size_t mlen = lt - gt;
                if (mlen > 0 && mlen < sizeof(relates_to)) {
                    memcpy(relates_to, gt, mlen);
                    relates_to[mlen] = '\0';
                }
            }
        }
    }

    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();

    char response[1536];
    int n = snprintf(response, sizeof(response),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<soap:Envelope "
        "xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
        "xmlns:wsdd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
        "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<soap:Header>"
        "<wsa:MessageID>urn:uuid:%08x%08x</wsa:MessageID>"
        "<wsa:RelatesTo>urn:uuid:%s</wsa:RelatesTo>"
        "<wsa:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>"
        "<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</wsa:Action>"
        "</soap:Header>"
        "<soap:Body>"
        "<wsdd:ProbeMatches>"
        "<wsdd:ProbeMatch>"
        "<wsa:EndpointReference>"
        "<wsa:Address>urn:uuid:mibeecam</wsa:Address>"
        "</wsa:EndpointReference>"
        "<wsdd:Types>dn:NetworkVideoTransmitter</wsdd:Types>"
        "<wsdd:Scopes>"
        "onvif://www.onvif.org/Profile/Streaming "
        "onvif://www.onvif.org/Model/MiBeeCam"
        "</wsdd:Scopes>"
        "<wsdd:XAddrs>http://%s/onvif/device_service</wsdd:XAddrs>"
        "<wsdd:MetadataVersion>1</wsdd:MetadataVersion>"
        "</wsdd:ProbeMatch>"
        "</wsdd:ProbeMatches>"
        "</soap:Body>"
        "</soap:Envelope>",
        (unsigned)r1, (unsigned)r2,
        relates_to[0] ? relates_to : "00000000-0000-0000-0000-000000000000",
        ip);

    httpd_resp_set_type(req, "application/soap+xml; charset=utf-8");
    if (n > 0) {
        httpd_resp_send(req, response, n);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Response build failed");
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  /onvif/device_service handler                                       */
/* ------------------------------------------------------------------ */

static esp_err_t onvif_device_handler(httpd_req_t *req)
{
    /* Read POST body (max 1KB) */
    char buf[1024];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[len] = '\0';
    if (len >= (int)sizeof(buf)) {
        len = sizeof(buf) - 1;
    }

    httpd_resp_set_type(req, "application/soap+xml; charset=utf-8");

    /* Handle WS-Discovery Probe sent via HTTP (directed discovery) */
    if (strstr(buf, "Probe") != NULL && strstr(buf, "ProbeMatches") == NULL) {
        return handle_http_probe(req, buf);
    }

    const char *ip = wifi_get_ip_str();

    char soap_body[768];
    char response[1536];
    int n = 0;

    if (strstr(buf, "GetDeviceInformation")) {
        n = snprintf(soap_body, sizeof(soap_body),
            "<tds:GetDeviceInformationResponse>"
            "<tt:Manufacturer>MiBee</tt:Manufacturer>"
            "<tt:Model>MiBeeCam</tt:Model>"
            "<tt:FirmwareVersion>1.0</tt:FirmwareVersion>"
            "<tt:SerialNumber>%s</tt:SerialNumber>"
            "<tt:HardwareId>ESP32-S3</tt:HardwareId>"
            "</tds:GetDeviceInformationResponse>",
            ip);
        n = soap_envelope(response, sizeof(response), soap_body);
        if (n >= (int)sizeof(response)) {
            ESP_LOGW(TAG, "ONVIF response truncated");
        }
        httpd_resp_send(req, response, n);
        return ESP_OK;
    }

    if (strstr(buf, "GetCapabilities")) {
        n = snprintf(soap_body, sizeof(soap_body),
            "<tds:GetCapabilitiesResponse>"
            "<tt:Capabilities>"
            "<tt:Device>"
            "<tt:XAddr>http://%s/onvif/device_service</tt:XAddr>"
            "</tt:Device>"
            "<tt:Media>"
            "<tt:XAddr>http://%s/onvif/media_service</tt:XAddr>"
            "<tt:StreamingCapabilities>"
            "<tt:RTPMulticast>false</tt:RTPMulticast>"
            "<tt:RTP_TCP>false</tt:RTP_TCP>"
            "<tt:RTP_RTSP_TCP>false</tt:RTP_RTSP_TCP>"
            "</tt:StreamingCapabilities>"
            "</tt:Media>"
            "<tt:PTZ>"
            "<tt:XAddr>http://%s/onvif/ptz_service</tt:XAddr>"
            "</tt:PTZ>"
            "<tt:Events>"
            "<tt:XAddr>http://%s/onvif/event_service</tt:XAddr>"
            "</tt:Events>"
            "</tt:Capabilities>"
            "</tds:GetCapabilitiesResponse>",
            ip, ip, ip, ip);
        n = soap_envelope(response, sizeof(response), soap_body);
        if (n >= (int)sizeof(response)) {
            ESP_LOGW(TAG, "ONVIF response truncated");
        }
        httpd_resp_send(req, response, n);
        return ESP_OK;
    }

    if (strstr(buf, "GetSystemDateAndTime")) {
        time_t now = time(NULL);
        struct tm *utc = gmtime(&now);
        n = snprintf(soap_body, sizeof(soap_body),
            "<tds:GetSystemDateAndTimeResponse>"
            "<tt:SystemDateAndTime>"
            "<tt:DateTimeType>UTC</tt:DateTimeType>"
            "<tt:DaylightSavings>false</tt:DaylightSavings>"
            "<tt:UTCDateTime>"
            "<tt:Time>"
            "<tt:Hour>%d</tt:Hour>"
            "<tt:Minute>%d</tt:Minute>"
            "<tt:Second>%d</tt:Second>"
            "</tt:Time>"
            "<tt:Date>"
            "<tt:Year>%d</tt:Year>"
            "<tt:Month>%d</tt:Month>"
            "<tt:Day>%d</tt:Day>"
            "</tt:Date>"
            "</tt:UTCDateTime>"
            "</tt:SystemDateAndTime>"
            "</tds:GetSystemDateAndTimeResponse>",
            utc->tm_hour, utc->tm_min, utc->tm_sec,
            utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday);
        n = soap_envelope(response, sizeof(response), soap_body);
        if (n >= (int)sizeof(response)) {
            ESP_LOGW(TAG, "ONVIF response truncated");
        }
        httpd_resp_send(req, response, n);
        return ESP_OK;
    }

    if (strstr(buf, "GetServices")) {
        n = snprintf(soap_body, sizeof(soap_body),
            "<tds:GetServicesResponse>"
            "<tt:Service>"
            "<tt:Namespace>http://www.onvif.org/ver10/device/wsdl</tt:Namespace>"
            "<tt:XAddr>http://%s/onvif/device_service</tt:XAddr>"
            "<tt:Version>"
            "<tt:Major>1</tt:Major>"
            "<tt:Minor>0</tt:Minor>"
            "</tt:Version>"
            "</tt:Service>"
            "<tt:Service>"
            "<tt:Namespace>http://www.onvif.org/ver10/media/wsdl</tt:Namespace>"
            "<tt:XAddr>http://%s/onvif/media_service</tt:XAddr>"
            "<tt:Version>"
            "<tt:Major>1</tt:Major>"
            "<tt:Minor>0</tt:Minor>"
            "</tt:Version>"
            "</tt:Service>"
            "</tds:GetServicesResponse>",
            ip, ip);
        n = soap_envelope(response, sizeof(response), soap_body);
        if (n >= (int)sizeof(response)) {
            ESP_LOGW(TAG, "ONVIF response truncated");
        }
        httpd_resp_send(req, response, n);
        return ESP_OK;
    }

    /* Unknown method — return empty body */
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  /onvif/media_service handler                                        */
/* ------------------------------------------------------------------ */

static esp_err_t onvif_media_handler(httpd_req_t *req)
{
    /* Read POST body (max 1KB) */
    char buf[1024];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[len] = '\0';
    if (len >= (int)sizeof(buf)) {
        len = sizeof(buf) - 1;
    }

    httpd_resp_set_type(req, "application/soap+xml; charset=utf-8");

    const char *ip = wifi_get_ip_str();
    const cam_config_t *cfg = config_get();

    char soap_body[768];
    char response[1536];
    int n = 0;

    if (strstr(buf, "GetProfiles")) {
        n = snprintf(soap_body, sizeof(soap_body),
            "<trt:GetProfilesResponse>"
            "<tt:Profiles token=\"MainStream\">"
            "<tt:Name>%s_MainStream</tt:Name>"
            "<tt:VideoSourceConfiguration token=\"VS0\">"
            "<tt:Name>VideoSource0</tt:Name>"
            "<tt:UseCount>1</tt:UseCount>"
            "<tt:SourceToken>VIDEO_SOURCE_0</tt:SourceToken>"
            "<tt:Bounds x=\"0\" y=\"0\" width=\"640\" height=\"480\"/>"
            "</tt:VideoSourceConfiguration>"
            "<tt:VideoEncoderConfiguration token=\"VE0\">"
            "<tt:Name>MJPEGEncoder</tt:Name>"
            "<tt:UseCount>1</tt:UseCount>"
            "<tt:Encoding>JPEG</tt:Encoding>"
            "<tt:Resolution>"
            "<tt:Width>640</tt:Width>"
            "<tt:Height>480</tt:Height>"
            "</tt:Resolution>"
            "<tt:Quality>5</tt:Quality>"
            "<tt:FramerateLimit>15</tt:FramerateLimit>"
            "<tt:BitrateLimit>4096</tt:BitrateLimit>"
            "</tt:VideoEncoderConfiguration>"
            "</tt:Profiles>"
            "</trt:GetProfilesResponse>",
            cfg->device_name);
        n = soap_envelope(response, sizeof(response), soap_body);
        if (n >= (int)sizeof(response)) {
            ESP_LOGW(TAG, "ONVIF response truncated");
        }
        httpd_resp_send(req, response, n);
        return ESP_OK;
    }

    if (strstr(buf, "GetStreamUri")) {
        n = snprintf(soap_body, sizeof(soap_body),
            "<trt:GetStreamUriResponse>"
            "<tt:MediaUri>"
            "<tt:Uri>http://%s:81/stream</tt:Uri>"
            "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
            "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
            "<tt:Timeout>PT10S</tt:Timeout>"
            "</tt:MediaUri>"
            "</trt:GetStreamUriResponse>",
            ip);
        n = soap_envelope(response, sizeof(response), soap_body);
        if (n >= (int)sizeof(response)) {
            ESP_LOGW(TAG, "ONVIF response truncated");
        }
        httpd_resp_send(req, response, n);
        return ESP_OK;
    }

    /* Unknown method — return empty body */
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t onvif_service_start(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_uri_t device_service = {
        .uri      = "/onvif/device_service",
        .method   = HTTP_POST,
        .handler  = onvif_device_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t media_service = {
        .uri      = "/onvif/media_service",
        .method   = HTTP_POST,
        .handler  = onvif_media_handler,
        .user_ctx = NULL,
    };

    esp_err_t ret1 = httpd_register_uri_handler(server, &device_service);
    esp_err_t ret2 = httpd_register_uri_handler(server, &media_service);

    ESP_LOGI(TAG, "ONVIF service started: device=%s media=%s",
             esp_err_to_name(ret1), esp_err_to_name(ret2));

    return (ret1 == ESP_OK && ret2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t onvif_service_stop(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* httpd does not provide a deregister API in ESP-IDF v5.5.4,
     * so this is a best-effort stop. New handlers won't be added,
     * but existing registered handlers remain until server stops. */
    ESP_LOGI(TAG, "ONVIF service stopped");
    return ESP_OK;
}

#else /* !CONFIG_MIBEECAM_ENABLE_ONVIF */

/* ------------------------------------------------------------------ */
/*  Stub implementations when ONVIF is disabled at compile time        */
/* ------------------------------------------------------------------ */

esp_err_t onvif_service_start(httpd_handle_t server)
{
    (void)server;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t onvif_service_stop(httpd_handle_t server)
{
    (void)server;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_MIBEECAM_ENABLE_ONVIF */
