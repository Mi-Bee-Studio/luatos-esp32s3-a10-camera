/*
 * webhook.c - Asynchronous HTTP webhook client
 *
 * Events are queued via webhook_emit() and sent by a dedicated FreeRTOS task.
 * Features: 5s timeout, no retry, max 8 queued events (older dropped).
 * Payload format: JSON with device/event/timestamp/data fields.
 *
 * Guarded by CONFIG_MIBEECAM_ENABLE_WEBHOOK compile flag.
 */

#include "webhook.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "config_manager.h"
#include "event_bus.h"

/* -------------------------------------------------------------------------- */
/*  Conditional compilation guard                                             */
/* -------------------------------------------------------------------------- */
#ifdef CONFIG_MIBEECAM_ENABLE_WEBHOOK

static const char *TAG = "webhook";

/* -------------------------------------------------------------------------- */
/*  Constants                                                                 */
/* -------------------------------------------------------------------------- */
#define WEBHOOK_QUEUE_LENGTH      8
#define WEBHOOK_TASK_STACK_SIZE   6144
#define WEBHOOK_TASK_PRIORITY     2
#define WEBHOOK_MAX_PAYLOAD       1024
#define WEBHOOK_BODY_BUF_SIZE     2048

/* -------------------------------------------------------------------------- */
/*  Queue item                                                                */
/* -------------------------------------------------------------------------- */
typedef struct {
    char     event_type[32];
    char     payload[WEBHOOK_MAX_PAYLOAD];
    int64_t  timestamp_us;   /*< timestamp in microseconds (from esp_timer) */
    bool     truncated;
} webhook_queue_item_t;

/* -------------------------------------------------------------------------- */
/*  Static state                                                              */
/* -------------------------------------------------------------------------- */
static QueueHandle_t  s_webhook_queue   = NULL;
static TaskHandle_t   s_webhook_task    = NULL;
static volatile bool  s_webhook_active  = false;

/* Subscription IDs for cleanup */
static subscription_id_t s_sub_ids[9];
static int s_sub_count = 0;

/* -------------------------------------------------------------------------- */
/*  Forward declarations                                                      */
/* -------------------------------------------------------------------------- */
static void webhook_task(void *arg);
static void webhook_event_handler(const event_t *event, void *user_data);

/* -------------------------------------------------------------------------- */
/*  Event type → string mapping                                               */
/* -------------------------------------------------------------------------- */
static const char *event_type_to_string(event_type_t type)
{
    switch (type) {
        case EVENT_MOTION_DETECTED:              return "motion_detected";
        case EVENT_MOTION_END:                   return "motion_end";
        case EVENT_WIFI_STATE_CHANGED:           return "wifi_state_changed";
        case EVENT_WIFI_SWITCHED_SSID:           return "wifi_switched_ssid";
        case EVENT_STREAM_CLIENT_CONNECTED:      return "stream_client_connected";
        case EVENT_STREAM_CLIENT_DISCONNECTED:   return "stream_client_disconnected";
        case EVENT_HEALTH_WARNING:               return "health_warning";
        case EVENT_UPLOAD_SUCCESS:               return "upload_success";
        case EVENT_UPLOAD_FAILED:                return "upload_failed";
        default:                                 return "unknown";
    }
}

/* -------------------------------------------------------------------------- */
/*  Event bus subscription handler                                            */
/* -------------------------------------------------------------------------- */
static void webhook_event_handler(const event_t *event, void *user_data)
{
    (void)user_data;
    if (event == NULL) {
        return;
    }

    const char *type_str = event_type_to_string(event->type);

    /* If the event carries a payload, try to forward it as JSON data */
    if (event->payload != NULL && event->payload_len > 0) {
        /* Copy up to WEBHOOK_MAX_PAYLOAD bytes of the payload as JSON fragment */
        size_t copy_len = event->payload_len;
        if (copy_len > WEBHOOK_MAX_PAYLOAD - 1) {
            copy_len = WEBHOOK_MAX_PAYLOAD - 1;
        }
        char tmp[WEBHOOK_MAX_PAYLOAD];
        memcpy(tmp, event->payload, copy_len);
        tmp[copy_len] = '\0';
        webhook_emit(type_str, tmp);
    } else {
        webhook_emit(type_str, NULL);
    }
}

