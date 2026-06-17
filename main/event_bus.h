/*
 * event_bus.h - In-memory publish/subscribe event bus
 *
 * Provides a lightweight pub/sub system for inter-module communication.
 * Max 8 subscribers total across all event types. Synchronous dispatch.
 * Thread-safe subscribe/unsubscribe via mutex protection.
 */

#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Event types supported by the event bus
 */
typedef enum {
    EVENT_MOTION_DETECTED,              /**< Motion detection triggered */
    EVENT_MOTION_END,                   /**< Motion detection ended */
    EVENT_WIFI_STATE_CHANGED,           /**< WiFi state changed */
    EVENT_WIFI_SWITCHED_SSID,           /**< WiFi switched to a different SSID */
    EVENT_STREAM_CLIENT_CONNECTED,      /**< MJPEG stream client connected */
    EVENT_STREAM_CLIENT_DISCONNECTED,   /**< MJPEG stream client disconnected */
    EVENT_HEALTH_WARNING,               /**< System health warning (e.g., low heap) */
    EVENT_UPLOAD_SUCCESS,               /**< Remote upload completed successfully */
    EVENT_UPLOAD_FAILED,                /**< Remote upload failed */
} event_type_t;

/**
 * @brief Event structure passed to all subscribers
 */
typedef struct {
    event_type_t type;        /**< Event type identifier */
    int64_t timestamp;        /**< Event timestamp (microseconds since boot, set by publisher) */
    const void *payload;      /**< Optional payload pointer (caller guarantees lifetime during publish) */
    size_t payload_len;       /**< Payload length in bytes (0 if no payload) */
} event_t;

/**
 * @brief Callback function type for event subscribers
 * @param event  Pointer to the event data (valid only during the callback invocation)
 * @param user_data  User-provided pointer registered at subscription time
 */
typedef void (*event_handler_t)(const event_t *event, void *user_data);

/**
 * @brief Type for subscription identifiers
 */
typedef int subscription_id_t;

/** @brief Invalid subscription ID value */
#define INVALID_SUBSCRIPTION_ID (-1)

/**
 * @brief Initialize the event bus
 *        Must be called once before any subscribe/publish operations.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if mutex creation fails
 */
esp_err_t event_bus_init(void);

/**
 * @brief Register a subscriber for a specific event type
 * @param type      Event type to subscribe to
 * @param handler   Callback function (must not be NULL)
 * @param user_data Opaque pointer passed to the callback on each invocation
 * @param out_id    Output parameter receiving the subscription ID (can be NULL)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if handler is NULL
 * @return ESP_ERR_NO_MEM if the subscription table is full (max 8)
 */
esp_err_t event_bus_subscribe(event_type_t type, event_handler_t handler, void *user_data, subscription_id_t *out_id);

/**
 * @brief Remove a subscriber by its subscription ID
 * @param id Subscription ID returned by event_bus_subscribe()
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if id is INVALID_SUBSCRIPTION_ID or not found
 */
esp_err_t event_bus_unsubscribe(subscription_id_t id);

/**
 * @brief Publish an event synchronously to all subscribers of that type
 *        Handlers are called in the publisher's context and must not block.
 * @param event Pointer to the event to publish (must not be NULL)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if event is NULL
 */
esp_err_t event_bus_publish(const event_t *event);

#ifdef __cplusplus
}
#endif

#endif // EVENT_BUS_H
