/**
 * @file mjpeg_streamer.c
 * @brief MJPEG real-time streaming — independent TCP server on port 81.
 *
 * Architecture:
 *   mjpeg_streamer_init()
 *   mjpeg_streamer_start() — create listen socket, spawn listen task (Core 1)
 *     mjpeg_listen_task — accept() loop
 *       for each client: spawn mjpeg_client_task (Core 1, max 2 total)
 *         read HTTP GET /stream
 *         send multipart/x-mixed-replace MJPEG stream
 *         fbroadcast_acquire_latest + camera_capture fallback
 *         cleanup on disconnect
 *   mjpeg_streamer_stop() — close sockets, all tasks exit
 */

#include "mjpeg_streamer.h"
#include "esp_log.h"
#ifdef CONFIG_MIBEECAM_ENABLE_FRAME_BROADCASTER
#include "frame_broadcaster.h"
#endif
#include "camera_driver.h"
#include "config_manager.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <errno.h>

static const char *TAG = "mjpeg";

/* ---------- Stream protocol constants ---------- */

#define STREAM_PORT        81
#define PART_BOUNDARY      "123456789000000000000987654321"
#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=" PART_BOUNDARY
#define STREAM_BOUNDARY    "\r\n--" PART_BOUNDARY "\r\n" \
                           "Content-Type: image/jpeg\r\n" \
                           "Content-Length: %u\r\n\r\n"
#define MAX_STREAM_CLIENTS 2
#define CHUNK_SIZE         4096
#define LISTEN_BACKLOG     5
#define CLIENT_TASK_STACK  4096
#define SEND_TIMEOUT_MS    5000

/* ---------- Module state ---------- */

static int               s_client_count   = 0;
static int               s_client_socks[MAX_STREAM_CLIENTS];
static SemaphoreHandle_t s_mutex          = NULL;
static TaskHandle_t      s_listen_task    = NULL;
static int               s_listen_sock    = -1;
static volatile bool     s_running        = false;

/* ---------- Forward declarations ---------- */

static void mjpeg_listen_task(void *arg);
static void mjpeg_client_task(void *arg);

/* ---------- Internal helpers ---------- */

/**
 * @brief Send a JPEG frame as a multipart chunk over a raw socket.
 * @param sock  Connected socket fd.
 * @param buf   JPEG buffer pointer.
 * @param len   JPEG buffer length.
 * @return true on success, false if send failed (client disconnected).
 */
static bool send_jpeg_frame(int sock, const uint8_t *buf, size_t len)
{
    char part_hdr[128];
    int hdrlen = snprintf(part_hdr, sizeof(part_hdr),
                          STREAM_BOUNDARY, (unsigned int)len);

    /* Send part header */
    if (send(sock, part_hdr, hdrlen, 0) != hdrlen) {
        return false;
    }

    /* Send JPEG body in CHUNK_SIZE pieces */
    size_t remaining = len;
    const uint8_t *ptr = buf;
    while (remaining > 0) {
        size_t chunk = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
        int sent = send(sock, (const char *)ptr, chunk, 0);
        if (sent <= 0) {
            return false;
        }
        ptr      += sent;
        remaining -= sent;
    }

    /* Trailing CRLF */
    if (send(sock, "\r\n", 2, 0) != 2) {
        return false;
    }

    return true;
}

/* ---------- Client task — serves one MJPEG stream connection ---------- */

