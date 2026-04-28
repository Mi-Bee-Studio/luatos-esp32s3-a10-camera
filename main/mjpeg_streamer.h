/**
 * @file mjpeg_streamer.h
 * @brief MJPEG real-time video streaming over HTTP.
 *
 * Provides a multipart/x-mixed-replace MJPEG stream handler
 * for esp_http_server. Supports up to 2 concurrent clients.
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * @brief Initialize the MJPEG streamer.
 *
 * Creates internal mutex and resets client count.
 * Must be called once before registering the URI handler.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if mutex creation fails.
 */
esp_err_t mjpeg_streamer_init(void);

/**
 * @brief Register the /stream URI handler on the given HTTP server.
 *
 * @param server  HTTP server handle (must not be NULL).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if server is NULL.
 */
esp_err_t mjpeg_streamer_register(httpd_handle_t server);

/**
 * @brief Stop all active streams and release resources.
 *
 * Safe to call multiple times.
 */
void mjpeg_streamer_stop(void);

/**
 * @brief Get the number of currently connected streaming clients.
 *
 * @return Client count (0 .. MAX_STREAM_CLIENTS).
 */
int mjpeg_streamer_get_client_count(void);

/**
 * @brief HTTP handler for GET /stream.
 *
 * Sends a multipart/x-mixed-replace MJPEG stream until the
 * client disconnects or capture fails.  Returns 503 if
 * MAX_STREAM_CLIENTS are already connected.
 *
 * @param req  HTTP request object.
 * @return ESP_OK on normal stream end, ESP_FAIL on errors.
 */
esp_err_t mjpeg_streamer_http_handler(httpd_req_t *req);
