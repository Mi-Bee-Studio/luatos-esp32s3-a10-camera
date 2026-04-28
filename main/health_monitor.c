#include "wifi_manager.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "health_monitor.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>

static const char *TAG = "health_monitor";

static temperature_sensor_handle_t temp_sensor = NULL;
static float cached_temp = 0.0;
static time_t last_temp_read = 0;
static const time_t temp_cache_duration = 5; // 5 seconds

static TaskHandle_t health_task_handle = NULL;

static float read_temperature_sensor(void) {
    time_t now = time(NULL);
    
    // Return cached value if still valid
    if (now - last_temp_read < temp_cache_duration && cached_temp > -100.0) {
        return cached_temp;
    }
    
    float temp_c = 0.0;
    esp_err_t ret = temperature_sensor_get_celsius(temp_sensor, &temp_c);
    
    if (ret == ESP_OK) {
        cached_temp = temp_c;
        last_temp_read = now;
        ESP_LOGD(TAG, "Temperature: %.2f°C", temp_c);
        return temp_c;
    } else {
        ESP_LOGE(TAG, "Failed to read temperature: %s", esp_err_to_name(ret));
        return cached_temp; // Return cached value even if read fails
    }
}

static void health_monitor_task(void *pvParameters) {
    while (1) {
        time_t uptime = time(NULL);
        
        // Get system metrics
        size_t free_heap = esp_get_free_heap_size();
        size_t min_heap = esp_get_minimum_free_heap_size();
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        
        // Get temperature
        float temp = read_temperature_sensor();
        
        // Get WiFi state
        wifi_state_t wifi_state = wifi_get_state();
        const char *wifi_state_str = "Unknown";
        switch (wifi_state) {
            case WIFI_STATE_STA_CONNECTED: wifi_state_str = "Connected"; break;
            case WIFI_STATE_STA_DISCONNECTED: wifi_state_str = "Disconnected"; break;
            case WIFI_STATE_STA_CONNECTING: wifi_state_str = "Connecting"; break;
            case WIFI_STATE_AP: wifi_state_str = "AP Mode"; break;
            default: wifi_state_str = "Unknown"; break;
        }
        
        ESP_LOGI(TAG, "Health Report | Uptime: %ld | Heap: %u/%u | PSRAM: %u | Min Heap: %u | Temp: %.2f\u00b0C | WiFi: %s",
                 (long)uptime, (unsigned)free_heap, (unsigned)(free_heap - min_heap), (unsigned)free_psram, (unsigned)min_heap, temp, wifi_state_str);
        
        vTaskDelay(pdMS_TO_TICKS(60000)); // 60 seconds
    }
}

esp_err_t health_monitor_init(void) {
    // Initialize temperature sensor
    temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    esp_err_t ret = temperature_sensor_install(&temp_sensor_config, &temp_sensor);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install temperature sensor: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = temperature_sensor_enable(temp_sensor);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable temperature sensor: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize cached temperature
    cached_temp = read_temperature_sensor();
    
    // Create health monitoring task
    BaseType_t task_ret = xTaskCreate(
        health_monitor_task,
        "health_monitor",
        4096,
        NULL,
        2,  // Priority 2
        &health_task_handle
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create health monitor task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Health monitor initialized");
    return ESP_OK;
}

esp_err_t health_monitor_deinit(void) {
    if (health_task_handle) {
        vTaskDelete(health_task_handle);
        health_task_handle = NULL;
    }
    
    if (temp_sensor) {
        temperature_sensor_disable(temp_sensor);
        temperature_sensor_uninstall(temp_sensor);
        temp_sensor = NULL;
    }
    
    ESP_LOGI(TAG, "Health monitor deinitialized");
    return ESP_OK;
}

float get_chip_temp(void) {
    return read_temperature_sensor();
}