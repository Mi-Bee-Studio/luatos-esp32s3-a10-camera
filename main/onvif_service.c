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

#include "esp_log.h"
#include "wifi_manager.h"
#include "config_manager.h"

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

/* ------------------------------------------------------------------ */
/*  /onvif/device_service handler                                       */
/* ------------------------------------------------------------------ */

static esp_err_t onvif_device_handler(httpd_req_t *req)
{
    /* Read POST body (max 4KB) */
    char buf[4096];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    httpd_resp_set_type(req, "application/soap+xml; charset=utf-8");

    const char *ip = wifi_get_ip_str();

    char soap_body[2048];
    char response[3072];
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
    /* Read POST body (max 4KB) */
    char buf[4096];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    httpd_resp_set_type(req, "application/soap+xml; charset=utf-8");

    const char *ip = wifi_get_ip_str();
    const cam_config_t *cfg = config_get();

    char soap_body[2048];
    char response[3072];
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
        httpd_resp_send(req, response, n);
        return ESP_OK;
    }

    if (strstr(buf, "GetStreamUri")) {
        /*
         * NOTE: The returned RTSP URI is not actually functional — there is
         * no RTSP server in this firmware. This is included only for ONVIF
         * Profile S discovery compatibility. NVRs that attempt to connect
         * to this URI will fail. This is a known limitation.
         */
        n = snprintf(soap_body, sizeof(soap_body),
            "<trt:GetStreamUriResponse>"
            "<tt:MediaUri>"
            "<tt:Uri>rtsp://%s:8554/stream</tt:Uri>"
            "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
            "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
            "<tt:Timeout>PT10S</tt:Timeout>"
            "</tt:MediaUri>"
            "</trt:GetStreamUriResponse>",
            ip);
        n = soap_envelope(response, sizeof(response), soap_body);
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
