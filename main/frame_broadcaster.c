/*
 * frame_broadcaster.c - Single-consumer DRAM frame cache with reference counting
 *
 * This module provides a single-consumer frame cache for camera frames.
 * The camera publishes frames via fbroadcast_publish(), and consumers
 * (streaming, motion detection) acquire/release frames via the ref-counted API.
 *
 * DROP policy: If a new frame arrives while the previous frame is still held
 * (ref_count > 0), the new frame is silently dropped. This prevents blocking
 * the camera producer.
 *
 * Memory: Static DRAM allocation (50KB). No heap fragmentation.
 * Concurrency: Mutex protects metadata access.
 */

#include "frame_broadcaster.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "frame_bus";

/* Maximum frame size: 50KB for VGA JPEG */
#define MAX_FRAME_SIZE (50 * 1024)

/* ======================================================================== *
 * Stub implementations (when CONFIG_MIBEECAM_ENABLE_FRAME_BROADCASTER is 0)
 * ======================================================================== */
#ifndef CONFIG_MIBEECAM_ENABLE_FRAME_BROADCASTER

esp_err_t fbroadcast_init(void)
{
    return ESP_OK;
}

esp_err_t fbroadcast_publish(const uint8_t *jpeg_buf, size_t len, uint64_t ts)
{
    (void)jpeg_buf;
    (void)len;
    (void)ts;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fbroadcast_acquire_latest(frame_ref_t **out)
{
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

void fbroadcast_release(frame_ref_t *frame)
{
    (void)frame;
}

/* ======================================================================== *
 * Full implementation (when CONFIG_MIBEECAM_ENABLE_FRAME_BROADCASTER is 1)
 * ======================================================================== */
#else /* CONFIG_MIBEECAM_ENABLE_FRAME_BROADCASTER */

/* ---------- Module state ---------- */

/* Static frame buffer — DRAM only, no heap allocation */
static uint8_t s_frame_buf[MAX_FRAME_SIZE];

/* Latest published frame metadata */
static frame_ref_t s_latest_frame = {
    .buf          = s_frame_buf,
    .len          = 0,
    .ref_count    = 0,
    .timestamp_us = 0,
};

/* Mutex for thread-safe metadata access */
static SemaphoreHandle_t s_mutex = NULL;

/* ---------- Public API ---------- */

esp_err_t fbroadcast_init(void)
{
    if (s_mutex != NULL) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Reset state */
    s_latest_frame.len          = 0;
    s_latest_frame.ref_count    = 0;
    s_latest_frame.timestamp_us = 0;

    ESP_LOGI(TAG, "Frame broadcaster initialized (max %u bytes per frame)",
             (unsigned)MAX_FRAME_SIZE);
    return ESP_OK;
}

esp_err_t fbroadcast_publish(const uint8_t *jpeg_buf, size_t len, uint64_t ts)
{
    if (jpeg_buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len > MAX_FRAME_SIZE) {
        ESP_LOGW(TAG, "Frame too large: %u > %u, dropping",
                 (unsigned)len, (unsigned)MAX_FRAME_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* DROP policy: if someone still holds the previous frame, discard this one */
    if (s_latest_frame.ref_count > 0) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Frame dropped: ref_count=%u (consumer still holding previous frame)",
                 (unsigned)s_latest_frame.ref_count);
        return ESP_ERR_INVALID_STATE;
    }

    /* Copy frame data into static buffer (DRAM) */
    memcpy(s_frame_buf, jpeg_buf, len);
    s_latest_frame.len          = len;
    s_latest_frame.timestamp_us = ts;
    /* ref_count stays 0 — consumer will increment on acquire */

    xSemaphoreGive(s_mutex);

    ESP_LOGD(TAG, "Frame published: %u bytes, ts=%llu",
             (unsigned)len, (unsigned long long)ts);
    return ESP_OK;
}

esp_err_t fbroadcast_acquire_latest(frame_ref_t **out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_latest_frame.len == 0) {
        xSemaphoreGive(s_mutex);
        ESP_LOGD(TAG, "Acquire: no frame available");
        return ESP_ERR_NOT_FOUND;
    }

    /* Increment reference count — consumer now holds the frame */
    s_latest_frame.ref_count++;
    *out = &s_latest_frame;

    xSemaphoreGive(s_mutex);

    ESP_LOGD(TAG, "Frame acquired: len=%u, ref_count=%u",
             (unsigned)s_latest_frame.len, (unsigned)s_latest_frame.ref_count);
    return ESP_OK;
}

void fbroadcast_release(frame_ref_t *frame)
{
    if (frame == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (frame->ref_count > 0) {
        frame->ref_count--;
    } else {
        ESP_LOGW(TAG, "Release called on frame with ref_count=0");
    }

    xSemaphoreGive(s_mutex);

    ESP_LOGD(TAG, "Frame released: ref_count=%u", (unsigned)frame->ref_count);
}

#endif /* CONFIG_MIBEECAM_ENABLE_FRAME_BROADCASTER */
