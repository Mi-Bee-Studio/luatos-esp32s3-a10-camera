/*
 * webhook.h - Asynchronous HTTP webhook client
 *
 * Events are enqueued via webhook_emit() and sent by a dedicated task.
 * Features: 5s timeout, no retry, max 8 queued events (older dropped).
 * Payload format: JSON with device/event/timestamp/data fields.
 * Auth: X-MiBee-Event header + optional X-MiBee-Signature (HMAC-SHA256).
 *
 * Default disabled. Enable via CONFIG_MIBEECAM_ENABLE_WEBHOOK compile flag
 * AND runtime config webhook_url non-empty.
 */
#ifndef WEBHOOK_H
#define WEBHOOK_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the webhook subsystem.
 *        Creates queue, starts webhook task. Subscribes to all event_bus events
 *        for automatic forwarding (if webhook_forward_all_events is enabled).
 *        No-op if webhook_url config is empty.
 * @return ESP_OK on success
 */
esp_err_t webhook_init(void);

/**
 * @brief Emit a webhook event (non-blocking, queues for async send)
 * @param event_type  Event type string (e.g., "motion_detected")
 * @param payload_json JSON payload string (can be NULL or empty)
 * @return ESP_OK on success, ESP_ERR_NO_MEM if queue full (event dropped)
 */
esp_err_t webhook_emit(const char *event_type, const char *payload_json);

/**
 * @brief Check if webhook is active (URL configured + task running)
 * @return true if webhook will send events
 */
bool webhook_is_active(void);

#ifdef __cplusplus
}
#endif

#endif // WEBHOOK_H
