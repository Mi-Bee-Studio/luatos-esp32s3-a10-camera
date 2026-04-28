idf_component_register(
    SRCS "cJSON.c" "config_manager.c" "wifi_manager.c" "camera_driver.c" "mjpeg_streamer.c" "web_server.c" "status_led.c" "time_sync.c" "health_monitor.c" "motion_detect.c" "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi nvs_flash esp_http_server esp_http_client esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_ledc esp_timer heap lwip spiffs esp_system esp_event esp_netif driver esp_driver_tsens
)