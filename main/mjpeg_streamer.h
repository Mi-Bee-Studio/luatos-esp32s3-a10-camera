/**
 * @file mjpeg_streamer.h
 * @brief MJPEG real-time video streaming — independent TCP server (port 81).
 *
 * Separate listen + per-client FreeRTOS tasks bypass the single-threaded
 * httpd entirely. The main web server on port 80 stays responsive even
 * when stream clients are connected for hours.
 *
 * Architecture:
 *   mjpeg_streamer_init()
 *   mjpeg_streamer_start() — TCP listen on port 81, spawn listen task (Core 1)
 *     accept() → per-client task (Core 1) → multipart/x-mixed-replace stream
 *   mjpeg_streamer_stop()  — close all connections, stop listen task
 */

#pragma once

#include "esp_err.h"

/**
 * @brief Initialize the MJPEG streamer module.
 *
 * Creates internal mutex and resets client count.
 * Must be called once before mjpeg_streamer_start().
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if mutex creation fails.
 */
esp_err_t mjpeg_streamer_init(void);

/**
 * @brief Start the MJPEG TCP server on port 81.
 *
 * Creates a TCP listen socket, binds to port 81, and spawns a listen task
 * on Core 1. Accepted connections get a dedicated client task (max 2).
 * Streams multipart/x-mixed-replace MJPEG via raw TCP (no httpd dependency).
 *
 * @return ESP_OK on success.
 */
esp_err_t mjpeg_streamer_start(void);

/**
 * @brief Stop the MJPEG streamer and close all connections.
 *
 * Closes the listen socket and all active client sockets. Listen and
 * client tasks clean up and exit. Safe to call multiple times.
 */
void mjpeg_streamer_stop(void);

/**
 * @brief Get the number of currently connected streaming clients.
 *
 * @return Client count (0 .. MAX_STREAM_CLIENTS).
 */
int mjpeg_streamer_get_client_count(void);
