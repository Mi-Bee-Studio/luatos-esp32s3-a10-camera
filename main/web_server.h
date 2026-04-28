/*
 * Web Server Module
 * REST API + SPIFFS static file serving for ESP32-S3-A10 camera
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * @brief Start the HTTP web server on the given port.
 *        Registers all API endpoints and SPIFFS static file handler.
 * @param port  TCP port to listen on (e.g. 80).
 * @return ESP_OK on success.
 */
esp_err_t web_server_start(uint16_t port);

/**
 * @brief Stop the HTTP web server and release resources.
 */
void web_server_stop(void);

/**
 * @brief Get the current HTTP server handle.
 * @return Server handle, or NULL if not running.
 */
httpd_handle_t web_server_get_handle(void);
