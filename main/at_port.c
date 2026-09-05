/*
 * at_port.c — luatos ESP32-S3-A10 板级 port 层（家族 AT 核心 at_command.c 的钩子）
 *
 * IO：UART0（CH343，宿主侧 /dev/ttyACM1）115200-8N1，VFS+fgets（承袭旧
 * at_command.c 的成熟路径；console 已占口时优雅降级复用现有 stdin/stdout）。
 *
 * 生效语义（契约 §6/§7，本板三处全部登记）：
 *  - 分辨率/画质变更：保存 + 1s 后重启（fb_count=1 DRAM 热重配在并发取帧下
 *    致设备级静默死亡，PIT-012——与 POST /api/camera 同款语义）；
 *  - WiFi 凭据写入：热连（保存后不停机切换 STA，本板独有，v1.1 登记）。
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"

#include "at_port.h"
#include "config_manager.h"
#include "camera_driver.h"
#include "wifi_manager.h"
#include "health_monitor.h"
#include "mjpeg_streamer.h"
#include "motion_detect.h"

static const char *TAG = "at_port";

/* ── 能力裁剪（契约 §2 ⬜ 项；wifi_scan 随 Kconfig）──────────────── */

#ifdef CONFIG_MIBEECAM_ENABLE_WIFI_SCAN
#define PORT_CAP_WIFI_SCAN true
#else
#define PORT_CAP_WIFI_SCAN false
#endif

static const at_caps_t s_caps = {
    .wifi_scan = PORT_CAP_WIFI_SCAN,
    .cfg       = true,
};

const at_caps_t *at_port_caps(void)
{
    return &s_caps;
}

/* ── IO：UART0 + VFS（fgets 行读；NULL 时 10ms 轮询让出核心）────── */

esp_err_t at_port_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_driver_install(UART_NUM_0, 512, 512, 20, NULL, 0);
    if (ret == ESP_OK) {
        /* Fresh install — configure and connect to VFS（stdin 方可 fgets）*/
        uart_param_config(UART_NUM_0, &uart_config);
        uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        uart_vfs_dev_use_driver(UART_NUM_0);
        ESP_LOGI(TAG, "UART0 driver installed for AT console");
    } else {
        /* Driver already installed by console subsystem — use existing */
        ESP_LOGW(TAG, "UART0 already in use (%s), using existing console",
                 esp_err_to_name(ret));
    }
    return ESP_OK;
}

int at_port_getline(char *buf, int len)
{
    if (!fgets(buf, len, stdin)) {
        vTaskDelay(pdMS_TO_TICKS(10));
        return -1;
    }
    /* 去 \r\n */
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) {
        buf[--n] = '\0';
    }
    fflush(stdout);
    return (int)n;
}

void at_port_write(const char *s)
{
    fputs(s, stdout);
    fflush(stdout);
}

/* ── 扩展指令的应答小框架（核心的 at_* 是 static，port 自备同款）─── */

static void ext_ok(void)
{
    at_port_write("OK\r\n");
}

static void ext_err(const char *why)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "ERROR: %s\r\n", why ? why : "unknown");
    at_port_write(buf);
}

