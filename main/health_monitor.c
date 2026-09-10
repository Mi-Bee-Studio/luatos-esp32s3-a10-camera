#include "wifi_manager.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "health_monitor.h"
#include "web_server.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "ping/ping_sock.h"
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

/* --- 链路旁证（PIT-040）：探测失败时区分"httpd 真瘫"与"链路失聪" ---
 * 弱链失聪窗里 localhost 探针会一起超时（httpd 会话被搁浅发送占满 /
 * tcpip 线程拥塞），旧逻辑照数不误 → 4/4 → 整机重启，而重启治不了射频。
 * 网关 2 发小包 ICMP（300ms 间隔、800ms 超时），任一应答即链路活着；
 * 网关地址不可得时返回 false，由调用侧按 WiFi 状态分流。 */
static SemaphoreHandle_t s_ping_done_sem = NULL;
static volatile bool s_ping_got_reply;

static void ping_success_cb(esp_ping_handle_t hdl, void *args)
{
    s_ping_got_reply = true;
}

static void ping_end_cb(esp_ping_handle_t hdl, void *args)
{
    xSemaphoreGive(s_ping_done_sem);
}

static bool gateway_link_alive(void)
{
    esp_netif_t *netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip;
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.gw.addr == 0) {
        return false;
    }
    if (!s_ping_done_sem) {
        s_ping_done_sem = xSemaphoreCreateBinary();
        if (!s_ping_done_sem) return false;
    }

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = 2;
    cfg.interval_ms = 300;
    cfg.timeout_ms = 800;
    cfg.data_size = 16;
    /* 默认 ≈2.7-3KB（2048+TASK_EXTRA）；失聪窗里堆被搁浅发送压到 3~6KB，
     * 默认栈屡屡建不起来（2026-09-09 实测 create ping task failed）——
     * ICMP 收发+给信号量 2048 足够，尽量保住旁证可测性 */
    cfg.task_stack_size = 2048;
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = ip.gw.addr;

    esp_ping_callbacks_t cbs = {
        .cb_args = NULL,
        .on_ping_success = ping_success_cb,
        .on_ping_timeout = NULL,
        .on_ping_end = ping_end_cb,
    };
    esp_ping_handle_t ping = NULL;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK || !ping) {
        return false;
    }
    s_ping_got_reply = false;
    xSemaphoreTake(s_ping_done_sem, 0);
    esp_ping_start(ping);
    bool alive = false;
    if (xSemaphoreTake(s_ping_done_sem, pdMS_TO_TICKS(4000))) {
        alive = s_ping_got_reply;
    }
    esp_ping_stop(ping);
    esp_ping_delete_session(ping);
    return alive;
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
         * 2026-09-03 (PIT-002 家族规则): WiFi 未连接时探测必失败（EHOSTUNREACH/EMFILE
         * 也要占用插座），此时不计数——那是网络不在，不是 httpd 死了；否则掉线 120s
         * 会被翻译成重启，越重启越乱（ai-thinker 同款事故）。
         * 2026-09-09（PIT-040，luatos 2026-09-08 晚 3 次连环误杀实录）两处补洞：
         *  - boot 假种子：health（Step 7）先于 web_server（STA 连上后才起）启动，
         *    首轮探测必失败 → 每 boot 白送 1/4；现在 httpd 未起直接不计。
         *  - 已连接但半聋：失聪窗里 localhost 探针一起超时，旧逻辑照数不误 →
         *    4/4 → 整机重启（重启治不了射频，反而把流/NVR 一起打断）。现在
         *    探测失败先 ping 网关旁证：链路活着才累计 httpd 罪名（4 次重启，
         *    保持原行为）；链路失聪走快速重联——连续 3 次（≈90s+ 持续失聪）
         *    主动断开重连，连不上则由 wifi_manager 既有逻辑 3 败切备用网。 */
        static int httpd_stuck_count = 0;
        static int link_deaf_count = 0;
        /* 升级阀：失聪判定若连续 3 轮强制重联都换不来一次探测成功，说明
         * 不是射频坏窗而是设备侧真瘫（含"ping 建不起来+链路其实活着"的
         * 误判死循环）——回到重启兜底，避免永久重联不复位。 */
        static int forced_reassocs_no_recovery = 0;
        if (web_server_get_handle() == NULL && uptime < 300) {
            /* httpd 尚未启动（boot 早期 / STA 未连上的延迟启动）：不计。
             * 注意 5min 宽限后不再豁免：web 迟迟不起 = STA 卡 CONNECTING
             * 的楔死态（服务全部延迟启动），必须走失聪升级链自愈——
             * 2026-09-09 楔死实录：无此宽限时限的话本门永远静默。 */
        } else if (web_server_get_handle() == NULL) {
            link_deaf_count++;
            ESP_LOGW(TAG, "web server not up after %llus — stuck CONNECTING? deaf (%d/3)",
                     (unsigned long long)uptime, link_deaf_count);
            if (link_deaf_count >= 3) {
                link_deaf_count = 0;
                if (++forced_reassocs_no_recovery >= 3) {
                    ESP_LOGE(TAG, "3 forced re-assocs without recovery — device-side wedge, rebooting");
                    esp_restart();
                }
                wifi_manager_force_reassoc();
            }
        } else if (!probe_httpd_port80()) {
            wifi_state_t probe_ws = wifi_get_state();
            if (probe_ws != WIFI_STATE_STA_CONNECTED && probe_ws != WIFI_STATE_AP) {
                httpd_stuck_count = 0;
                link_deaf_count = 0;
                ESP_LOGW(TAG, "httpd probe failed but WiFi down — not counting (network issue, not httpd)");
            } else if (probe_ws == WIFI_STATE_STA_CONNECTED && !gateway_link_alive()) {
                httpd_stuck_count = 0;
                link_deaf_count++;
                ESP_LOGW(TAG, "httpd probe failed, gateway ping lost — link deaf (%d/3)", link_deaf_count);
                if (link_deaf_count >= 3) {
                    link_deaf_count = 0;
                    if (++forced_reassocs_no_recovery >= 3) {
                        ESP_LOGE(TAG, "3 forced re-assocs without recovery — device-side wedge, rebooting");
                        esp_restart();
                    }
                    wifi_manager_force_reassoc();
                }
            } else {
                link_deaf_count = 0;
                httpd_stuck_count++;
                ESP_LOGW(TAG, "httpd :80 probe failed, link alive (%d/4)", httpd_stuck_count);
                if (httpd_stuck_count >= 4) {
                    ESP_LOGE(TAG, "httpd :80 unresponsive for 120s with link alive — rebooting");
                    esp_restart();
                }
            }
        } else {
            httpd_stuck_count = 0;
            link_deaf_count = 0;
            forced_reassocs_no_recovery = 0;
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