/*
 * WiFi 管理模块实现
 * 功能：状态机驱动的 WiFi AP/STA 管理，自动重连，回调通知
 */

#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include "nvs.h"
#include "event_bus.h"
#ifdef CONFIG_MIBEECAM_ENABLE_MDNS
#include "mdns.h"
#endif
#include "config_manager.h"

static const char *TAG = "wifi_manager";

ESP_EVENT_DEFINE_BASE(WIFI_MANAGER_EVENTS);
// --- Static state ---
static wifi_state_t s_state = WIFI_STATE_AP;
static char s_ip_str[16] = "0.0.0.0";
static wifi_state_callback_t s_callback = NULL;
static void *s_user_data = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

// Event handler instances (needed for unregister)
static esp_event_handler_instance_t s_wifi_handler = NULL;
static esp_event_handler_instance_t s_ip_handler = NULL;
static esp_event_handler_instance_t s_state_handler = NULL;

// WiFi retry timer (non-blocking)
static esp_timer_handle_t s_retry_timer = NULL;

// STA reconnect state
static int s_retry_count = 0;
#define RETRY_DELAY_S 10
#define RETRY_LOG_INTERVAL 10  // Log every N retries

#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
/* 双网络故障转移（2026-09-09 按 n16r8 配方移植，AT 契约 v1.2）：
 * 连败快速切换（替代旧的 3 败切备用）+ 切换上限轮换（消灭"备用败→AP 死路"）
 * + DHCP 盲区判切（关联后无 IP）+ 开机 RSSI 择优（≥8dB 规则 + NVS last_net 记忆）。 */
static int  s_active_ssid_index    = 0;     // 0=primary, 1=backup
static int  s_net_switches         = 0;     // 本轮开机的网络切换次数（防乒乓，连上即清零）
static bool s_expected_disconnect  = false; // 自致断开（stop/force_reassoc）：不计连败不触发切网
static esp_timer_handle_t s_dhcp_timer = NULL; // 盲区判定 12s 一次性定时器
#define NET_FAILS_SWITCH  2      /* 当前网连续失败 N 次后切另一网（n16r8 同款） */
#define NET_MAX_SWITCHES  6      /* 网络切换总次数上限，超过转 AP 兜底 */
#define STA_MAX_RETRIES   3      /* 无处可切时的重试上限，超过转 AP（n16r8 同款） */
#define DHCP_TIMEOUT_MS   12000  /* 关联后无 IP 判 DHCP 盲区（ai 教训，n16r8 配方） */

static bool secondary_configured(void)
{
    const cam_config_t *cfg = config_get();
    return cfg->wifi_ssid_2[0] != '\0' && cfg->wifi_pass_2[0] != '\0';
}

static void save_last_net(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi_pref", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "last_net", s_active_ssid_index ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool load_last_net(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("wifi_pref", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "last_net", &v);
        nvs_close(h);
    }
    return v == 1;
}
#endif
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void notify_state(wifi_state_t new_state);
static void wifi_retry_timer_callback(void *arg);
static void wifi_state_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data);
#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
static bool failover_to_other_net(const char *why);
static void dhcp_blind_timer_cb(void *arg);
#endif
// --- Helper ---
static void set_state(wifi_state_t new_state)
{
    if (s_state != new_state) {
        ESP_LOGI(TAG, "State: %d -> %d", s_state, new_state);
        s_state = new_state;
        notify_state(new_state);
        // Publish WiFi state change event
        event_t wifi_event = {
            .type = EVENT_WIFI_STATE_CHANGED,
            .timestamp = esp_timer_get_time(),
            .payload = NULL,
            .payload_len = 0,
        };
        event_bus_publish(&wifi_event);
    }
}

// --- Retry timer callback ---
static void wifi_retry_timer_callback(void *arg)
{
    ESP_LOGI(TAG, "Non-blocking retry: attempt %d", s_retry_count);
    // Retry connection without disconnecting first (avoids retry cascade)
    // Keep AP list cached for correct auth channel
    esp_wifi_connect();
}

static void wifi_state_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data)
{
    wifi_state_t new_state = (wifi_state_t)event_id;
    if (s_callback) {
        s_callback(new_state, s_user_data);
    }
}

