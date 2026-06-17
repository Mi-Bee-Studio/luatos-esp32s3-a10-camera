/*
 * onvif_discovery.h - ONVIF WS-Discovery UDP listener
 *
 * Listens for WS-Discovery Probe messages on multicast 239.255.255.250:3702
 * and responds with ProbeMatches containing the device's service address.
 *
 * Default disabled. Enable via CONFIG_MIBEECAM_ENABLE_ONVIF compile flag
 * AND runtime config onvif_enabled=1.
 */
#ifndef ONVIF_DISCOVERY_H
#define ONVIF_DISCOVERY_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the ONVIF WS-Discovery listener
 *        Creates a UDP socket, joins IGMP multicast group, starts listener task.
 * @return ESP_OK on success
 */
esp_err_t onvif_discovery_start(void);

/**
 * @brief Stop the ONVIF WS-Discovery listener
 *        Closes socket, leaves multicast group, deletes task.
 * @return ESP_OK on success
 */
esp_err_t onvif_discovery_stop(void);

/**
 * @brief Check if discovery is running
 * @return true if listener task is active
 */
bool onvif_discovery_is_running(void);

#ifdef __cplusplus
}
#endif

#endif // ONVIF_DISCOVERY_H