static void ext_data(const char *name, const char *fmt, ...)
{
    char body[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    char line[128];
    snprintf(line, sizeof(line), "+%s: %s\r\n", name, body);
    at_port_write(line);
}

/* ── GMR / 传感器 ──────────────────────────────────────────────── */

void at_port_gmr_info(at_gmr_info_t *out)
{
    strlcpy(out->board, "luatos-a10", sizeof(out->board));
    /* 信设备不信文档：实戴传感器型号由 camera_driver 查组件能力表上报 */
    strlcpy(out->sensor, camera_get_sensor_name(), sizeof(out->sensor));
}

/* ── WiFi ──────────────────────────────────────────────────────── */

void at_port_wifi_info(at_wifi_info_t *out)
{
    memset(out, 0, sizeof(*out));
    switch (wifi_get_state()) {
        case WIFI_STATE_AP:              strlcpy(out->state, "ap", sizeof(out->state)); break;
        case WIFI_STATE_STA_CONNECTING:  strlcpy(out->state, "connecting", sizeof(out->state)); break;
        case WIFI_STATE_STA_CONNECTED:   strlcpy(out->state, "connected", sizeof(out->state)); break;
        default:                         strlcpy(out->state, "disconnected", sizeof(out->state)); break;
    }
    /* ssid = 当前实际连接（区别于配置值；未连接为空，契约 at_port.h 注）*/
    if (wifi_get_state() == WIFI_STATE_STA_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            strlcpy(out->ssid, (const char *)ap.ssid, sizeof(out->ssid));
        }
    }
    strlcpy(out->ip, wifi_get_ip_str(), sizeof(out->ip));

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            snprintf(out->gw, sizeof(out->gw), IPSTR, IP2STR(&ip.gw));
            snprintf(out->mask, sizeof(out->mask), IPSTR, IP2STR(&ip.netmask));
        }
    }
}