static void notify_state(wifi_state_t new_state)
{
    // Post to event loop instead of calling directly — WiFi event handlers run
    // in the "wifi" task context (limited stack). Defer to the event loop task.
    esp_event_post(WIFI_MANAGER_EVENTS, (int32_t)new_state, NULL, 0, portMAX_DELAY);
}

// --- Infinite retry (no AP fallback) ---
static void wifi_schedule_retry(uint32_t delay_s)
{
    ESP_LOGI(TAG, "STA retry scheduled (%d attempts, retrying indefinitely)", s_retry_count);
    set_state(WIFI_STATE_STA_DISCONNECTED);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_s * 1000000ULL);
}

// --- Event handler ---
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started, connecting...");
            set_state(WIFI_STATE_STA_CONNECTING);
            // Do NOT call esp_wifi_connect() here — wifi_start_sta() already called it
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA connected to AP, waiting for IP...");
            set_state(WIFI_STATE_STA_CONNECTING);
#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
            /* 关联成功起 12s 盲区窗：超时未拿到 IP = DHCP 盲区（主网弱态
             * 典型症状），到期由 dhcp_blind_timer_cb 直接切网。 */
            if (s_dhcp_timer && !esp_timer_is_active(s_dhcp_timer)) {
                esp_timer_start_once(s_dhcp_timer, DHCP_TIMEOUT_MS * 1000ULL);
            }
#endif
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
            if (s_dhcp_timer && esp_timer_is_active(s_dhcp_timer)) {
                esp_timer_stop(s_dhcp_timer);   /* 关联已消失，盲区窗作废 */
            }
            if (s_expected_disconnect || disconn->reason == WIFI_REASON_ASSOC_LEAVE) {
                /* 自致断开（wifi_start_sta/wifi_start_ap 的 stop、force_reassoc）：
                 * 不计连败、不触发切网；重连由发起方自行调度。判据双保险——
                 * 旗标可能因 stop 时 radio 未启动（无事件）而悬空（09:18 实录
                 * bug），reason==8(ASSOC_LEAVE) 是自致断开的稳定签名。 */
                s_expected_disconnect = false;
                ESP_LOGI(TAG, "expected disconnect (self-initiated, reason=%d) — not counting",
                         disconn->reason);
                break;
            }
#endif
            s_retry_count++;

#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
            /* 连败快速切换：当前网 2 败先切另一网（n16r8 配方），再谈重试/AP。
             * 旧的"主网 3 败→备用，备用 >6 → AP 死路"已被替代。 */
            if (s_retry_count >= NET_FAILS_SWITCH &&
                failover_to_other_net("connect failures")) {
                break;
            }
            if (s_retry_count >= STA_MAX_RETRIES) {
                ESP_LOGW(TAG, "STA max retries reached (nowhere to switch) — AP fallback");
                wifi_start_ap();
                break;
            }
#endif
            // Log every RETRY_LOG_INTERVAL attempts, or first few
            if (s_retry_count <= 3 || s_retry_count % RETRY_LOG_INTERVAL == 0) {
                ESP_LOGW(TAG, "STA disconnected, reason=%d (0x%x), retrying (attempt %d, no fallback)",
                         disconn->reason, disconn->reason, s_retry_count);
            }
            // Don't clear AP list — let ESP-IDF cache scan results for faster reconnect
            wifi_schedule_retry(RETRY_DELAY_S);
            break;
        }

        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP started (SSID: MiBeeCam)");
            set_state(WIFI_STATE_AP);
#ifdef CONFIG_MIBEECAM_ENABLE_MDNS
            {
                char ap_hostname[40];
                const char *base = config_get()->mdns_hostname;
                snprintf(ap_hostname, sizeof(ap_hostname), "%s-ap", base ? base : "mibee");
                wifi_start_mdns(ap_hostname);
            }
#endif
            break;

        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "AP stopped");
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "STA got IP: %s", s_ip_str);
            s_retry_count = 0;
#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
            s_net_switches = 0;     /* 连上即清零：允许下次掉线再转移（n16r8 同款） */
            if (s_dhcp_timer && esp_timer_is_active(s_dhcp_timer)) {
                esp_timer_stop(s_dhcp_timer);
            }
            save_last_net();        /* 上次拿到 IP 的网络，下次开机优先 */
            // Note: s_active_ssid_index stays as-is (successfully connected)
