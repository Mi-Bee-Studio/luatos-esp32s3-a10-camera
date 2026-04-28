#include "status_led.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdint.h>

#define LED_GPIO 10
#define TIMER_PERIOD_MS 50

static const char *TAG = "status_led";
static esp_timer_handle_t led_timer = NULL;
static led_status_t current_status = LED_STARTING;
static uint32_t phase_counter = 0;


static void led_timer_callback(void *arg) {
    int gpio_level = gpio_get_level(LED_GPIO);

    
    switch (current_status) {
        case LED_STARTING:
            // Toggle every 200ms (4 phases per 200ms at 50ms per phase)
            if (phase_counter % 4 == 0) {
                gpio_set_level(LED_GPIO, !gpio_level);
            }
            break;
            
        case LED_AP_MODE:
            // Toggle every 1000ms (20 phases per 1000ms at 50ms per phase)
            if (phase_counter % 20 == 0) {
                gpio_set_level(LED_GPIO, !gpio_level);
            }
            break;
            
        case LED_WIFI_CONNECTING:
            // Pattern: 200ms on, 200ms off, 200ms on, 600ms off (total 1200ms)
            if (phase_counter % 24 == 0) {
                gpio_set_level(LED_GPIO, 1); // On
            } else if (phase_counter % 24 == 4) {
                gpio_set_level(LED_GPIO, 0); // Off
            } else if (phase_counter % 24 == 8) {
                gpio_set_level(LED_GPIO, 1); // On
            } else if (phase_counter % 24 == 12) {
                gpio_set_level(LED_GPIO, 0); // Off
            }
            break;
            
        case LED_RUNNING:
            // Solid on - timer should be stopped, but just in case keep it high
            gpio_set_level(LED_GPIO, 1);
            break;
            
        case LED_ERROR:
            // SOS pattern: ... --- ... (dot=200, dash=600, gap=200)
            // Pattern repeats every 2000ms (40 phases at 50ms per phase)
            int sos_phase = phase_counter % 40;
            if (sos_phase < 3 || sos_phase == 7 || sos_phase == 12 || sos_phase == 17 || 
                sos_phase == 22 || sos_phase == 27 || sos_phase == 32) {
                gpio_set_level(LED_GPIO, 1); // Dot or dash
            } else {
                gpio_set_level(LED_GPIO, 0); // Gap
            }
            break;
            
        default:
            break;
    }
    
    phase_counter++;
}

esp_err_t led_init(void) {
    ESP_LOGI(TAG, "Initializing LED status indicator");
    
    // Configure GPIO10 as output
    esp_rom_gpio_pad_select_gpio(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0); // Start with LED off
    
    // Create periodic timer
    esp_timer_create_args_t timer_args = {
        .callback = led_timer_callback,
        .name = "led_status_timer"
    };
    
    esp_err_t ret = esp_timer_create(&timer_args, &led_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED timer");
        return ret;
    }
    
    // Start timer
    ret = esp_timer_start_periodic(led_timer, TIMER_PERIOD_MS * 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LED timer");
        esp_timer_delete(led_timer);
        led_timer = NULL;
        return ret;
    }
    
    ESP_LOGI(TAG, "LED status indicator initialized");
    return ESP_OK;
}

void led_set_status(led_status_t status) {
    ESP_LOGI(TAG, "LED status changed to %d", status);
    current_status = status;
    
    // Handle special cases that don't need the timer
    if (status == LED_RUNNING) {
        gpio_set_level(LED_GPIO, 1); // Solid on
        // Timer continues running but doesn't change state
        return;
    }
    
    phase_counter = 0; // Reset phase counter for new pattern
}

void led_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing LED status indicator");
    
    if (led_timer != NULL) {
        esp_timer_stop(led_timer);
        esp_timer_delete(led_timer);
        led_timer = NULL;
    }
    
    gpio_set_level(LED_GPIO, 0); // Turn off LED
    esp_rom_gpio_pad_select_gpio(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_INPUT);
}