/* -------------------------------------------------------------------------- */
/*  Webhook task — dequeues and sends events                                  */
/* -------------------------------------------------------------------------- */
static void webhook_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Webhook task started");

    webhook_queue_item_t item;

    while (1) {
        /* Block indefinitely waiting for an event */
        if (xQueueReceive(s_webhook_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Re-read config each iteration (may have changed at runtime) */
        const cam_config_t *cfg = config_get();
        if (cfg->webhook_url[0] == '\0') {
            ESP_LOGW(TAG, "Webhook URL empty, dropping event: %s", item.event_type);
            continue;
        }

        /* Warn if HTTPS URL (not supported yet) */
        if (strncmp(cfg->webhook_url, "https://", 8) == 0) {
            ESP_LOGW(TAG, "HTTPS webhooks not supported yet, using HTTP");
        }

        /* Build JSON body manually (avoid cJSON in this module) */
        char body[WEBHOOK_BODY_BUF_SIZE];
        int body_len;

        if (item.payload[0] == '\0') {
            /* No payload — use empty data object */
            body_len = snprintf(body, sizeof(body),
                "{\"device\":\"%s\",\"event\":\"%s\",\"timestamp\":%lld,\"data\":{}}",
                cfg->device_name, item.event_type,
                (long long)time(NULL));
        } else {
            /* Has payload — embed as-is (caller guarantees valid JSON) */
            if (item.truncated) {
                body_len = snprintf(body, sizeof(body),
                    "{\"device\":\"%s\",\"event\":\"%s\",\"timestamp\":%lld,\"data\":%s,\"truncated\":true}",
                    cfg->device_name, item.event_type,
                    (long long)time(NULL), item.payload);
            } else {
                body_len = snprintf(body, sizeof(body),
                    "{\"device\":\"%s\",\"event\":\"%s\",\"timestamp\":%lld,\"data\":%s}",
                    cfg->device_name, item.event_type,
                    (long long)time(NULL), item.payload);
            }
        }

        if (body_len >= (int)sizeof(body)) {
            ESP_LOGW(TAG, "Webhook body truncated (%d bytes)", body_len);
        }

        /* --- HTTP POST --- */
        esp_http_client_config_t http_cfg = {
            .url = cfg->webhook_url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 5000,
            .disable_auto_redirect = true,
        };

        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        if (client == NULL) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            continue;
        }

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "X-MiBee-Event", item.event_type);

        /* HMAC-SHA256 signature — not yet implemented */
        if (cfg->webhook_secret[0] != '\0') {
            ESP_LOGW(TAG, "HMAC-SHA256 webhook signing not implemented, skipping signature");
        }

        esp_http_client_set_post_field(client, body, strlen(body));
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);

        esp_http_client_cleanup(client);

        /* Dispatch success/failure to event bus (no retry) */
        if (err == ESP_OK && status >= 200 && status < 300) {
            ESP_LOGI(TAG, "Webhook sent: %s → %d", item.event_type, status);
            event_t evt = {
                .type = EVENT_UPLOAD_SUCCESS,
                .timestamp = esp_timer_get_time(),
                .payload = NULL,
                .payload_len = 0,
            };
            event_bus_publish(&evt);
        } else {
            ESP_LOGW(TAG, "Webhook failed: %s → HTTP %d, err=%s",
                     item.event_type, status, esp_err_to_name(err));
            event_t evt = {
                .type = EVENT_UPLOAD_FAILED,
                .timestamp = esp_timer_get_time(),
                .payload = NULL,
                .payload_len = 0,
            };
            event_bus_publish(&evt);
            /* NO RETRY */
        }
    }

    /* unreachable */
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