#endif
            set_state(WIFI_STATE_STA_CONNECTED);
#ifdef CONFIG_MIBEECAM_ENABLE_MDNS
            // Start mDNS with configured hostname
            const char *hostname = config_get()->mdns_hostname;
            if (hostname && hostname[0]) {
                wifi_start_mdns(hostname);
            }
#endif
        }
    }
}

// --- Public API ---

#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
/* 切到另一张网（有配置且未达上限才切）；返回 true = 已发起切换。
 * n16r8 配方：连上即清零 s_net_switches，上限只在"两网都连不上"的
 * 循环里触顶 → 调用方转 AP 兜底。 */
static bool failover_to_other_net(const char *why)
{
    if (s_net_switches >= NET_MAX_SWITCHES) {
        ESP_LOGW(TAG, "net switch cap reached (%d) — leaving to caller", s_net_switches);
        return false;
    }
    int target = s_active_ssid_index ^ 1;
    if (target == 1 && !secondary_configured()) {
        return false;   /* nowhere to go */
    }
    s_net_switches++;
    s_retry_count = 0;
    ESP_LOGW(TAG, "%s — switching to %s network (switch %d/%d)",
             why, target ? "backup" : "primary", s_net_switches, NET_MAX_SWITCHES);
    event_t event = {
        .type = EVENT_WIFI_SWITCHED_SSID,
        .timestamp = esp_timer_get_time(),
        .payload = NULL,
        .payload_len = 0,
    };
    event_bus_publish(&event);
    const cam_config_t *cfg = config_get();
    if (target == 1) {
        wifi_start_sta(cfg->wifi_ssid_2, cfg->wifi_pass_2);
    } else {
        wifi_start_sta(cfg->wifi_ssid, cfg->wifi_pass);
    }
    return true;
}

/* DHCP 盲区判定：关联后 12s 无 IP。这里只做轻动作——主动断开一次，
 * 产生的 DISCONNECTED 事件进入正常计败路径（2 败切网）。绝不在 esp_timer
 * 任务上下文里调 wifi_start_sta/esp_wifi_stop（小栈 + 可能的 wifi 任务
 * 互锁，2026-09-09 楔死排查的教训）。 */
static void dhcp_blind_timer_cb(void *arg)
{
    if (wifi_get_state() == WIFI_STATE_STA_CONNECTED) {
        return;   /* 拿到 IP 与到期竞态（GOT_IP 已 stop，这里双保险） */
    }
    ESP_LOGW(TAG, "associated but no IP in %dms (DHCP blind spot) — forcing disconnect", DHCP_TIMEOUT_MS);
    esp_wifi_disconnect();   /* 事件路径计败并重试/切换 */
}
#endif


esp_err_t wifi_init(void)
{
    esp_err_t ret;

    ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        ESP_LOGE(TAG, "Failed to create AP netif");
        return ESP_FAIL;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "Failed to create STA netif");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }


    // Set country code for regulatory compliance
    ret = esp_wifi_set_country_code("CN", false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set country code: %s", esp_err_to_name(ret));
    }


    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, &s_wifi_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, NULL, &s_ip_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
        return ret;
    }
    // Register custom event handler to dispatch state callbacks on the event loop task
    ret = esp_event_handler_instance_register(WIFI_MANAGER_EVENTS, ESP_EVENT_ANY_ID,
                                              &wifi_state_event_handler, NULL, &s_state_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register state event handler: %s", esp_err_to_name(ret));
        return ret;
    }


    // Create one-shot retry timer
    esp_timer_create_args_t timer_args = {
        .callback = wifi_retry_timer_callback,
        .name = "wifi_retry"
    };
    ret = esp_timer_create(&timer_args, &s_retry_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create WiFi retry timer: %s", esp_err_to_name(ret));
        return ret;
    }
