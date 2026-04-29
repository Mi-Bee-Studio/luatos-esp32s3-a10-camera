/*
 * motion_detect.c - Frame-difference motion detection with remote image upload
 *
 * Algorithm:
 *   1. Capture frame A
 *   2. Wait ~500ms
 *   3. Capture frame B
 *   4. Compare raw JPEG bytes (sampled) for differences
 *   5. If difference exceeds threshold → motion detected
 *   6. On trigger (with cooldown): capture fresh frame → HTTP POST upload
 *   7. Retry upload up to 3 times with 2s interval
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "motion_detect.h"
#include "camera_driver.h"
#include "config_manager.h"
#include "mjpeg_streamer.h"

static const char *TAG = "motion_detect";

/* Sampling parameters for JPEG byte comparison */
#define SAMPLE_STEP    10
#define PIXEL_DELTA    20

/* Upload retry */
#define UPLOAD_MAX_RETRIES 3
#define UPLOAD_RETRY_DELAY_MS 2000

/* Task parameters */
#define MOTION_TASK_PRIORITY  5
#define MOTION_TASK_STACK_SIZE 8192

/* Static state */
static TaskHandle_t s_motion_task_handle = NULL;
static volatile bool s_running = false;
static bool s_in_cooldown = false;
static int64_t s_cooldown_start_us = 0;

/* ---- Internal helpers ---- */

/**
 * @brief Upload a JPEG buffer to the configured server via HTTP POST.
 *
 * POST {server_url}/upload
 * Content-Type: image/jpeg
 * X-Device-ID: {device_name}
 *
 * @param jpeg_buf  Pointer to JPEG data
 * @param jpeg_len  Length of JPEG data in bytes
 * @return ESP_OK on success (HTTP 200), ESP_FAIL otherwise
 */
