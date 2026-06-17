/*
 * onvif_discovery.c - ONVIF WS-Discovery UDP listener
 *
 * Listens for WS-Discovery Probe messages on multicast 239.255.255.250:3702
 * and responds with ProbeMatches containing the device's XAddrs.
 * Also sends periodic WS-Discovery Hello announcements on startup and every 30s.
 *
 * Default disabled. Enable via CONFIG_MIBEECAM_ENABLE_ONVIF compile flag.
 * When disabled, all public functions return ESP_ERR_NOT_SUPPORTED / false.
 */
#include "onvif_discovery.h"

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "wifi_manager.h"

#ifdef CONFIG_MIBEECAM_ENABLE_ONVIF

static const char *TAG = "onvif_d";

#define ONVIF_MULTICAST_ADDR "239.255.255.250"
#define ONVIF_PORT            3702
#define ONVIF_BUF_SIZE        2048
#define RESP_BUF_SIZE         2048
#define SELECT_TIMEOUT_MS     100
#define HELLO_INTERVAL_S      30

static TaskHandle_t s_onvif_task = NULL;
static int s_sock = -1;
static volatile bool s_running = false;

/* ProbeMatches SOAP response template.
 * Format args: %08x%08x (MessageID random), %s (RelatesTo UUID),
 *              %s (device IP for XAddrs) */
static const char *PROBE_MATCHES_TEMPLATE =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<soap:Envelope "
    "xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\"\r\n"
    " xmlns:wsdd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\"\r\n"
    " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">\r\n"
    "<soap:Header>\r\n"
    "<wsa:MessageID>urn:uuid:%08x%08x</wsa:MessageID>\r\n"
    "<wsa:RelatesTo>urn:uuid:%s</wsa:RelatesTo>\r\n"
    "<wsa:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous"
    "</wsa:To>\r\n"
    "<wsa:Action>"
    "http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches"
    "</wsa:Action>\r\n"
    "</soap:Header>\r\n"
    "<soap:Body>\r\n"
    "<wsdd:ProbeMatches>\r\n"
    "<wsdd:ProbeMatch>\r\n"
    "<wsa:EndpointReference>\r\n"
    "<wsa:Address>urn:uuid:mibeecam</wsa:Address>\r\n"
    "</wsa:EndpointReference>\r\n"
    "<wsdd:Types>dn:NetworkVideoTransmitter</wsdd:Types>\r\n"
    "<wsdd:Scopes>"
    "onvif://www.onvif.org/Profile/Streaming "
    "onvif://www.onvif.org/Model/MiBeeCam"
    "</wsdd:Scopes>\r\n"
    "<wsdd:XAddrs>http://%s/onvif/device_service</wsdd:XAddrs>\r\n"
    "<wsdd:MetadataVersion>1</wsdd:MetadataVersion>\r\n"
    "</wsdd:ProbeMatch>\r\n"
    "</wsdd:ProbeMatches>\r\n"
    "</soap:Body>\r\n"
    "</soap:Envelope>\r\n";

/* WS-Discovery Hello SOAP envelope template.
 * Format args: %08x%08x (MessageID random), %s (device IP for XAddrs) */
static const char *HELLO_TEMPLATE =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<soap:Envelope "
    "xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\"\r\n"
    " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\"\r\n"
    " xmlns:wsdd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\"\r\n"
    " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">\r\n"
    "<soap:Header>\r\n"
    "<wsa:Action>"
    "http://schemas.xmlsoap.org/ws/2005/04/discovery/Hello"
    "</wsa:Action>\r\n"
    "<wsa:MessageID>urn:uuid:%08x%08x</wsa:MessageID>\r\n"
    "<wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>\r\n"
    "<wsa:From>\r\n"
    "<wsa:Address>urn:uuid:mibeecam</wsa:Address>\r\n"
    "</wsa:From>\r\n"
    "</soap:Header>\r\n"
    "<soap:Body>\r\n"
    "<wsdd:Hello>\r\n"
    "<wsa:EndpointReference>\r\n"
    "<wsa:Address>urn:uuid:mibeecam</wsa:Address>\r\n"
    "</wsa:EndpointReference>\r\n"
    "<wsdd:Types>dn:NetworkVideoTransmitter</wsdd:Types>\r\n"
    "<wsdd:Scopes>"
    "onvif://www.onvif.org/Profile/Streaming "
    "onvif://www.onvif.org/Model/MiBeeCam"
    "</wsdd:Scopes>\r\n"
    "<wsdd:XAddrs>http://%s/onvif/device_service</wsdd:XAddrs>\r\n"
    "<wsdd:MetadataVersion>1</wsdd:MetadataVersion>\r\n"
    "</wsdd:Hello>\r\n"
    "</soap:Body>\r\n"
    "</soap:Envelope>\r\n";