static void mjpeg_client_task(void *arg)
{
    int client_sock = (int)(intptr_t)arg;

    /* Set send + recv timeout so a stuck client does not hang the task */
    struct timeval tv = {
        .tv_sec  = SEND_TIMEOUT_MS / 1000,
        .tv_usec = (SEND_TIMEOUT_MS % 1000) * 1000
    };
    setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Read HTTP request (first 511 bytes is enough to validate) */
    char req_buf[512];
    int req_len = recv(client_sock, req_buf, sizeof(req_buf) - 1, 0);
    if (req_len <= 0) {
        ESP_LOGW(TAG, "Failed to read HTTP request from stream client");
        close(client_sock);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_count--;
        for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
            if (s_client_socks[i] == client_sock) {
                s_client_socks[i] = -1;
                break;
            }
        }
        xSemaphoreGive(s_mutex);
        vTaskDelete(NULL);
        return;
    }
    req_buf[req_len] = '\0';

    /* Validate: must be GET /stream (accept /stream?xxx too) */
    if (strncmp(req_buf, "GET /stream", 11) != 0) {
        ESP_LOGW(TAG, "Unexpected stream request: %.60s", req_buf);
        const char *resp = "HTTP/1.1 400 Bad Request\r\n"
                           "Content-Length: 0\r\n"
                           "Connection: close\r\n\r\n";
        send(client_sock, resp, strlen(resp), 0);
        close(client_sock);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_count--;
        for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
            if (s_client_socks[i] == client_sock) {
                s_client_socks[i] = -1;
                break;
            }
        }
        xSemaphoreGive(s_mutex);
        vTaskDelete(NULL);
        return;
    }

    /* Send HTTP 200 + multipart/x-mixed-replace headers */
    char headers[512];
    int hdr_len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: " STREAM_CONTENT_TYPE "\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n");

    if (send(client_sock, headers, hdr_len, 0) != hdr_len) {
        ESP_LOGW(TAG, "Failed to send stream response headers");
        close(client_sock);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_count--;
        for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
            if (s_client_socks[i] == client_sock) {
                s_client_socks[i] = -1;
                break;
            }
        }
        xSemaphoreGive(s_mutex);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Stream client connected (total %d/%d)",
             s_client_count, MAX_STREAM_CLIENTS);

    /* Publish client connected event */
    event_t connect_event = {
        .type = EVENT_STREAM_CLIENT_CONNECTED,
        .timestamp = esp_timer_get_time(),
        .payload = NULL,
        .payload_len = 0,
    };
    event_bus_publish(&connect_event);

    /* ---- Stream loop ---- */
    int consecutive_failures = 0;

    while (1) {
#ifdef CONFIG_MIBEECAM_ENABLE_FRAME_BROADCASTER
        /* Try broadcaster first (non-blocking, returns NOT_FOUND if no frame) */
        frame_ref_t *frame_ref = NULL;
        if (fbroadcast_acquire_latest(&frame_ref) == ESP_OK && frame_ref != NULL) {
            if (!send_jpeg_frame(client_sock, frame_ref->buf, frame_ref->len)) {
                fbroadcast_release(frame_ref);
                break;
            }
            fbroadcast_release(frame_ref);
            consecutive_failures = 0;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
#endif
        /* Fallback: direct camera capture with retry */
        camera_fb_t *fb = NULL;
        esp_err_t ret;
        int retries;
        for (retries = 0; retries < 3; retries++) {
            ret = camera_capture(&fb);
            if (ret == ESP_OK) break;
            ESP_LOGD(TAG, "Capture retry %d/3", retries + 1);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (ret != ESP_OK || fb == NULL) {
            consecutive_failures++;
            if (consecutive_failures >= 10) {
                ESP_LOGE(TAG, "Camera capture failed %d consecutive times, ending stream",
                         consecutive_failures);
                break;
            }
            ESP_LOGW(TAG, "Camera capture failed (%d/10 consecutive), retrying...",
                     consecutive_failures);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        consecutive_failures = 0;

        if (!send_jpeg_frame(client_sock, fb->buf, fb->len)) {
            camera_return_fb(fb);
            break;
        }
        camera_return_fb(fb);

        /* Brief yield for watchdog and task switching */
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    /* ---- Cleanup ---- */

    /* Send closing boundary (best-effort) */
    send(client_sock, "\r\n--" PART_BOUNDARY "--\r\n",
         strlen(PART_BOUNDARY) + 8, 0);

    close(client_sock);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_client_count--;
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        if (s_client_socks[i] == client_sock) {
            s_client_socks[i] = -1;
            break;
        }
    }
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Stream client disconnected (total %d)", s_client_count);

    /* Publish client disconnected event */
    event_t disconnect_event = {
        .type = EVENT_STREAM_CLIENT_DISCONNECTED,
        .timestamp = esp_timer_get_time(),
        .payload = NULL,
        .payload_len = 0,
    };
    event_bus_publish(&disconnect_event);

    vTaskDelete(NULL);
}

/* ---------- Listen task — accepts connections, spawns client tasks ---------- */

static void mjpeg_listen_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Listen task started on port %d", STREAM_PORT);

    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(s_listen_sock,
                                 (struct sockaddr *)&client_addr,
                                 &addr_len);
        if (client_sock < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (!s_running) break;
            ESP_LOGE(TAG, "accept() failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Enforce client limit */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_client_count >= MAX_STREAM_CLIENTS) {
            xSemaphoreGive(s_mutex);
            ESP_LOGW(TAG, "Max stream clients (%d) reached, rejecting",
                     MAX_STREAM_CLIENTS);
            const char *reject = "HTTP/1.1 503 Service Unavailable\r\n"
                                 "Content-Length: 25\r\n\r\nMax stream connections\r\n";
            send(client_sock, reject, strlen(reject), 0);
            close(client_sock);
            continue;
        }

        /* Find free slot in client socket tracking array */
        int slot = -1;
        for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
            if (s_client_socks[i] < 0) {
                s_client_socks[i] = client_sock;
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            /* Should not happen since client_count < MAX, but defensive */
            xSemaphoreGive(s_mutex);
            ESP_LOGW(TAG, "No free slot for stream client");
            const char *reject = "HTTP/1.1 503 Service Unavailable\r\n"
                                 "Content-Length: 16\r\n\r\nServer busy\r\n";
            send(client_sock, reject, strlen(reject), 0);
            close(client_sock);
            continue;
        }
        s_client_count++;
        xSemaphoreGive(s_mutex);

        /* Spawn a dedicated client task (Core 1, priority 2) */
        BaseType_t created = xTaskCreatePinnedToCore(
            mjpeg_client_task,
            "mjpeg_cli",
            CLIENT_TASK_STACK,
            (void *)(intptr_t)client_sock,
            2,
            NULL,
            1);

        if (created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create client task");
            close(client_sock);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_client_count--;
            for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
                if (s_client_socks[i] == client_sock) {
                    s_client_socks[i] = -1;
                    break;
                }
            }
            xSemaphoreGive(s_mutex);
        }
    }

    ESP_LOGI(TAG, "Listen task exiting");
    vTaskDelete(NULL);
}