static esp_err_t upload_image(const uint8_t *jpeg_buf, size_t jpeg_len)
{
    const cam_config_t *cfg = config_get();
    if (cfg->server_url[0] == '\0') {
        ESP_LOGW(TAG, "No server URL configured, skipping upload");
        return ESP_ERR_INVALID_STATE;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/upload", cfg->server_url);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    esp_http_client_set_header(client, "X-Device-ID", cfg->device_name);
    esp_http_client_set_post_field(client, (const char *)jpeg_buf, (int)jpeg_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 200) {
        ESP_LOGI(TAG, "Image uploaded successfully (%u bytes)", (unsigned)jpeg_len);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Upload failed: HTTP %d, err=%s", status, esp_err_to_name(err));
    return ESP_FAIL;
}

/**
 * @brief Upload with retry logic (3 attempts, 2s interval).
 *
 * @param buf  Pointer to JPEG data
 * @param len  Length of JPEG data
 * @return ESP_OK if any attempt succeeded, ESP_FAIL if all failed
 */
static esp_err_t upload_with_retry(const uint8_t *buf, size_t len)
{
    for (int i = 0; i < UPLOAD_MAX_RETRIES; i++) {
        esp_err_t err = upload_image(buf, len);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Upload retry %d/%d", i + 1, UPLOAD_MAX_RETRIES);
        vTaskDelay(pdMS_TO_TICKS(UPLOAD_RETRY_DELAY_MS));
    }
    ESP_LOGE(TAG, "Upload failed after %d retries", UPLOAD_MAX_RETRIES);
    return ESP_FAIL;
}

/**
 * @brief FreeRTOS task entry point for motion detection loop.
 */
static void motion_detection_task(void *arg)
{
    ESP_LOGI(TAG, "Motion detection task started");

    while (s_running) {
        /* Skip motion detection while MJPEG stream is active */
        if (mjpeg_streamer_get_client_count() > 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* Wait for camera to be ready */
        if (!camera_is_initialized()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const cam_config_t *cfg = config_get();

        /* --- Single-buffer-safe capture sequence ---
         * With fb_count=1 we cannot hold two frame buffers simultaneously.
         * Strategy: sample bytes from fb_a into a small malloc'd array,
         * return fb_a immediately, then capture fb_b and compare.
         */

        /* Capture reference frame */
        camera_fb_t *fb_a = NULL;
        if (camera_capture(&fb_a) != ESP_OK || fb_a == NULL) {
            ESP_LOGW(TAG, "Failed to capture reference frame");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Sample bytes from fb_a for later comparison (every SAMPLE_STEP bytes) */
        size_t a_len = fb_a->len;
        size_t sample_count = (a_len + SAMPLE_STEP - 1) / SAMPLE_STEP;
        uint8_t *samples_a = NULL;
        if (sample_count > 0) {
            samples_a = (uint8_t *)malloc(sample_count);
        }
        if (samples_a == NULL) {
            ESP_LOGW(TAG, "Failed to allocate sample buffer (%u bytes)", (unsigned)sample_count);
            camera_return_fb(fb_a);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        for (size_t i = 0, j = 0; i < a_len && j < sample_count; i += SAMPLE_STEP, j++) {
            samples_a[j] = fb_a->buf[i];
        }

        /* Return fb_a IMMEDIATELY — free the frame buffer for other tasks */
        camera_return_fb(fb_a);
        fb_a = NULL;

        /* Wait between captures for detectable change */
        vTaskDelay(pdMS_TO_TICKS(500));

        /* Capture comparison frame */
        camera_fb_t *fb_b = NULL;
        if (camera_capture(&fb_b) != ESP_OK || fb_b == NULL) {
            ESP_LOGW(TAG, "Failed to capture comparison frame");
            free(samples_a);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Compare saved samples against fb_b */
        size_t min_len = (a_len < fb_b->len) ? a_len : fb_b->len;
        size_t total = 0;
        size_t changed = 0;
        for (size_t i = 0, j = 0; i < min_len && j < sample_count; i += SAMPLE_STEP, j++) {
            total++;
            if (abs((int)samples_a[j] - (int)fb_b->buf[i]) > PIXEL_DELTA) {
                changed++;
            }
        }
        bool motion = false;
        if (total > 0) {
            uint8_t percent = (uint8_t)((changed * 100) / total);
            motion = percent >= cfg->motion_threshold;
            ESP_LOGD(TAG, "Frame diff: %u/%u = %u%% (threshold=%u%%)",
                     (unsigned)changed, (unsigned)total, percent, cfg->motion_threshold);
        }

        /* Free fb_b and sample buffer */
        camera_return_fb(fb_b);
        free(samples_a);

        if (motion && !s_in_cooldown) {
            ESP_LOGI(TAG, "Motion detected!");

            /* Capture a fresh frame for upload */
            camera_fb_t *fb_upload = NULL;
            if (camera_capture(&fb_upload) == ESP_OK && fb_upload != NULL) {
                upload_with_retry(fb_upload->buf, fb_upload->len);
                camera_return_fb(fb_upload);
            } else {
                ESP_LOGW(TAG, "Failed to capture frame for upload");
            }

            /* Enter cooldown */
            s_in_cooldown = true;
            s_cooldown_start_us = esp_timer_get_time();
        }

        /* Check cooldown expiration */
        if (s_in_cooldown) {
            int64_t elapsed_us = esp_timer_get_time() - s_cooldown_start_us;
            int64_t cooldown_us = (int64_t)cfg->motion_cooldown * 1000000LL;
            if (elapsed_us >= cooldown_us) {
                s_in_cooldown = false;
                ESP_LOGD(TAG, "Cooldown expired");
            }
        }

        /* Brief delay to prevent watchdog trigger */
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Motion detection task exiting");
    s_motion_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ---- Public API ---- */

esp_err_t motion_detect_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "Motion detection already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_running = true;
    s_in_cooldown = false;
    s_cooldown_start_us = 0;

    BaseType_t ret = xTaskCreate(
        motion_detection_task,
        "motion_detect",
        MOTION_TASK_STACK_SIZE,
        NULL,
        MOTION_TASK_PRIORITY,
        &s_motion_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create motion detection task");
        s_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Motion detection started (threshold=%u%%, cooldown=%us)",
             config_get()->motion_threshold, config_get()->motion_cooldown);
    return ESP_OK;
}

void motion_detect_stop(void)
{
    if (!s_running) {
        return;
    }

    ESP_LOGI(TAG, "Stopping motion detection...");
    s_running = false;

    /* Wait for task to exit (max 2 seconds) */
    TickType_t timeout = pdMS_TO_TICKS(2000);
    while (s_motion_task_handle != NULL && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout -= pdMS_TO_TICKS(100);
    }

    if (s_motion_task_handle != NULL) {
        ESP_LOGW(TAG, "Motion task did not exit in time, deleting");
        vTaskDelete(s_motion_task_handle);
        s_motion_task_handle = NULL;
    }

    s_in_cooldown = false;
    ESP_LOGI(TAG, "Motion detection stopped");
}

bool motion_detect_is_running(void)
{
    return s_running;
}
