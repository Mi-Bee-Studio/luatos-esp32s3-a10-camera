/*
 * MiBeeCam - ESP32-S3-A10 Camera System
 * Clean startup: NVS → config → LED → SPIFFS → WiFi → camera → health
 *               → AP/STA → time_sync → streamer → web_server → motion → boot_btn
 */

#include "esp_log.h"
#include "esp_system.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "config_manager.h"
#include "wifi_manager.h"
#include "camera_driver.h"
#include "mjpeg_streamer.h"
#include "web_server.h"
#include "status_led.h"
#include "time_sync.h"
#include "health_monitor.h"
#include "motion_detect.h"

static const char *TAG = "main";

/* Track what's been started (prevent double-init) */
static bool s_web_server_started = false;
static bool s_motion_started = false;
static bool s_time_sync_started = false;

/* ---------------------------------------------------------------------------
 * WiFi state callback — updates LED and triggers post-connection services
 * ---------------------------------------------------------------------------*/
static void wifi_state_cb(wifi_state_t state, void *user_data)
{
    switch (state) {
    case WIFI_STATE_AP:
        led_set_status(LED_AP_MODE);
        break;

    case WIFI_STATE_STA_CONNECTING:
        led_set_status(LED_WIFI_CONNECTING);
        break;

    case WIFI_STATE_STA_CONNECTED:
        ESP_LOGI(TAG, "WiFi connected, IP: %s", wifi_get_ip_str());

        /* Start time sync (once) */
        if (!s_time_sync_started) {
            const cam_config_t *cfg = config_get();
            esp_err_t ret = time_sync_init(cfg->timezone[0] ? cfg->timezone : CONFIG_DEFAULT_TIMEZONE);
            if (ret == ESP_OK) {
                s_time_sync_started = true;
            } else {
                ESP_LOGW(TAG, "Time sync init failed: %s", esp_err_to_name(ret));
            }
        }

        /* Start web server (once) */
        if (!s_web_server_started) {
            esp_err_t ret = web_server_start(80);
            if (ret == ESP_OK) {
                s_web_server_started = true;
                ESP_LOGI(TAG, "Web server started");
            } else {
                ESP_LOGE(TAG, "Web server start failed: %s", esp_err_to_name(ret));
            }
        }

        /* Start motion detection (once) */
        if (!s_motion_started) {
            esp_err_t ret = motion_detect_start();
            if (ret == ESP_OK) {
                s_motion_started = true;
                ESP_LOGI(TAG, "Motion detection started");
            } else {
                ESP_LOGE(TAG, "Motion detection start failed: %s", esp_err_to_name(ret));
            }
        }

        /* System is fully up */
        led_set_status(LED_RUNNING);
        break;

    case WIFI_STATE_STA_DISCONNECTED:
        /* Don't change LED — let reconnect handle it */
        break;
    }
}

/* ---------------------------------------------------------------------------
 * BOOT button (GPIO0) long-press 5s → factory reset
 * ---------------------------------------------------------------------------*/