/**
 * @brief Send a WS-Discovery Hello multicast announcement.
 * Sets multicast interface, builds the Hello message, and sends to
 * the WS-Discovery multicast group (239.255.255.250:3702).
 * Reuses the provided buffer for building the message.
 */
static void send_hello_multicast(int sock, const char *local_ip,
                                  char *buf, size_t buf_size)
{
    struct in_addr mc_if;
    mc_if.s_addr = inet_addr(local_ip);
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &mc_if, sizeof(mc_if));
    int mc_ttl = 2;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &mc_ttl, sizeof(mc_ttl));

    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    int hlen = snprintf(buf, buf_size,
                        HELLO_TEMPLATE,
                        (unsigned)r1, (unsigned)r2,
                        local_ip);
    if (hlen > 0 && hlen < (int)buf_size) {
        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(ONVIF_PORT);
        dest.sin_addr.s_addr = inet_addr(ONVIF_MULTICAST_ADDR);
        sendto(sock, buf, hlen, 0,
               (struct sockaddr *)&dest, sizeof(dest));
        ESP_LOGI(TAG, "Sent WS-Discovery Hello to %s:%d",
                 ONVIF_MULTICAST_ADDR, ONVIF_PORT);
    }
}

/**
 * @brief Extract the UUID portion from a Probe's MessageID element.
 *        Looks for <wsa:MessageID>...</wsa:MessageID> and strips
 *        the "urn:uuid:" prefix if present.
 * @param xml     Raw received XML data
 * @param buf     Output buffer for the extracted UUID
 * @param buf_size Size of output buffer
 * @return Pointer to buf, or NULL if MessageID not found
 */
static const char* extract_message_id(const char *xml,
                                       char *buf, size_t buf_size)
{
    const char *start = strstr(xml, "<wsa:MessageID>");
    if (!start) {
        return NULL;
    }
    start += strlen("<wsa:MessageID>");

    const char *end = strstr(start, "</wsa:MessageID>");
    if (!end) {
        return NULL;
    }

    size_t len = end - start;
    if (len >= buf_size) {
        len = buf_size - 1;
    }

    memcpy(buf, start, len);
    buf[len] = '\0';

    /* Strip "urn:uuid:" prefix if present */
    const char *prefix = "urn:uuid:";
    const char *uuid = strstr(buf, prefix);
    if (uuid) {
        uuid += strlen(prefix);
        size_t uuid_len = strlen(uuid);
        memmove(buf, uuid, uuid_len + 1);
    }

    return buf;
}

