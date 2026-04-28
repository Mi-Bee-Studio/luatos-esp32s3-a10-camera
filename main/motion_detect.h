/*
 * motion_detect.h - Frame-difference motion detection with remote upload
 *
 * Captures consecutive frames, compares pixel data, and uploads
 * a fresh JPEG to the configured server when motion is detected.
 */

#ifndef MOTION_DETECT_H
#define MOTION_DETECT_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Start motion detection task.
 *        Captures frames, compares for motion, uploads on trigger.
 * @return ESP_OK on success.
 */
esp_err_t motion_detect_start(void);

/**
 * @brief Stop motion detection task.
 */
void motion_detect_stop(void);

/**
 * @brief Check if motion detection is running.
 * @return true if running.
 */
bool motion_detect_is_running(void);

#endif /* MOTION_DETECT_H */
