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
#include "event_bus.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "health_monitor";

static temperature_sensor_handle_t temp_sensor = NULL;
static float cached_temp = 0.0;
static time_t last_temp_read = 0;
static const time_t temp_cache_duration = 5; // 5 seconds

static TaskHandle_t health_task_handle = NULL;

/* httpd :80 self-heal probe — sends a real HTTP request to localhost:80.
 * TCP connect alone is insufficient: LWIP accepts connections even when
 * httpd has no free worker. Only a real request proves the event loop is alive. */
static bool probe_httpd_port80(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(80),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };

    bool ok = false;
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) == 0) {
        static const char req[] =
            "GET /api/status HTTP/1.0\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n\r\n";
        if (send(sock, req, sizeof(req) - 1, 0) > 0) {
            char buf[32];
            int n = recv(sock, buf, sizeof(buf), 0);
            ok = (n > 0);
        }
    }
    close(sock);
    return ok;
}


static size_t s_baseline_free_heap = 0;
static size_t s_baseline_min_heap = 0;
/* 本板无 PSRAM：单路 MJPEG 拉流的正常稳态 free ≈ 24~26KB（基线 105KB）。
 * 30KB 阈值会把正常运行当警告刷屏（每 30s 一条 + WS 事件），校准到 15KB：
 * 低于它才意味着流/WS/motion 之外出现了真正的内存泄漏。 */
#define HEAP_WARNING_THRESHOLD 15360  // 15KB

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
        /* esp_timer, not time(): SNTP jumps the wall clock, time() would
         * report epoch seconds instead of uptime after sync */
        uint64_t uptime = esp_timer_get_time() / 1000000ULL;
        
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
        
        int heap_delta = (int)free_heap - (int)s_baseline_free_heap;
        ESP_LOGI(TAG, "Health Report | Uptime: %ld | Heap: %u/%u | PSRAM: %u | Min Heap: %u | Temp: %.2f\u00b0C | WiFi: %s | HeapDelta: %d",
                 (unsigned long)uptime, (unsigned)free_heap, (unsigned)(free_heap - min_heap), (unsigned)free_psram, (unsigned)min_heap, temp, wifi_state_str, heap_delta);

        // Check heap threshold and publish warning
        size_t current_free = esp_get_free_heap_size();
        if (current_free < HEAP_WARNING_THRESHOLD) {
            event_t health_event = {
                .type = EVENT_HEALTH_WARNING,
                .timestamp = esp_timer_get_time(),
                .payload = NULL,
                .payload_len = 0,
            };
            event_bus_publish(&health_event);
            ESP_LOGW(TAG, "Health warning: free heap %u < %dKB threshold", (unsigned)current_free, HEAP_WARNING_THRESHOLD / 1024);
        }

        // Per-task stack high water marks (diagnostic)
        TaskStatus_t task_stats[20];
        UBaseType_t task_count = uxTaskGetSystemState(task_stats, 20, NULL);
        ESP_LOGD(TAG, "Task stack high water marks:");
        for (UBaseType_t i = 0; i < task_count; i++) {
            ESP_LOGD(TAG, "  %s: %u bytes free", task_stats[i].pcTaskName,
                     (unsigned)uxTaskGetStackHighWaterMark(task_stats[i].xHandle) * sizeof(StackType_t));
        }

        /* httpd :80 self-heal: probe every cycle (30s).
         * 4 consecutive failures (120s unresponsive) → reboot.
         * 2026-09-03 (PIT-002 家族规则): WiFi 未连接时探测必失败（EHOSTUNREACH/EMFILE
         * 也要占用插座），此时不计数——那是网络不在，不是 httpd 死了；否则掉线 120s
         * 会被翻译成重启，越重启越乱（ai-thinker 同款事故）。 */
        static int httpd_stuck_count = 0;
        if (!probe_httpd_port80()) {
            wifi_state_t probe_ws = wifi_get_state();
            if (probe_ws != WIFI_STATE_STA_CONNECTED && probe_ws != WIFI_STATE_AP) {
                httpd_stuck_count = 0;
                ESP_LOGW(TAG, "httpd probe failed but WiFi down — not counting (network issue, not httpd)");
            } else {
                httpd_stuck_count++;
                ESP_LOGW(TAG, "httpd :80 probe failed (%d/4)", httpd_stuck_count);
                if (httpd_stuck_count >= 4) {
                    ESP_LOGE(TAG, "httpd :80 unresponsive for 120s — rebooting");
                    esp_restart();
                }
            }
        } else {
            httpd_stuck_count = 0;
        }
        
        vTaskDelay(pdMS_TO_TICKS(30000)); // 30 seconds
    }
}

esp_err_t health_monitor_init(void) {
    // Initialize temperature sensor
    // 量程 (20,100)：S3 固定档为 [-10,80]/[20,100]/[50,125]/[-30,50]，摄像头长期运行
    // 会超过 50°C，原 (10,50) 档在发热后读数超量程失真
    temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
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

    // Record heap baselines
    s_baseline_free_heap = esp_get_free_heap_size();
    s_baseline_min_heap = esp_get_minimum_free_heap_size();
    ESP_LOGI(TAG, "Heap baselines recorded: free=%u, min=%u",
             (unsigned)s_baseline_free_heap, (unsigned)s_baseline_min_heap);
    
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

void health_get_baselines(size_t *free_heap_out, size_t *min_heap_out) {
    if (free_heap_out) *free_heap_out = s_baseline_free_heap;
    if (min_heap_out) *min_heap_out = s_baseline_min_heap;
}

void health_check_threshold(size_t *free_heap_out, bool *warning_out) {
    size_t current = esp_get_free_heap_size();
    if (free_heap_out) *free_heap_out = current;
    if (warning_out) *warning_out = (current < HEAP_WARNING_THRESHOLD);
}

float get_chip_temp(void) {
    return read_temperature_sensor();
}