esp_err_t webhook_init(void)
{
    if (s_webhook_queue != NULL) {
        ESP_LOGW(TAG, "Webhook already initialized");
        return ESP_OK;
    }

    const cam_config_t *cfg = config_get();
    if (cfg->webhook_url[0] == '\0') {
        ESP_LOGI(TAG, "Webhook URL empty, not starting webhook task");
        s_webhook_active = false;
        return ESP_OK;
    }

    /* Create queue */
    s_webhook_queue = xQueueCreate(WEBHOOK_QUEUE_LENGTH, sizeof(webhook_queue_item_t));
    if (s_webhook_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create webhook queue");
        return ESP_ERR_NO_MEM;
    }

    /* Create task pinned to core 1 */
    BaseType_t ret = xTaskCreatePinnedToCore(
        webhook_task,
        "webhook",
        WEBHOOK_TASK_STACK_SIZE,
        NULL,
        WEBHOOK_TASK_PRIORITY,
        &s_webhook_task,
        1  /* core 1 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create webhook task");
        vQueueDelete(s_webhook_queue);
        s_webhook_queue = NULL;
        return ESP_FAIL;
    }

    /* Subscribe to all event bus events for automatic forwarding */
    event_type_t all_types[] = {
        EVENT_MOTION_DETECTED,
        EVENT_MOTION_END,
        EVENT_WIFI_STATE_CHANGED,
        EVENT_WIFI_SWITCHED_SSID,
        EVENT_STREAM_CLIENT_CONNECTED,
        EVENT_STREAM_CLIENT_DISCONNECTED,
        EVENT_HEALTH_WARNING,
        EVENT_UPLOAD_SUCCESS,
        EVENT_UPLOAD_FAILED,
    };

    s_sub_count = 0;
    for (size_t i = 0; i < sizeof(all_types) / sizeof(all_types[0]); i++) {
        esp_err_t sub_err = event_bus_subscribe(
            all_types[i],
            webhook_event_handler,
            NULL,
            &s_sub_ids[s_sub_count]);
        if (sub_err == ESP_OK) {
            s_sub_count++;
        } else {
            ESP_LOGW(TAG, "Failed to subscribe to event type %d", (int)all_types[i]);
        }
    }

    s_webhook_active = true;
    ESP_LOGI(TAG, "Webhook initialized: %s (subscribed to %d event types)",
             cfg->webhook_url, s_sub_count);
    return ESP_OK;
}

esp_err_t webhook_emit(const char *event_type, const char *payload_json)
{
    if (event_type == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_webhook_queue == NULL) {
        ESP_LOGW(TAG, "Webhook not initialized, dropping event: %s", event_type);
        return ESP_ERR_INVALID_STATE;
    }

    webhook_queue_item_t item;
    memset(&item, 0, sizeof(item));

    /* Copy event type (truncate silently) */
    strncpy(item.event_type, event_type, sizeof(item.event_type) - 1);
    item.event_type[sizeof(item.event_type) - 1] = '\0';

    /* Copy payload (truncate if > 1KB) */
    item.truncated = false;
    if (payload_json != NULL && payload_json[0] != '\0') {
        size_t plen = strlen(payload_json);
        if (plen >= WEBHOOK_MAX_PAYLOAD) {
            memcpy(item.payload, payload_json, WEBHOOK_MAX_PAYLOAD - 1);
            item.payload[WEBHOOK_MAX_PAYLOAD - 1] = '\0';
            item.truncated = true;
            ESP_LOGW(TAG, "Webhook payload truncated (%zu bytes)", plen);
        } else {
            memcpy(item.payload, payload_json, plen + 1);
        }
    }
    /* else: item.payload stays empty → "data":{} */

    item.timestamp_us = esp_timer_get_time();

    /* Non-blocking send — drop if queue full */
    BaseType_t queued = xQueueSend(s_webhook_queue, &item, 0);
    if (queued != pdTRUE) {
        ESP_LOGW(TAG, "Webhook queue full, dropping event: %s", event_type);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool webhook_is_active(void)
{
    return s_webhook_active && (s_webhook_queue != NULL);
}

/* -------------------------------------------------------------------------- */
/*  Stub implementations — webhook disabled                                   */
/* -------------------------------------------------------------------------- */
#else /* !CONFIG_MIBEECAM_ENABLE_WEBHOOK */

static const char *TAG = "webhook";

esp_err_t webhook_init(void)
{
    ESP_LOGI(TAG, "Webhook disabled (CONFIG_MIBEECAM_ENABLE_WEBHOOK not set)");
    return ESP_OK;
}

esp_err_t webhook_emit(const char *event_type, const char *payload_json)
{
    (void)event_type;
    (void)payload_json;
    return ESP_ERR_NOT_SUPPORTED;
}

bool webhook_is_active(void)
{
    return false;
}

#endif /* CONFIG_MIBEECAM_ENABLE_WEBHOOK */