esp_err_t at_port_wifi_set(const char *ssid, const char *pass)
{
    if (!ssid || !pass || !ssid[0] || !pass[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) >= 33 || strlen(pass) >= 65) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 保存凭据（事务式 copy+save，写即落盘）*/
    cam_config_t cfg;
    config_get_copy(&cfg);
    strlcpy(cfg.wifi_ssid, ssid, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, pass, sizeof(cfg.wifi_pass));
    esp_err_t ret = config_save(&cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 本板独有生效语义（契约 §6/§7）：热连——不重启，直接切换 STA。
     * 承袭旧 CWJAP= 路径：先停重连定时器（防状态机脱节）再连接；
     * 连接本身异步（结果经 AT+WIFI? / WS 事件观察），此处 fire-and-forget。 */
    at_port_write("+APPLY: live connect\r\n");
    wifi_stop_retry();
    ret = wifi_start_sta(ssid, pass);
    if (ret != ESP_OK) {
        /* 凭据已保存、重试机制仍在；连接启动失败只降级为日志 + 提示行 */
        ESP_LOGW(TAG, "live connect start failed: %s (credentials saved)",
                 esp_err_to_name(ret));
        at_port_write("+APPLY: connect pending (check AT+WIFI?)\r\n");
    }
    return ESP_OK;
}

esp_err_t at_port_wifi_scan(at_scan_emit_fn emit)
{
#ifdef CONFIG_MIBEECAM_ENABLE_WIFI_SCAN
    /* 承袭旧 CWLAP：wifi_manager 封装（~15 AP），RSSI 降序（契约 §2）*/
    wifi_ap_record_t recs[15];
    uint16_t found = 0;
    esp_err_t ret = wifi_scan(recs, sizeof(recs) / sizeof(recs[0]), &found);
    if (ret != ESP_OK) {
        return ret;
    }
    for (int i = 1; i < (int)found; i++) {
        wifi_ap_record_t key = recs[i];
        int j = i - 1;
        while (j >= 0 && recs[j].rssi < key.rssi) {
            recs[j + 1] = recs[j];
            j--;
        }
        recs[j + 1] = key;
    }
    for (int i = 0; i < (int)found; i++) {
        emit((const char *)recs[i].ssid, recs[i].rssi, recs[i].authmode);
    }
    return ESP_OK;
#else
    (void)emit;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* ── 摄像头 ────────────────────────────────────────────────────── */

/* 板级可选表（与 GET /api/camera supported_resolutions 同源）：
 * 板上限 VGA（board 层实测常数，PIT-012：SVGA 起 DRAM fb 枯竭螺旋）*/
static const at_res_opt_t s_res_opts[] = {
    { 10, "VGA (640x480)" },
};

bool at_port_cam_res_info(at_cam_res_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->cur = config_get()->cam_framesize;
    out->cur_label = (out->cur == (int)CAMERA_RES_VGA) ? "VGA" : "UNKNOWN";
    out->opts = s_res_opts;
    out->opt_count = (int)(sizeof(s_res_opts) / sizeof(s_res_opts[0]));
    out->cap = (int)camera_get_effective_max_res();
    out->cap_source = camera_res_cap_source();
    return true;
}

esp_err_t at_port_cam_res_set(int value)
{
    /* 板上限 VGA：唯一合法值 10，其余一律拒绝（与 POST /api/camera 同源）*/
    if (value != (int)CAMERA_RES_VGA) {
        return ESP_ERR_INVALID_ARG;
    }
    cam_config_t cfg;
    config_get_copy(&cfg);
    cfg.cam_framesize = (uint8_t)value;
    esp_err_t ret = config_save(&cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    /* 本板生效语义（契约 §6）：保存 + 1s 后重启应用（fb_count=1 DRAM
     * 热重配竞态，PIT-012——重启是干净的锤子，零竞态）*/
    at_port_write("+REBOOTING: camera change\r\n");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;   /* unreachable */
}

int at_port_cam_qual_get(int *qmin, int *qmax)
{
    if (qmin) *qmin = CAMERA_QUALITY_MIN;
    if (qmax) *qmax = CAMERA_QUALITY_MAX;
    return config_get()->cam_quality;
}

esp_err_t at_port_cam_qual_set(int value)
{
    if (value < CAMERA_QUALITY_MIN || value > CAMERA_QUALITY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (value == (int)config_get()->cam_quality) {
        return ESP_OK;   /* 同值：无需保存/重启（承袭旧 CAMQUAL 短路）*/
    }
    cam_config_t cfg;
    config_get_copy(&cfg);
    cfg.cam_quality = (uint8_t)value;
    esp_err_t ret = config_save(&cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    /* 本板生效语义（契约 §6）：保存 + 1s 后重启应用（同 CAMRES，PIT-012）*/
    at_port_write("+REBOOTING: camera change\r\n");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;   /* unreachable */
}

/* ── STATUS 板级增量行（核心最后调用）──────────────────────────── */

void at_port_status_extra(void (*emit)(const char *name, const char *value))
{
    char buf[24];
    /* 旧 AT+STATUS/STREAM? 的有价值增量：流客户端 + 侦测任务 + 温度 */
    snprintf(buf, sizeof(buf), "%d", mjpeg_streamer_get_client_count());
    emit("stream", buf);
    snprintf(buf, sizeof(buf), "%s", motion_detect_is_running() ? "running" : "stopped");
    emit("motion", buf);
    snprintf(buf, sizeof(buf), "%.2f", get_chip_temp());
    emit("temp", buf);
}

/* ── 系统动作 ──────────────────────────────────────────────────── */

void at_port_reboot(void)
{
    at_port_write("+REBOOTING\r\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void at_port_restore(void)
{
    at_port_write("+RESTORING: factory reset\r\n");
    config_reset();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ── CFGGET/CFGSET 白名单（契约 §2；读侧自动剔除 secret）────────── */

/* 字段行：name/type/secret/offset/maxsz（单一事实源 = cam_config_t 布局）*/
typedef struct {
    const char *name;
    at_cfg_type_t type;
    bool secret;
    size_t off;
    size_t maxsz;      /* STR 行的缓冲上限（含 null）；数值行为 0 */
} port_field_t;

#define PF_STR(nam, field)   { nam, AT_CFG_STR, false, offsetof(cam_config_t, field), sizeof(((cam_config_t *)0)->field) }
#define PF_SEC(nam, field)   { nam, AT_CFG_STR, true,  offsetof(cam_config_t, field), sizeof(((cam_config_t *)0)->field) }
#define PF_U8(nam, field)    { nam, AT_CFG_U8,  false, offsetof(cam_config_t, field), 0 }
#define PF_U16(nam, field)   { nam, AT_CFG_U16, false, offsetof(cam_config_t, field), 0 }

static const port_field_t s_fields[] = {
    PF_STR("device_name",              device_name),
    PF_STR("wifi_ssid",                wifi_ssid),
    PF_SEC("wifi_pass",                wifi_pass),
    PF_STR("wifi_ssid_2",              wifi_ssid_2),
    PF_SEC("wifi_pass_2",              wifi_pass_2),
    PF_SEC("web_password",             web_password),
    PF_STR("timezone",                 timezone),
    PF_U8("cam_framesize",             cam_framesize),
    PF_U8("cam_fps",                   cam_fps),
    PF_U8("cam_quality",               cam_quality),
    PF_U8("cam_vflip",                 cam_vflip),
    PF_U8("cam_hmirror",               cam_hmirror),
    PF_U8("xclk_freq_mhz",             xclk_freq_mhz),
    PF_U8("onvif_enable",              onvif_enable),
    PF_U8("ws_enable",                 ws_enable),
    PF_U8("motion_enabled",            motion_enabled),
    PF_U8("motion_sensitivity",        motion_sensitivity),
    PF_U16("motion_cooldown_s",        motion_cooldown_s),
    PF_U8("motion_active_interval_s",  motion_active_interval_s),
    PF_U8("alert_webhook_enabled",     alert_webhook_enabled),
    PF_STR("alert_webhook_url",        alert_webhook_url),
    PF_SEC("webhook_secret",           webhook_secret),
    PF_STR("server_url",               server_url),
    PF_STR("mdns_hostname",            mdns_hostname),
};

static const port_field_t *find_field(const char *name)
{
    for (size_t i = 0; i < sizeof(s_fields) / sizeof(s_fields[0]); i++) {
        if (strcasecmp(s_fields[i].name, name) == 0) {
            return &s_fields[i];
        }
    }
    return NULL;
}

static void field_get(const port_field_t *f, char *buf, size_t len)
{
    cam_config_t snap;
    config_get_copy(&snap);
    const void *p = (const uint8_t *)&snap + f->off;
    switch (f->type) {
        case AT_CFG_STR: snprintf(buf, len, "%s", (const char *)p); break;
        case AT_CFG_U8:  snprintf(buf, len, "%u", (unsigned)*(const uint8_t *)p); break;
        case AT_CFG_U16: snprintf(buf, len, "%u", (unsigned)*(const uint16_t *)p); break;
        case AT_CFG_I8:  snprintf(buf, len, "%d", (int)*(const int8_t *)p); break;
    }
}

/* 泛型 getter 生成器（核心的 at_cfg_field_t.get 无上下文指针 → 名字路由）*/
#define CFG_GET(nam) \
    static void cfg_get_##nam(char *buf, size_t len) \
    { \
        const port_field_t *f = find_field(#nam); \
        if (f) field_get(f, buf, len); \
    }

CFG_GET(device_name)
CFG_GET(wifi_ssid)
CFG_GET(wifi_ssid_2)
CFG_GET(timezone)
CFG_GET(cam_framesize)
CFG_GET(cam_fps)
CFG_GET(cam_quality)
CFG_GET(cam_vflip)
CFG_GET(cam_hmirror)
CFG_GET(xclk_freq_mhz)
CFG_GET(onvif_enable)
CFG_GET(ws_enable)
CFG_GET(motion_enabled)
CFG_GET(motion_sensitivity)
CFG_GET(motion_cooldown_s)
CFG_GET(motion_active_interval_s)
CFG_GET(alert_webhook_enabled)
CFG_GET(alert_webhook_url)
CFG_GET(server_url)
CFG_GET(mdns_hostname)

/* ── 写侧：事务式 copy+save（本仓 config_manager 无类型化 setter）── */

static bool parse_long(const char *v, long *out)
{
    char *end = NULL;
    *out = strtol(v, &end, 10);
    return !(end == v || *end != '\0');
}

static esp_err_t set_str_field(const port_field_t *f, const char *val, bool allow_empty)
{
    if (!val || (!allow_empty && !val[0]) || strlen(val) >= f->maxsz) {
        return ESP_ERR_INVALID_ARG;
    }
    cam_config_t cfg;
    config_get_copy(&cfg);
    strlcpy((char *)((uint8_t *)&cfg + f->off), val, f->maxsz);
    return config_save(&cfg);
}

static esp_err_t set_num_field(const port_field_t *f, long val)
{
    cam_config_t cfg;
    config_get_copy(&cfg);
    void *p = (uint8_t *)&cfg + f->off;
    if (f->type == AT_CFG_U8) {
        *(uint8_t *)p = (uint8_t)val;
    } else if (f->type == AT_CFG_U16) {
        *(uint16_t *)p = (uint16_t)val;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return config_save(&cfg);
}

static esp_err_t cfg_set_wifi_ssid(const char *v)
{
    /* 允许清空（回到 AP 配网态）；长度校验走 set_str_field */
    return set_str_field(find_field("wifi_ssid"), v, true);
}
static esp_err_t cfg_set_wifi_pass(const char *v)
{
    if (!v || !v[0]) return ESP_ERR_INVALID_ARG;   /* 清空凭据走 AT+CWQAP */
    return set_str_field(find_field("wifi_pass"), v, false);
}
static esp_err_t cfg_set_wifi_ssid_2(const char *v)
{
    /* 空串 = 禁用备用网络（承袭 config_set_wifi_secondary 语义）*/
    return set_str_field(find_field("wifi_ssid_2"), v, true);
}
static esp_err_t cfg_set_wifi_pass_2(const char *v)
{
    return set_str_field(find_field("wifi_pass_2"), v, true);
}
static esp_err_t cfg_set_web_password(const char *v)
{
    if (!v || strlen(v) < 6) return ESP_ERR_INVALID_ARG;   /* 契约 v1.1 下限 */
    return set_str_field(find_field("web_password"), v, false);
}
static esp_err_t cfg_set_device_name(const char *v)
{
    return set_str_field(find_field("device_name"), v, false);
}
static esp_err_t cfg_set_timezone(const char *v)
{
    /* 契约 §4：非空时长度 1-47；空串 = UTC（家族默认） */
    return set_str_field(find_field("timezone"), v, true);
}
static esp_err_t cfg_set_mdns_hostname(const char *v)
{
    return set_str_field(find_field("mdns_hostname"), v, false);
}
static esp_err_t cfg_set_server_url(const char *v)
{
    /* 契约 §7：板级扩展，可选字段 → 允许清空 */
    return set_str_field(find_field("server_url"), v, true);
}
static esp_err_t cfg_set_alert_webhook_url(const char *v)
{
    return set_str_field(find_field("alert_webhook_url"), v, true);
}
static esp_err_t cfg_set_webhook_secret(const char *v)
{
    return set_str_field(find_field("webhook_secret"), v, true);
}
static esp_err_t cfg_set_cam_framesize(const char *v)
{
    long val;
    if (!parse_long(v, &val)) return ESP_ERR_INVALID_ARG;
    return at_port_cam_res_set((int)val);   /* 保存+重启（PIT-012），同 web 路径 */
}
static esp_err_t cfg_set_cam_fps(const char *v)
{
    long val;
    if (!parse_long(v, &val) || val < 1 || val > 30) return ESP_ERR_INVALID_ARG;
    return set_num_field(find_field("cam_fps"), val);
}
static esp_err_t cfg_set_cam_quality(const char *v)
{
    long val;
    if (!parse_long(v, &val) || val < CAMERA_QUALITY_MIN || val > CAMERA_QUALITY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return set_num_field(find_field("cam_quality"), val);
}
static esp_err_t cfg_set_xclk(const char *v)
{
    long val;
    if (!parse_long(v, &val) || (val != 10 && val != 16 && val != 20)) {
        return ESP_ERR_INVALID_ARG;   /* 契约 §3.1 离散域 */
    }
    return set_num_field(find_field("xclk_freq_mhz"), val);
}
static esp_err_t cfg_set_cam_vflip(const char *v)
{
    long val;
    if (!parse_long(v, &val) || (val != 0 && val != 1)) return ESP_ERR_INVALID_ARG;
    return set_num_field(find_field("cam_vflip"), val);
}
static esp_err_t cfg_set_cam_hmirror(const char *v)
{
    long val;
    if (!parse_long(v, &val) || (val != 0 && val != 1)) return ESP_ERR_INVALID_ARG;
    return set_num_field(find_field("cam_hmirror"), val);
}
static esp_err_t cfg_set_onvif(const char *v)
{
    long val;
    if (!parse_long(v, &val) || (val != 0 && val != 1)) return ESP_ERR_INVALID_ARG;
    return set_num_field(find_field("onvif_enable"), val);
}
static esp_err_t cfg_set_ws(const char *v)
{
    long val;
    if (!parse_long(v, &val) || (val != 0 && val != 1)) return ESP_ERR_INVALID_ARG;
    return set_num_field(find_field("ws_enable"), val);
}
static esp_err_t cfg_set_motion_en(const char *v)
{
    long val;
    if (!parse_long(v, &val) || (val != 0 && val != 1)) return ESP_ERR_INVALID_ARG;
    return set_num_field(find_field("motion_enabled"), val);
}
static esp_err_t cfg_set_motion_sens(const char *v)
{
    long val;
    if (!parse_long(v, &val) || val < 0 || val > 100) return ESP_ERR_INVALID_ARG;
    return set_num_field(find_field("motion_sensitivity"), val);
}
static esp_err_t cfg_set_motion_cool(const char *v)
{
    long val;
    if (!parse_long(v, &val) || val < 1 || val > 300) return ESP_ERR_INVALID_ARG;
    return set_num_field(find_field("motion_cooldown_s"), val);
}
static esp_err_t cfg_set_motion_act(const char *v)
{
    long val;
    if (!parse_long(v, &val) || val < 1 || val > 30) return ESP_ERR_INVALID_ARG;
    return set_num_field(find_field("motion_active_interval_s"), val);
}
static esp_err_t cfg_set_alert_webhook_en(const char *v)
{
    long val;
    if (!parse_long(v, &val) || (val != 0 && val != 1)) return ESP_ERR_INVALID_ARG;
    return set_num_field(find_field("alert_webhook_enabled"), val);
}

/* 白名单表（get 仅非 secret 字段；set 含 secret 写入——只写不读，契约 §3）*/
static const at_cfg_field_t s_cfg_fields[] = {
    { "device_name",              AT_CFG_STR, false, cfg_get_device_name,        cfg_set_device_name },
    { "wifi_ssid",                AT_CFG_STR, false, cfg_get_wifi_ssid,          cfg_set_wifi_ssid },
    { "wifi_pass",                AT_CFG_STR, true,  NULL,                       cfg_set_wifi_pass },
    { "wifi_ssid_2",              AT_CFG_STR, false, cfg_get_wifi_ssid_2,        cfg_set_wifi_ssid_2 },
    { "wifi_pass_2",              AT_CFG_STR, true,  NULL,                       cfg_set_wifi_pass_2 },
    { "web_password",             AT_CFG_STR, true,  NULL,                       cfg_set_web_password },
    { "timezone",                 AT_CFG_STR, false, cfg_get_timezone,           cfg_set_timezone },
    { "cam_framesize",            AT_CFG_U8,  false, cfg_get_cam_framesize,      cfg_set_cam_framesize },
    { "cam_fps",                  AT_CFG_U8,  false, cfg_get_cam_fps,            cfg_set_cam_fps },
    { "cam_quality",              AT_CFG_U8,  false, cfg_get_cam_quality,        cfg_set_cam_quality },
    { "cam_vflip",                AT_CFG_U8,  false, cfg_get_cam_vflip,         cfg_set_cam_vflip },
    { "cam_hmirror",              AT_CFG_U8,  false, cfg_get_cam_hmirror,       cfg_set_cam_hmirror },
    { "xclk_freq_mhz",            AT_CFG_U8,  false, cfg_get_xclk_freq_mhz,     cfg_set_xclk },
    { "onvif_enable",             AT_CFG_U8,  false, cfg_get_onvif_enable,      cfg_set_onvif },
    { "ws_enable",                AT_CFG_U8,  false, cfg_get_ws_enable,         cfg_set_ws },
    { "motion_enabled",           AT_CFG_U8,  false, cfg_get_motion_enabled,    cfg_set_motion_en },
    { "motion_sensitivity",       AT_CFG_U8,  false, cfg_get_motion_sensitivity, cfg_set_motion_sens },
    { "motion_cooldown_s",        AT_CFG_U16, false, cfg_get_motion_cooldown_s, cfg_set_motion_cool },
    { "motion_active_interval_s", AT_CFG_U8,  false, cfg_get_motion_active_interval_s, cfg_set_motion_act },
    { "alert_webhook_enabled",    AT_CFG_U8,  false, cfg_get_alert_webhook_enabled, cfg_set_alert_webhook_en },
    { "alert_webhook_url",        AT_CFG_STR, false, cfg_get_alert_webhook_url, cfg_set_alert_webhook_url },
    { "webhook_secret",           AT_CFG_STR, true,  NULL,                       cfg_set_webhook_secret },
    { "server_url",               AT_CFG_STR, false, cfg_get_server_url,        cfg_set_server_url },
    { "mdns_hostname",            AT_CFG_STR, false, cfg_get_mdns_hostname,     cfg_set_mdns_hostname },
};

const at_cfg_field_t *at_port_cfg_fields(int *count)
{
    if (count) {
        *count = (int)(sizeof(s_cfg_fields) / sizeof(s_cfg_fields[0]));
    }
    return s_cfg_fields;
}

void at_port_save(void)
{
    /* 本仓写路径为事务式 copy+save（写即落盘）→ SAVE = 幂等再落盘当前值
     * （承袭旧 AT+SAVE 的 config_save 语义，契约 §2）*/
    cam_config_t cfg;
    config_get_copy(&cfg);
    config_save(&cfg);
}

/* ── 历史别名（契约 §4；入参/出参均不含 AT+ 前缀）───────────────── */

const char *at_port_alias(const char *name)
{
    if (strcasecmp(name, "CWJAP") == 0) return "WIFI";
    if (strcasecmp(name, "CWLAP") == 0) return "WIFISCAN";
    if (strcasecmp(name, "RST") == 0)   return "REBOOT";
    if (strcasecmp(name, "CIFSR") == 0) return "IP";   /* §2 AT+IP? 行登记的别名 */
    return NULL;
}

/* ── 板级扩展指令（契约 §4/§5 登记）────────────────────────────── */

/* 去首尾双引号（承袭旧 strip_quotes；AT 参数习惯带引号）*/
static char *strip_quotes(char *s)
{
    if (!s) return s;
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        s[len - 1] = '\0';
        return s + 1;
    }
    return s;
}

/* AT+STREAM? — MJPEG 流状态（契约 §5 本板登记扩展）*/
static esp_err_t ext_stream(const char *cmd)
{
    if (strchr(cmd, '=')) {
        ext_err("usage: AT+STREAM?");
        return ESP_OK;
    }
    ext_data("STREAM", "clients:%d", mjpeg_streamer_get_client_count());
    ext_data("STREAM", "max:1");   /* 硬单流（DRAM 限制，与 status.stream_clients_max 同步）*/
    ext_data("STREAM", "motion:%s", motion_detect_is_running() ? "running" : "stopped");
    ext_ok();
    return ESP_OK;
}

/* AT+HEAP — 堑水位（契约 §4：STATUS 分量保留独立可用）*/
static esp_err_t ext_heap(const char *cmd)
{
    (void)cmd;
    size_t current_free = 0, baseline_free = 0, baseline_min = 0;
    bool warning = false;
    health_check_threshold(&current_free, &warning);
    health_get_baselines(&baseline_free, &baseline_min);
    ext_data("HEAP", "free:%u", (unsigned)current_free);
    ext_data("HEAP", "min:%u", (unsigned)esp_get_minimum_free_heap_size());
    ext_data("HEAP", "baseline:%u", (unsigned)baseline_free);
    ext_ok();
    return ESP_OK;
}

/* AT+UPTIME — 运行时长 */
static esp_err_t ext_uptime(const char *cmd)
{
    (void)cmd;
    ext_data("UPTIME", "%u seconds",
             (unsigned)(esp_timer_get_time() / 1000000ULL));
    ext_ok();
    return ESP_OK;
}

/* AT+TEMP — 芯片温度 */
static esp_err_t ext_temp(const char *cmd)
{
    (void)cmd;
    ext_data("TEMP", "%.2f C", get_chip_temp());
    ext_ok();
    return ESP_OK;
}

/* AT+NAME? / AT+NAME= — 设备名（等价 CFGGET/SET=device_name，契约 §4）*/
static esp_err_t ext_name(const char *cmd)
{
    const char *eq = strchr(cmd, '=');
    if (!eq) {
        ext_data("NAME", "%s", config_get()->device_name);
        ext_ok();
        return ESP_OK;
    }
    char buf[64];
    strlcpy(buf, eq + 1, sizeof(buf));
    char *name = strip_quotes(buf);
    if (!name[0] || strlen(name) > 32) {
        ext_err("name must be 1-32 chars");
        return ESP_OK;
    }
    cam_config_t cfg;
    config_get_copy(&cfg);
    strlcpy(cfg.device_name, name, sizeof(cfg.device_name));
    if (config_save(&cfg) != ESP_OK) {
        ext_err("save failed");
        return ESP_OK;
    }
    ext_ok();
    return ESP_OK;
}

/* AT+CWQAP — 清凭据并切 AP 配网态（契约 §4：清凭据专用，保留）*/
static esp_err_t ext_cwqap(const char *cmd)
{
    (void)cmd;
    cam_config_t cfg;
    config_get_copy(&cfg);
    cfg.wifi_ssid[0] = '\0';
    cfg.wifi_pass[0] = '\0';
    esp_err_t ret = config_save(&cfg);
    if (ret != ESP_OK) {
        ext_err("save failed");
        return ESP_OK;
    }
    wifi_stop_retry();
    ret = wifi_start_ap();
    if (ret != ESP_OK) {
        ext_err("AP start failed (credentials cleared, reboot to enter AP)");
        return ESP_OK;
    }
    ext_data("CWQAP", "credentials cleared, AP mode (http://192.168.4.1)");
    ext_ok();
    return ESP_OK;
}

static const at_ext_cmd_t s_ext_cmds[] = {
    { "STREAM?", "MJPEG stream status (clients/max/motion)", ext_stream },
    { "HEAP",    "heap free/min/baseline",                   ext_heap },
    { "UPTIME",  "uptime in seconds",                        ext_uptime },
    { "TEMP",    "chip temperature",                         ext_temp },
    { "NAME",    "NAME? | NAME=name",                        ext_name },
    { "CWQAP",   "clear wifi credentials + AP mode",         ext_cwqap },
};

const at_ext_cmd_t *at_port_ext_cmds(int *count)
{
    if (count) {
        *count = (int)(sizeof(s_ext_cmds) / sizeof(s_ext_cmds[0]));
    }
    return s_ext_cmds;
}