static void onvif_discovery_task(void *pvParameters)
{
    struct sockaddr_in addr;
    struct ip_mreq mreq;
    int ret;

    /* Allocate buffers on heap — 2x2KB exceeds 4KB task stack */
    char *recv_buf = (char *)malloc(ONVIF_BUF_SIZE);
    char *resp_buf = (char *)malloc(RESP_BUF_SIZE);
    if (!recv_buf || !resp_buf) {
        ESP_LOGE(TAG, "Failed to allocate ONVIF buffers");
        free(recv_buf);
        free(resp_buf);
        s_onvif_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* Create UDP socket */
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        free(recv_buf);
        free(resp_buf);
        s_sock = -1;
        s_onvif_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* Allow address reuse for bind */
    int reuse = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* Bind to ONVIF port on any interface */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ONVIF_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    ret = bind(s_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to bind socket to port %d: errno %d",
                 ONVIF_PORT, errno);
        close(s_sock);
        s_sock = -1;
        free(recv_buf);
        free(resp_buf);
        s_onvif_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* Join WS-Discovery multicast group */
    mreq.imr_multiaddr.s_addr = inet_addr(ONVIF_MULTICAST_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    ret = setsockopt(s_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                     &mreq, sizeof(mreq));
    if (ret < 0) {
        ESP_LOGW(TAG, "IP_ADD_MEMBERSHIP failed (network may not be ready): "
                 "errno %d", errno);
    }

    ESP_LOGI(TAG, "ONVIF discovery listening on %s:%d",
             ONVIF_MULTICAST_ADDR, ONVIF_PORT);

    /* Send initial WS-Discovery Hello to announce device to NVRs */
    {
        char *local_ip = wifi_get_ip_str();
        if (local_ip && strcmp(local_ip, "0.0.0.0") != 0) {
            send_hello_multicast(s_sock, local_ip, resp_buf, RESP_BUF_SIZE);
        }
    }

    char relates_to[128];
    TickType_t last_hello_tick = xTaskGetTickCount();
    s_running = true;

    while (s_running) {
        fd_set readfds;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(s_sock, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = SELECT_TIMEOUT_MS * 1000;

        ret = select(s_sock + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "select() error: errno %d", errno);
            break;
        }
        if (ret == 0) {
            /* Check if it's time to send periodic Hello */
            TickType_t now = xTaskGetTickCount();
            if (now - last_hello_tick >= pdMS_TO_TICKS(HELLO_INTERVAL_S * 1000)) {
                last_hello_tick = now;
                char *local_ip = wifi_get_ip_str();
                if (local_ip && strcmp(local_ip, "0.0.0.0") != 0) {
                    send_hello_multicast(s_sock, local_ip, resp_buf, RESP_BUF_SIZE);
                }
            }
            continue;
        }

        struct sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);
        memset(recv_buf, 0, ONVIF_BUF_SIZE);

        int len = recvfrom(s_sock, recv_buf, ONVIF_BUF_SIZE - 1, 0,
                           (struct sockaddr *)&src_addr, &src_len);
        if (len <= 0) {
            if (errno == EINTR) continue;
            ESP_LOGD(TAG, "recvfrom() returned %d, errno %d", len, errno);
            continue;
        }
        recv_buf[len] = '\0';

        if (strstr(recv_buf, "Probe") == NULL) continue;

        ESP_LOGD(TAG, "Received Probe from %s (%d bytes)",
                 inet_ntoa(src_addr.sin_addr), len);

        const char *ip = wifi_get_ip_str();
        if (ip == NULL || strcmp(ip, "0.0.0.0") == 0) {
            ESP_LOGD(TAG, "No valid IP yet, skipping ProbeMatch");
            continue;
        }

        relates_to[0] = '\0';
        if (extract_message_id(recv_buf, relates_to, sizeof(relates_to)) == NULL) {
            strcpy(relates_to, "00000000-0000-0000-0000-000000000000");
        }

        uint32_t r1 = esp_random();
        uint32_t r2 = esp_random();
        int resp_len = snprintf(resp_buf, RESP_BUF_SIZE,
                                PROBE_MATCHES_TEMPLATE,
                                (unsigned)r1, (unsigned)r2,
                                relates_to, ip);
        if (resp_len >= (int)RESP_BUF_SIZE) {
            resp_len = (int)RESP_BUF_SIZE - 1;
        }

        ret = sendto(s_sock, resp_buf, resp_len, 0,
                     (struct sockaddr *)&src_addr, sizeof(src_addr));
        if (ret < 0) {
            ESP_LOGW(TAG, "sendto() failed: errno %d", errno);
        } else {
            ESP_LOGI(TAG, "ProbeMatch sent to %s",
                     inet_ntoa(src_addr.sin_addr));
        }
    }

    /* Cleanup */
    free(recv_buf);
    free(resp_buf);
    if (s_sock >= 0) {
        struct ip_mreq leave;
        leave.imr_multiaddr.s_addr = inet_addr(ONVIF_MULTICAST_ADDR);
        leave.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(s_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                   &leave, sizeof(leave));
        close(s_sock);
        s_sock = -1;
    }
    ESP_LOGI(TAG, "ONVIF discovery stopped");
    s_running = false;
    s_onvif_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t onvif_discovery_start(void)
{
    if (s_onvif_task != NULL) {
        ESP_LOGW(TAG, "ONVIF discovery already running");
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        onvif_discovery_task,
        "onvif_d",
        4096,
        NULL,
        3,      /* Priority 3 */
        &s_onvif_task,
        1       /* Core 1 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ONVIF discovery task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ONVIF discovery started");
    return ESP_OK;
}

esp_err_t onvif_discovery_stop(void)
{
    if (s_onvif_task == NULL) {
        return ESP_OK;
    }

    s_running = false;

    /* Close socket to immediately wake select() */
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }

    ESP_LOGI(TAG, "ONVIF discovery stopping");
    return ESP_OK;
}

bool onvif_discovery_is_running(void)
{
    return (s_onvif_task != NULL);
}

#else /* !CONFIG_MIBEECAM_ENABLE_ONVIF */

/* ------------------------------------------------------------------ */
/*  Stub implementations when ONVIF is disabled at compile time        */
/* ------------------------------------------------------------------ */

esp_err_t onvif_discovery_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t onvif_discovery_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool onvif_discovery_is_running(void)
{
    return false;
}

#endif /* CONFIG_MIBEECAM_ENABLE_ONVIF */