/* ---------- Public API ---------- */

esp_err_t mjpeg_streamer_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Initialize client socket tracking */
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        s_client_socks[i] = -1;
    }
    s_client_count = 0;
    s_listen_task = NULL;
    s_listen_sock = -1;
    s_running = false;

    ESP_LOGI(TAG, "MJPEG streamer initialized (max %d clients)", MAX_STREAM_CLIENTS);
    return ESP_OK;
}

esp_err_t mjpeg_streamer_start(void)
{
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Cannot start: not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_listen_task != NULL) {
        ESP_LOGW(TAG, "MJPEG streamer already started");
        return ESP_OK;
    }

    /* Create TCP listen socket */
    s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_sock < 0) {
        ESP_LOGE(TAG, "Failed to create listen socket: errno %d", errno);
        return ESP_FAIL;
    }

    int opt = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(STREAM_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "Failed to bind port %d: errno %d", STREAM_PORT, errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        return ESP_FAIL;
    }

    if (listen(s_listen_sock, LISTEN_BACKLOG) != 0) {
        ESP_LOGE(TAG, "Failed to listen on port %d: errno %d", STREAM_PORT, errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        return ESP_FAIL;
    }

    s_running = true;

    /* Spawn listen task on Core 1 */
    BaseType_t created = xTaskCreatePinnedToCore(
        mjpeg_listen_task,
        "mjpeg_listen",
        4096,     /* listen task: accept() + xTaskCreate needs ~3KB */
        NULL,
        3,      /* slightly higher than client tasks */
        &s_listen_task,
        1);     /* Core 1 */

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create listen task");
        close(s_listen_sock);
        s_listen_sock = -1;
        s_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "MJPEG streamer started on port %d", STREAM_PORT);
    return ESP_OK;
}

void mjpeg_streamer_stop(void)
{
    s_running = false;

    /* Close listen socket to unblock accept() */
    if (s_listen_sock >= 0) {
        close(s_listen_sock);
        s_listen_sock = -1;
    }

    /* Close all tracked client sockets (tasks will get send errors and exit) */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        if (s_client_socks[i] >= 0) {
            close(s_client_socks[i]);
            s_client_socks[i] = -1;
        }
    }
    s_client_count = 0;
    xSemaphoreGive(s_mutex);

    /* Small delay for tasks to react to closed sockets */
    vTaskDelay(pdMS_TO_TICKS(50));
    s_listen_task = NULL;

    ESP_LOGI(TAG, "MJPEG streamer stopped");
}

int mjpeg_streamer_get_client_count(void)
{
    int count;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    count = s_client_count;
    xSemaphoreGive(s_mutex);
    return count;
}