#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
    /* DHCP 盲区窗（关联后 12s 无 IP → 切网，n16r8 配方） */
    esp_timer_create_args_t dhcp_args = {
        .callback = dhcp_blind_timer_cb,
        .name = "wifi_dhcp_blind"
    };
    ret = esp_timer_create(&dhcp_args, &s_dhcp_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create DHCP blind timer: %s", esp_err_to_name(ret));
        s_dhcp_timer = NULL;   /* 盲区判定缺席不致命：连败转移仍在 */
    }
#endif
    s_state = WIFI_STATE_AP;
    s_retry_count = 0;
    memset(s_ip_str, 0, sizeof(s_ip_str));
    strcpy(s_ip_str, "0.0.0.0");

    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_start_ap(void)
{
    esp_err_t ret;

#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
    if (s_dhcp_timer && esp_timer_is_active(s_dhcp_timer)) {
        esp_timer_stop(s_dhcp_timer);   /* 转 AP：盲区窗作废 */
    }
#endif

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "MiBeeCam",
            .ssid_len = 8,
            .password = "mibeecam2026",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };

    ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(ret));
    }
#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
    if (ret == ESP_OK) {
        s_expected_disconnect = true;   /* stop 在跑 radio 的断开事件不计连败 */
    }
#endif

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AP mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AP config: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start AP: %s", esp_err_to_name(ret));
        return ret;
    }

    // State set by WIFI_EVENT_AP_START handler
    ESP_LOGI(TAG, "AP starting: SSID=MiBeeCam");
    return ESP_OK;
}

/* 开机择优的临时扫描会话（2026-09-09 楔死排查后重构）：仅起栈→扫→停，
 * 不触碰凭据、不连接。真正的连接路径 = 下面的原版 wifi_start_sta
 * （config 在 start 之前应用的久经考验顺序，与移植前逐字节一致）。 */
#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
static int boot_pick_scan(const char *p1, const char *p2)
{
    /* 返回 0=primary 1=backup；≥8dB 者胜出，平局/双缺 = -1（调用方保持
     * last_net），主网不在空中而备网在 → 1（n16r8 同款判据）。 */
    wifi_scan_config_t sc = { 0 };
    sc.show_hidden = false;
    int pick = -1;
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return -1;
    if (esp_wifi_start() != ESP_OK) return -1;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        wifi_ap_record_t *recs = malloc(sizeof(wifi_ap_record_t) * (n ? n : 1));
        if (recs) {
            if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
                int8_t r1 = -128, r2 = -128;
                for (uint16_t i = 0; i < n; i++) {
                    if (strcmp((const char *)recs[i].ssid, p1) == 0 && recs[i].rssi > r1) r1 = recs[i].rssi;
                    if (strcmp((const char *)recs[i].ssid, p2) == 0 && recs[i].rssi > r2) r2 = recs[i].rssi;
                }
                if (r1 > -128 && r2 > -128) {
                    if (r2 - r1 >= 8)      pick = 1;
                    else if (r1 - r2 >= 8) pick = 0;
                    ESP_LOGI(TAG, "boot pick: '%s' %ddBm vs '%s' %ddBm → %s",
                             p1, r1, p2, r2,
                             pick < 0 ? "last_net" : (pick ? "backup" : "primary"));
                } else if (r2 > -128 && r1 == -128) {
                    pick = 1;   /* 主网不在空中 */
                    ESP_LOGI(TAG, "boot pick: primary not on air, backup %ddBm → backup", r2);
                } else {
                    ESP_LOGW(TAG, "boot pick: neither net visible — using last_net");
                }
            }
            free(recs);
        }
    } else {
        ESP_LOGW(TAG, "boot pick: scan failed — using last_net");
    }
    esp_wifi_stop();   /* 扫描会话结束；连接由 wifi_start_sta 全新起栈 */
    return pick;
}
#endif

