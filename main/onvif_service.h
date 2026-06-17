/*
 * onvif_service.h - ONVIF SOAP service handlers
 *
 * Registers HTTP POST handlers for /onvif/device_service and /onvif/media_service.
 * Implements 5 minimal SOAP methods for ONVIF Profile S compatibility:
 *   GetDeviceInformation, GetCapabilities, GetServices (device_service)
 *   GetProfiles, GetStreamUri (media_service)
 *
 * Default disabled. Enable via CONFIG_MIBEECAM_ENABLE_ONVIF compile flag.
 * When disabled, all public functions return ESP_ERR_NOT_SUPPORTED.
 */
#ifndef ONVIF_SERVICE_H
#define ONVIF_SERVICE_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register ONVIF SOAP service handlers with the HTTP server
 *        Registers /onvif/device_service and /onvif/media_service POST handlers
 * @param server HTTP server handle
 * @return ESP_OK on success
 */
esp_err_t onvif_service_start(httpd_handle_t server);

/**
 * @brief Unregister ONVIF SOAP service handlers
 * @param server HTTP server handle
 * @return ESP_OK on success
 */
esp_err_t onvif_service_stop(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif // ONVIF_SERVICE_H
