/*
 * camera_driver.h - OV2640 camera driver for ESP32-S3-A10
 *
 * Provides camera initialization, capture, and deinitialization
 * for the 8225N (OV2640) camera module on the ESP32-S3-A10 board.
 */

#ifndef CAMERA_DRIVER_H
#define CAMERA_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_camera.h"

/**
 * Supported camera resolutions
 */
typedef enum {
    CAMERA_RES_VGA = 0,     /**< 640x480 */
    CAMERA_RES_SVGA = 1,    /**< 800x600 */
    CAMERA_RES_XGA = 2,     /**< 1024x768 */
    CAMERA_RES_UXGA = 3,    /**< 1600x1200 */
    CAMERA_RES_MAX
} camera_resolution_t;

/**
 * Initialize the OV2640 camera with the given parameters.
 *
 * @param resolution  Desired resolution (default: CAMERA_RES_VGA)
 * @param fps         Desired frame rate (default: 15)
 * @param jpeg_quality JPEG quality 0-63, lower = better (default: 12)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t camera_init(camera_resolution_t resolution, uint8_t fps, uint8_t jpeg_quality);

/**
 * Deinitialize the camera and release all resources.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t camera_deinit(void);

/**
 * Capture a single frame from the camera.
 *
 * @param fb  Output pointer to the frame buffer. Caller must call
 *            camera_return_fb() when done with the buffer.
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if not initialized,
 *         or other error codes from the camera driver
 */
esp_err_t camera_capture(camera_fb_t **fb);

/**
 * Return a previously captured frame buffer to the driver for reuse.
 *
 * @param fb  Frame buffer to return
 * @return ESP_OK on success
 */
esp_err_t camera_return_fb(camera_fb_t *fb);

/**
 * Check if the camera has been initialized.
 *
 * @return true if initialized, false otherwise
 */
bool camera_is_initialized(void);

/**
 * Get the human-readable name of the camera sensor.
 *
 * @return Sensor name string (e.g. "OV2640"), or "Unknown" if not initialized
 */
const char* camera_get_sensor_name(void);

/**
 * Get the current resolution setting.
 *
 * @return Current resolution enum value
 */
camera_resolution_t camera_get_resolution(void);

#endif /* CAMERA_DRIVER_H */