esp_err_t wifi_start_sta(const char *ssid, const char *pass)
{
    if (!ssid || !pass) {
        return ESP_ERR_INVALID_ARG;
    }

#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
    // Track which SSID index we're connecting to
    const cam_config_t *cfg = config_get();
    if (strcmp(ssid, cfg->wifi_ssid) == 0) {
        s_active_ssid_index = 0;
    } else if (strcmp(ssid, cfg->wifi_ssid_2) == 0) {
        s_active_ssid_index = 1;
    }
#endif

    esp_err_t ret;

    wifi_config_t wifi_config = {0};  // zero-init ALL fields
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;  // auto-negotiate any auth mode
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.listen_interval = 3;  // lower = better multicast, less likely rejected
    /* 全信道扫描：快速扫描依赖 scan 缓存，频繁 stop/切换后缓存过期 →
     * connect 报 201 NO_AP_FOUND（2026-09-09 切换风暴实录）。全信道慢
     * ~2s 但每次连接都真实扫一遍——弱链板正确性优先。 */
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set STA mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set STA config: %s", esp_err_to_name(ret));
        return ret;
    }
    /* Set DHCP hostname so router shows device name instead of "espressif" */
    const char *dev_name = config_get()->device_name;
    if (dev_name && dev_name[0]) {
        esp_netif_set_hostname(s_sta_netif, dev_name);
    }
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start STA: %s", esp_err_to_name(ret));
        return ret;
    }

    // Disable power save to avoid missing EAPOL/auth frames
    ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable PS: %s", esp_err_to_name(ret));
    }

    /* HT20 (强制 BW20)：本板曾与 HT40 AP (ch11) 谈到 40MHz，-70dBm 下 PER 恶化、
     * ping 1-2.4s。HT20 比 HT40 RX 灵敏度好 ~3dB —— ai-thinker 弱信号同款配置。 */
    ret = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "set_bandwidth HT20 failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "WiFi bandwidth: HT20 (weak-signal optimized)");
    }

    // Boost TX power to 15 dBm (same as working seeed-esp32s3-cam)
    {
        int8_t power_param = (int8_t)(15 / 0.25);
        esp_err_t pwr_err = esp_wifi_set_max_tx_power(power_param);
        if (pwr_err == ESP_OK) {
            ESP_LOGI(TAG, "WiFi TX power set to 15 dBm");
        } else {
            ESP_LOGW(TAG, "Failed to set WiFi TX power: %s", esp_err_to_name(pwr_err));
        }
    }

    esp_wifi_connect();

    s_retry_count = 0;
    memset(s_ip_str, 0, sizeof(s_ip_str));
    strcpy(s_ip_str, "0.0.0.0");

    ESP_LOGI(TAG, "STA starting, connecting to %s", ssid);
    return ESP_OK;
}

#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
/* 开机入口（main.c Step 8）：last_net 记忆为默认，双网异名时临时扫描
 * 会话比 RSSI（≥8dB 规则），然后走原版 wifi_start_sta 连接。 */
