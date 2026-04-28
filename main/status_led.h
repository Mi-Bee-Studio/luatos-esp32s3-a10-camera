#pragma once
#include "esp_err.h"

typedef enum {
    LED_STARTING = 0,       // Fast blink (200ms on/off)
    LED_AP_MODE,            // Slow blink (1000ms on/off)
    LED_WIFI_CONNECTING,    // Double blink (200ms on, 200ms off, 200ms on, 600ms off)
    LED_RUNNING,            // Solid on
    LED_ERROR               // SOS pattern (... --- ...)
} led_status_t;

esp_err_t led_init(void);
void led_set_status(led_status_t status);
void led_deinit(void);