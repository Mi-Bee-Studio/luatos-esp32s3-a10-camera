/*
 * event_bus.c - In-memory publish/subscribe event bus implementation
 *
 * Synchronous dispatch to registered subscribers. Mutex-protected
 * subscribe/unsubscribe operations. Max 8 subscribers total.
 */

#include "event_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "event_bus";

/** @brief Maximum number of subscriptions across all event types */
#define MAX_SUBSCRIPTIONS 8

/**
 * @brief Internal subscription entry
 */
typedef struct {
    subscription_id_t id;       /**< Unique subscription identifier */
    event_type_t type;          /**< Subscribed event type */
    event_handler_t handler;    /**< Callback function */
    void *user_data;            /**< User data passed to callback */
    bool active;                /**< true if this slot is in use */
} subscription_t;

/** @brief Static subscription table */
static subscription_t s_subscriptions[MAX_SUBSCRIPTIONS];

/** @brief Next subscription ID to assign (monotonically increasing) */
static int s_next_id = 0;

/** @brief Mutex protecting subscribe/unsubscribe operations */
static SemaphoreHandle_t s_mutex = NULL;

esp_err_t event_bus_init(void)
{
    if (s_mutex != NULL) {
        ESP_LOGW(TAG, "event_bus already initialized");
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Zero the subscription table */
    for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
        s_subscriptions[i].active = false;
        s_subscriptions[i].id = INVALID_SUBSCRIPTION_ID;
        s_subscriptions[i].handler = NULL;
        s_subscriptions[i].user_data = NULL;
    }
    s_next_id = 0;

    ESP_LOGI(TAG, "event bus initialized (max %d subscribers)", MAX_SUBSCRIPTIONS);
    return ESP_OK;
}

esp_err_t event_bus_subscribe(event_type_t type, event_handler_t handler, void *user_data, subscription_id_t *out_id)
{
    if (handler == NULL) {
        ESP_LOGE(TAG, "subscribe called with NULL handler");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "event_bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_ERR_NO_MEM;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        /* Find an available slot */
        int slot = -1;
        for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
            if (!s_subscriptions[i].active) {
                slot = i;
                break;
            }
        }

        if (slot >= 0) {
            s_subscriptions[slot].id = s_next_id++;
            s_subscriptions[slot].type = type;
            s_subscriptions[slot].handler = handler;
            s_subscriptions[slot].user_data = user_data;
            s_subscriptions[slot].active = true;

            if (out_id != NULL) {
                *out_id = s_subscriptions[slot].id;
            }

            ESP_LOGD(TAG, "subscriber #%d registered for event type %d", s_subscriptions[slot].id, (int)type);
            ret = ESP_OK;
        } else {
            ESP_LOGE(TAG, "subscription table full (max %d)", MAX_SUBSCRIPTIONS);
            ret = ESP_ERR_NO_MEM;
        }

        xSemaphoreGive(s_mutex);
    }

    return ret;
}

esp_err_t event_bus_unsubscribe(subscription_id_t id)
{
    if (id == INVALID_SUBSCRIPTION_ID) {
        ESP_LOGE(TAG, "unsubscribe called with INVALID_SUBSCRIPTION_ID");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "event_bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
            if (s_subscriptions[i].active && s_subscriptions[i].id == id) {
                s_subscriptions[i].active = false;
                s_subscriptions[i].handler = NULL;
                s_subscriptions[i].user_data = NULL;
                s_subscriptions[i].id = INVALID_SUBSCRIPTION_ID;
                ESP_LOGD(TAG, "subscriber #%d unregistered", id);
                ret = ESP_OK;
                break;
            }
        }

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "unsubscribe: subscription #%d not found", id);
        }

        xSemaphoreGive(s_mutex);
    }

    return ret;
}

esp_err_t event_bus_publish(const event_t *event)
{
    if (event == NULL) {
        ESP_LOGE(TAG, "publish called with NULL event");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "event_bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Lock the mutex to safely iterate the subscription table */
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
            if (s_subscriptions[i].active && s_subscriptions[i].type == event->type) {
                ESP_LOGD(TAG, "dispatching event type %d to subscriber #%d",
                         (int)event->type, s_subscriptions[i].id);
                s_subscriptions[i].handler(event, s_subscriptions[i].user_data);
            }
        }
        xSemaphoreGive(s_mutex);
    }

    return ESP_OK;
}