esp_err_t wifi_start_sta_boot(void)
{
    const cam_config_t *cfg = config_get();
    if (!cfg->wifi_ssid[0] || !cfg->wifi_pass[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    s_active_ssid_index = (load_last_net() && secondary_configured()) ? 1 : 0;

    if (secondary_configured() && strcmp(cfg->wifi_ssid, cfg->wifi_ssid_2) != 0) {
        int pick = boot_pick_scan(cfg->wifi_ssid, cfg->wifi_ssid_2);
        if (pick >= 0) {
            s_active_ssid_index = pick;
        }
    }

    return wifi_start_sta(s_active_ssid_index ? cfg->wifi_ssid_2 : cfg->wifi_ssid,
                          s_active_ssid_index ? cfg->wifi_pass_2 : cfg->wifi_pass);
}
#endif

wifi_state_t wifi_get_state(void)
{
    return s_state;
}

const char *wifi_get_ip_str(void)
{
    return s_ip_str;
}

#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
int wifi_get_current_ssid_index(void)
{
    wifi_state_t state = wifi_get_state();
    if (state == WIFI_STATE_AP) return -1;
    if (state != WIFI_STATE_STA_CONNECTED) return s_active_ssid_index;
    return s_active_ssid_index;
}
#endif

void wifi_manager_force_reassoc(void)
{
    /* 只断开、不重配：PIT-040 塌方自愈用。断开事件被 s_expected_disconnect
     * 旗标吸收（不计连败、不触发切网），1s 后快速回连同网；若重连不上，
     * 后续断开事件恢复正常计败，2 败自动切另一网。 */
    ESP_LOGW(TAG, "forcing STA re-association (link collapse recovery)");
#ifdef CONFIG_MIBEECAM_ENABLE_BACKUP_SSID
    s_expected_disconnect = true;
#endif
    esp_wifi_disconnect();
    wifi_schedule_retry(1);
}

esp_err_t wifi_register_callback(wifi_state_callback_t cb, void *user_data)
{
    s_callback = cb;
    s_user_data = user_data;
    return ESP_OK;
}

esp_err_t wifi_stop(void)
{
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "Failed to stop WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    memset(s_ip_str, 0, sizeof(s_ip_str));
    strcpy(s_ip_str, "0.0.0.0");
    s_retry_count = 0;
    set_state(WIFI_STATE_AP);

    ESP_LOGI(TAG, "WiFi stopped");
    return ESP_OK;
}

esp_err_t wifi_stop_retry(void)
{
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    return ESP_OK;
}

esp_err_t wifi_manager_deinit(void)
{
    esp_err_t ret;

    // Unregister event handlers
    if (s_wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler = NULL;
    }
    if (s_ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_handler = NULL;
    }
    if (s_state_handler) {
        esp_event_handler_instance_unregister(WIFI_MANAGER_EVENTS, ESP_EVENT_ANY_ID, s_state_handler);
        s_state_handler = NULL;
    }


    // Stop and delete retry timer
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
        esp_timer_delete(s_retry_timer);
        s_retry_timer = NULL;
    }
    ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop during deinit: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_deinit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Destroy netifs
    if (s_sta_netif) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }

    ret = esp_event_loop_delete_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "event loop delete: %s", esp_err_to_name(ret));
    }

    s_callback = NULL;
    s_user_data = NULL;
    s_state = WIFI_STATE_AP;
    s_retry_count = 0;
    memset(s_ip_str, 0, sizeof(s_ip_str));
    strcpy(s_ip_str, "0.0.0.0");

    ESP_LOGI(TAG, "WiFi manager deinitialized");
    return ESP_OK;
}

#ifdef CONFIG_MIBEECAM_ENABLE_MDNS

static bool s_mdns_started = false;

esp_err_t wifi_start_mdns(const char *hostname)
{
    if (hostname == NULL || hostname[0] == '\0') {
        ESP_LOGW(TAG, "mDNS: hostname is empty, skipping");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mdns_started) {
        ESP_LOGI(TAG, "mDNS already running, stopping first");
        wifi_stop_mdns();
    }

    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set hostname
    ret = mdns_hostname_set(hostname);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS hostname set failed: %s", esp_err_to_name(ret));
        mdns_free();
        return ret;
    }

    // Set instance name
    mdns_instance_name_set("MiBeeCam");

    // Add HTTP service on port 80
    ret = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS service add failed: %s", esp_err_to_name(ret));
        // Non-fatal — hostname resolution still works
    }

    s_mdns_started = true;
    ESP_LOGI(TAG, "mDNS started: %s.local", hostname);
    return ESP_OK;
}

esp_err_t wifi_stop_mdns(void)
{
    if (s_mdns_started) {
        mdns_free();
        s_mdns_started = false;
        ESP_LOGI(TAG, "mDNS stopped");
    }
    return ESP_OK;
}
#endif // CONFIG_MIBEECAM_ENABLE_MDNS

#ifdef CONFIG_MIBEECAM_ENABLE_WIFI_SCAN
esp_err_t wifi_scan(wifi_ap_record_t *results, uint16_t max_count, uint16_t *found_count)
{
    if (results == NULL || found_count == NULL || max_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *found_count = 0;

    // Configure scan: active, 100ms per channel
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,  // all channels
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    scan_config.scan_time.active.min = 0;
    scan_config.scan_time.active.max = 100;  // 100ms per channel

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);  // blocking
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi_scan_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint16_t ap_count = 0;
    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        return ret;
    }

    if (ap_count > max_count) {
        ap_count = max_count;
    }

    ret = esp_wifi_scan_get_ap_records(&ap_count, results);
    if (ret != ESP_OK) {
        return ret;
    }

    *found_count = ap_count;
    ESP_LOGI(TAG, "WiFi scan found %d networks", ap_count);
    return ESP_OK;
}
#endif // CONFIG_MIBEECAM_ENABLE_WIFI_SCAN