static void boot_btn_task(void *arg)
{
    gpio_num_t boot_gpio = GPIO_NUM_0;

    esp_rom_gpio_pad_select_gpio(boot_gpio);
    gpio_set_direction(boot_gpio, GPIO_MODE_INPUT);
    gpio_pullup_en(boot_gpio);

    ESP_LOGI(TAG, "BOOT button monitor started (GPIO0, 5s hold = factory reset)");

    while (1) {
        if (gpio_get_level(boot_gpio) == 0) {
            ESP_LOGW(TAG, "BOOT button pressed, waiting for long press...");
            vTaskDelay(pdMS_TO_TICKS(5000));

            if (gpio_get_level(boot_gpio) == 0) {
                ESP_LOGW(TAG, "FACTORY RESET triggered!");
                config_reset();
                esp_restart();
            } else {
                ESP_LOGI(TAG, "BOOT button released before 5s, no reset");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ---------------------------------------------------------------------------
 * app_main — system entry point
 * ---------------------------------------------------------------------------*/
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  MiBeeCam - ESP32-S3-A10");
    ESP_LOGI(TAG, "========================================");

    /* Step 1: NVS flash init */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, formatting...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[1/14] NVS initialized");

    /* Step 2: Config init */
    ret = config_init();
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[2/14] Config initialized");

    /* Step 3: LED init */
    ret = led_init();
    ESP_ERROR_CHECK(ret);
    led_set_status(LED_STARTING);
    ESP_LOGI(TAG, "[3/14] LED initialized");

    /* Step 4: SPIFFS mount */
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        esp_spiffs_info("spiffs", &total, &used);
        ESP_LOGI(TAG, "[4/14] SPIFFS mounted: %u KB total, %u KB used", total / 1024, used / 1024);
    }

    /* Step 5: Camera init (BEFORE WiFi to avoid I2C bus conflict) */
    {
        const cam_config_t *cam_cfg = config_get();
        ret = camera_init((camera_resolution_t)cam_cfg->resolution, cam_cfg->fps, cam_cfg->jpeg_quality);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(ret));
            led_set_status(LED_ERROR);
        } else {
            ESP_LOGI(TAG, "[5/14] Camera initialized (%s, %d fps, quality %d)",
                     camera_get_sensor_name(), cam_cfg->fps, cam_cfg->jpeg_quality);
        }
    }

    /* Step 6: WiFi init (netif + event loop) */
    ret = wifi_init();
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[6/14] WiFi subsystem initialized");

    /* Register WiFi state callback */
    wifi_register_callback(wifi_state_cb, NULL);

    /* Step 7: Health monitor init */
    ret = health_monitor_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Health monitor init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "[7/14] Health monitor initialized");
    }

    const cam_config_t *cfg = config_get();
    /* Step 8: Decide AP vs STA mode */
    bool has_wifi = cfg->wifi_ssid[0] != '\0' && cfg->wifi_pass[0] != '\0';

    if (has_wifi) {
        /* --- STA mode --- */
        ESP_LOGI(TAG, "[8/14] Valid WiFi config found, starting STA mode");

        ret = wifi_start_sta(cfg->wifi_ssid, cfg->wifi_pass);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "WiFi STA start failed: %s", esp_err_to_name(ret));
            led_set_status(LED_ERROR);
        }

        /* Remaining services (time_sync, web_server, motion) are started
         * by the wifi_state_cb when WIFI_STATE_STA_CONNECTED fires. */

        /* Step 10: MJPEG streamer init (needed before web_server registers it) */
        ret = mjpeg_streamer_init();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "MJPEG streamer init failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "[10/14] MJPEG streamer initialized");
        }
    } else {
        /* --- AP mode --- */
        ESP_LOGI(TAG, "[8/14] No valid WiFi config, starting AP mode");

        ret = wifi_start_ap();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "WiFi AP start failed: %s", esp_err_to_name(ret));
            led_set_status(LED_ERROR);
            return;
        }
        led_set_status(LED_AP_MODE);

        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "  AP Mode: SSID=MiBeeCam  Pass=12345678");
        ESP_LOGI(TAG, "  Config page: http://192.168.4.1");
        ESP_LOGI(TAG, "========================================");

        /* Step 11: Web server (config page only in AP mode) */
        ret = web_server_start(80);
        if (ret == ESP_OK) {
            s_web_server_started = true;
            ESP_LOGI(TAG, "[11/14] Web server started (AP mode)");
        } else {
            ESP_LOGE(TAG, "Web server start failed: %s", esp_err_to_name(ret));
        }

        /* In AP mode: no camera streaming, no motion, no time sync, no health.
         * Just the config web server for first-time setup. */
    }

    /* Step 14: BOOT button factory reset monitor */
    xTaskCreate(boot_btn_task, "boot_btn", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "[14/14] BOOT button monitor started");

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  System startup complete");
    ESP_LOGI(TAG, "========================================");
}
