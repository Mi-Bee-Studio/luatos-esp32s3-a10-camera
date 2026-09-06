/*
 * 配置管理模块实现 — 家族配置契约 v1.0（docs/config-contract.md）
 *
 * 持久化：mibee_cfg 命名空间逐键 NVS + schema_ver 版本键（2026-09-05 起，
 * 取代旧的整块 blob+magic 方案）。单键自治：读缺键=编译默认值；写单键失败
 * 只 WARN 不中断（PIT-022 教训）。所有 NVS 键 <=15 字符，构建期断言。
 *
 * 迁移：legacy blob（mibee_cfg/config v3，或更早 device_cfg/config v1/v2/v3）
 * 由 migrate_legacy_blob() 一次性翻译为逐键并落 schema_ver=1。梯子语义与
 * 旧 config_init 完全一致（v3 直读 / v2、v1 逐级补种），作为迁移输入整体
 * 保留；旧键改名 config_bak 留存只读（回滚备份）。迁移幂等：schema_ver
 * 已存在则跳过。
 */

#include "config_manager.h"
#include "sdkconfig.h"
#include "camera_driver.h"
#include "cJSON.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "config";

/* 契约 v1.1：家族统一默认管理密码（公开默认 mibeecam2026，本地可在 gitignored sdkconfig 覆盖） */
#define DEFAULT_WEB_PASSWORD CONFIG_MIBEE_CAM_DEFAULT_WEB_PASSWORD

#define NVS_NS          "mibee_cfg"      /* 家族统一命名空间（契约 §1）*/
#define NVS_NS_LEGACY   "device_cfg"     /* 前代命名空间（2026-09 前的 blob 所在地）*/
#define KEY_SCHEMA_VER  "schema_ver"
#define KEY_BLOB        "config"         /* legacy blob 键（迁移源）*/
#define KEY_LEGACY_BAK  "config_bak"     /* 迁移后的 legacy blob 备份键（只读回滚）*/
#define KEY_PW_SEED     "pw_seed_v1"     /* 契约 v1.1 密码一次性种子标记 */

static cam_config_t s_config = {0};
static bool s_config_initialized = false;
static SemaphoreHandle_t s_lock = NULL;

static void config_lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void config_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

/* ── 旧刻度 0-3 → 家族 framesize_t 刻度（api-contract §5 luatos 迁移行）── */
static uint8_t legacy_scale_to_framesize(uint8_t legacy)
{
    switch (legacy) {
        case 0:  return 10;  /* VGA */
        case 1:  return 11;  /* SVGA */
        case 2:  return 12;  /* XGA */
        case 3:  return 15;  /* UXGA */
        default: return 10;
    }
}

/* 板上限 VGA：家族刻度下 >10 一律钳回 10（api-contract §5 luatos 行；
 * SVGA 起 DRAM fb 枯竭螺旋，PIT-012）*/
static uint8_t clamp_to_board_cap(uint8_t framesize)
{
    if (framesize > (uint8_t)CAMERA_RES_BOARD_MAX) {
        return (uint8_t)CAMERA_RES_BOARD_MAX;
    }
    return framesize;
}

/* 本板出厂/缺键默认值（家族默认见契约 §3.1，板级差异见 §5）*/
static void apply_defaults(cam_config_t *cfg)
{
    memset(cfg, 0, sizeof(cam_config_t));
    strncpy(cfg->device_name, CONFIG_DEFAULT_DEVICE_NAME, sizeof(cfg->device_name) - 1);
    strncpy(cfg->web_password, DEFAULT_WEB_PASSWORD, sizeof(cfg->web_password) - 1);
    strncpy(cfg->timezone, CONFIG_DEFAULT_TIMEZONE, sizeof(cfg->timezone) - 1);
    cfg->cam_framesize = CAMERA_RES_VGA;               /* 10，板上限（§5）*/
    cfg->cam_fps = 15;
    cfg->cam_quality = 12;
    cfg->cam_vflip = 0;
    cfg->cam_hmirror = 0;
    cfg->xclk_freq_mhz = 20;                           /* 契约 §5 板值 */
    cfg->onvif_enable = 1;                             /* 家族默认（存量 0 由迁移保留）*/
    cfg->ws_enable = 1;
    /* motion 超集：默认灵敏度 95 = 旧 threshold 5（本板历史出厂值）*/
    cfg->motion_enabled = 1;
    cfg->motion_sensitivity = 95;
    cfg->motion_cooldown_s = 10;
    cfg->motion_active_interval_s = 5;
    cfg->alert_webhook_enabled = 0;
    strncpy(cfg->mdns_hostname, CONFIG_DEFAULT_MDNS_HOST, sizeof(cfg->mdns_hostname) - 1);
    /* wifi_ssid/pass、wifi_ssid_2/pass_2、alert_webhook_url、webhook_secret、
     * server_url 默认全空（memset 0 已就位）*/
}

/* ──────────────────────────────────────────────────────────────────
 * Legacy blob 布局（旧 config_manager.h 各版本的精确快照，仅迁移用，勿再改）
 * ────────────────────────────────────────────────────────────────── */
#define LEGACY_MAGIC 0xA5B6C7D8

/* v1（最早版本，字符串无 null 保证）*/
typedef struct {
    char wifi_ssid[32];
    char wifi_pass[64];
    char server_url[128];
    char device_name[32];
    uint32_t magic;
    uint32_t version;
} legacy_v1_t;

/* v2 */
typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char server_url[129];
    char device_name[33];
    uint8_t resolution;        /* 旧刻度 0-3 */
    uint8_t fps;
    uint8_t jpeg_quality;
    char web_password[33];
    char timezone[33];
    uint8_t motion_threshold;  /* 0-100，越小越灵 */
    uint8_t motion_cooldown;
    uint32_t magic;
    uint32_t version;
} legacy_v2_t;

/* v3（整块 blob 时代的最后版本，2026-09-05 per-key 迁移的直系输入）*/
typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char server_url[129];
    char device_name[33];
    uint8_t resolution;        /* 旧刻度 0-3 */
    uint8_t fps;
    uint8_t jpeg_quality;
    char web_password[33];
    char timezone[33];
    uint8_t motion_threshold;
    uint8_t motion_cooldown;
    char wifi_ssid2[33];
    char wifi_pass2[65];
    char mdns_hostname[33];
    char webhook_url[129];
    char webhook_secret[65];
    uint8_t onvif_enabled;
    uint8_t ws_enabled;
    uint32_t magic;
    uint32_t version;
} legacy_v3_t;

/* 安全拷贝旧字符串字段（旧数据不保证 null 结尾）*/
static void copy_legacy_str(char *dst, const char *src, size_t dstsz)
{
    strncpy(dst, src, dstsz - 1);
    dst[dstsz - 1] = '\0';
}

/* motion 模型收敛（契约 §6.1）：threshold(0-100 越小越灵) → sensitivity(越大越灵) */
static uint8_t threshold_to_sensitivity(uint8_t threshold)
{
    return (uint8_t)(100 - (threshold > 100 ? 100 : threshold));
}

/* v1 → 契约（v1 只有 4 个字符串字段，其余取本板默认，同旧梯子行为）*/
static void map_legacy_v1(const legacy_v1_t *lc, cam_config_t *cfg)
{
    apply_defaults(cfg);
    copy_legacy_str(cfg->wifi_ssid, lc->wifi_ssid, sizeof(cfg->wifi_ssid));
    copy_legacy_str(cfg->wifi_pass, lc->wifi_pass, sizeof(cfg->wifi_pass));
    copy_legacy_str(cfg->server_url, lc->server_url, sizeof(cfg->server_url));
    copy_legacy_str(cfg->device_name, lc->device_name, sizeof(cfg->device_name));
}

/* v2 → 契约（v3 组字段取默认，同旧梯子 v2→v3 补种行为）*/
static void map_legacy_v2(const legacy_v2_t *lc, cam_config_t *cfg)
{
    apply_defaults(cfg);
    copy_legacy_str(cfg->wifi_ssid, lc->wifi_ssid, sizeof(cfg->wifi_ssid));
    copy_legacy_str(cfg->wifi_pass, lc->wifi_pass, sizeof(cfg->wifi_pass));
    copy_legacy_str(cfg->server_url, lc->server_url, sizeof(cfg->server_url));
    copy_legacy_str(cfg->device_name, lc->device_name, sizeof(cfg->device_name));
    cfg->cam_framesize = clamp_to_board_cap(legacy_scale_to_framesize(lc->resolution));
    cfg->cam_fps = (lc->fps >= 1 && lc->fps <= 30) ? lc->fps : 15;
    if (lc->jpeg_quality < CAMERA_QUALITY_MIN) {
        cfg->cam_quality = CAMERA_QUALITY_MIN;
    } else if (lc->jpeg_quality > CAMERA_QUALITY_MAX) {
        cfg->cam_quality = CAMERA_QUALITY_MAX;
    } else {
        cfg->cam_quality = lc->jpeg_quality;
    }
    copy_legacy_str(cfg->web_password, lc->web_password, sizeof(cfg->web_password));
    copy_legacy_str(cfg->timezone, lc->timezone, sizeof(cfg->timezone));
    cfg->motion_sensitivity = threshold_to_sensitivity(lc->motion_threshold);
    cfg->motion_cooldown_s = (lc->motion_cooldown >= 1)   /* u8 <=255 天然 <300 */
        ? lc->motion_cooldown : 10;
}

/* v3 → 契约（改名 + 刻度翻译 + motion/webhook 模型收敛；存储值一律保留）*/
static void map_legacy_v3(const legacy_v3_t *lc, cam_config_t *cfg)
{
    apply_defaults(cfg);
    copy_legacy_str(cfg->wifi_ssid, lc->wifi_ssid, sizeof(cfg->wifi_ssid));
    copy_legacy_str(cfg->wifi_pass, lc->wifi_pass, sizeof(cfg->wifi_pass));
    copy_legacy_str(cfg->server_url, lc->server_url, sizeof(cfg->server_url));
    copy_legacy_str(cfg->device_name, lc->device_name, sizeof(cfg->device_name));
    cfg->cam_framesize = clamp_to_board_cap(legacy_scale_to_framesize(lc->resolution));
    cfg->cam_fps = (lc->fps >= 1 && lc->fps <= 30) ? lc->fps : 15;
    if (lc->jpeg_quality < CAMERA_QUALITY_MIN) {
        cfg->cam_quality = CAMERA_QUALITY_MIN;
    } else if (lc->jpeg_quality > CAMERA_QUALITY_MAX) {
        cfg->cam_quality = CAMERA_QUALITY_MAX;
    } else {
        cfg->cam_quality = lc->jpeg_quality;
    }
    copy_legacy_str(cfg->web_password, lc->web_password, sizeof(cfg->web_password));
    copy_legacy_str(cfg->timezone, lc->timezone, sizeof(cfg->timezone));
    cfg->motion_sensitivity = threshold_to_sensitivity(lc->motion_threshold);
    cfg->motion_cooldown_s = (lc->motion_cooldown >= 1)   /* u8 <=255 天然 <300 */
        ? lc->motion_cooldown : 10;
    copy_legacy_str(cfg->wifi_ssid_2, lc->wifi_ssid2, sizeof(cfg->wifi_ssid_2));
    copy_legacy_str(cfg->wifi_pass_2, lc->wifi_pass2, sizeof(cfg->wifi_pass_2));
    copy_legacy_str(cfg->mdns_hostname, lc->mdns_hostname, sizeof(cfg->mdns_hostname));
    if (cfg->mdns_hostname[0] == '\0') {
        strncpy(cfg->mdns_hostname, CONFIG_DEFAULT_MDNS_HOST, sizeof(cfg->mdns_hostname) - 1);
    }
    copy_legacy_str(cfg->alert_webhook_url, lc->webhook_url, sizeof(cfg->alert_webhook_url));
    copy_legacy_str(cfg->webhook_secret, lc->webhook_secret, sizeof(cfg->webhook_secret));
    /* 契约 §6.1 webhook：旧 url+secret 无开关 → url 非空补种 enabled=1 */
    cfg->alert_webhook_enabled = (lc->webhook_url[0] != '\0') ? 1 : 0;
    cfg->onvif_enable = lc->onvif_enabled ? 1 : 0;   /* 存量 0 保持 0 */
    cfg->ws_enable = lc->ws_enabled ? 1 : 0;
}

/* ──────────────────────────────────────────────────────────────────
 * 逐键 NVS 读写（契约 §1）
 * ────────────────────────────────────────────────────────────────── */
#define KEY_ASSERT(k) _Static_assert(sizeof(k) <= 16, "NVS key >15 chars: " k)
KEY_ASSERT("schema_ver");
KEY_ASSERT("device_name");
KEY_ASSERT("wifi_ssid");
KEY_ASSERT("wifi_pass");
KEY_ASSERT("wifi_ssid_2");
KEY_ASSERT("wifi_pass_2");
KEY_ASSERT("timezone");
KEY_ASSERT("web_password");
KEY_ASSERT("cam_framesize");
KEY_ASSERT("cam_fps");
KEY_ASSERT("cam_quality");
KEY_ASSERT("cam_vflip");
KEY_ASSERT("cam_hmirror");
KEY_ASSERT("xclk_freq_mhz");
KEY_ASSERT("onvif_enable");
KEY_ASSERT("motion_enabled");
KEY_ASSERT("motion_sens");
KEY_ASSERT("motion_cool_s");
/* 注：契约 §3.2 括注的 motion_act_int_s 有 16 字符，超 NVS 15 字符硬限，
 * 取参考实现（ai-thinker a12489b）的 motion_act_s */
KEY_ASSERT("motion_act_s");
KEY_ASSERT("webhook_en");
KEY_ASSERT("webhook_url");
KEY_ASSERT("webhook_secret");
KEY_ASSERT("pw_seed_v1");
KEY_ASSERT("config_bak");

/* 字符串键读取：缺键/读失败 → 保持默认（返回 false）*/
static bool rd_str(nvs_handle_t h, const char *key, char *out, size_t outsz)
{
    size_t len = outsz;
    return nvs_get_str(h, key, out, &len) == ESP_OK;
}
static bool rd_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    return nvs_get_u8(h, key, out) == ESP_OK;
}
static bool rd_u16(nvs_handle_t h, const char *key, uint16_t *out)
{
    return nvs_get_u16(h, key, out) == ESP_OK;
}

/* 写入辅助：单键失败只 WARN（PIT-022——不得中止整批写入）*/
static void wr_str(nvs_handle_t h, const char *key, const char *val)
{
    esp_err_t ret = nvs_set_str(h, key, val);
    if (ret != ESP_OK) ESP_LOGW(TAG, "nvs_set_str(%s) failed: %s", key, esp_err_to_name(ret));
}
static void wr_u8(nvs_handle_t h, const char *key, uint8_t val)
{
    esp_err_t ret = nvs_set_u8(h, key, val);
    if (ret != ESP_OK) ESP_LOGW(TAG, "nvs_set_u8(%s) failed: %s", key, esp_err_to_name(ret));
}
static void wr_u16(nvs_handle_t h, const char *key, uint16_t val)
{
    esp_err_t ret = nvs_set_u16(h, key, val);
    if (ret != ESP_OK) ESP_LOGW(TAG, "nvs_set_u16(%s) failed: %s", key, esp_err_to_name(ret));
}

static void load_keys_from_nvs(nvs_handle_t h, cam_config_t *cfg)
{
    /* 缺键 = apply_defaults 已就位的默认值（契约 §1 单键自治）*/
    rd_str(h, "device_name",   cfg->device_name,   sizeof(cfg->device_name));
    rd_str(h, "wifi_ssid",     cfg->wifi_ssid,     sizeof(cfg->wifi_ssid));
    rd_str(h, "wifi_pass",     cfg->wifi_pass,     sizeof(cfg->wifi_pass));
    rd_str(h, "wifi_ssid_2",   cfg->wifi_ssid_2,   sizeof(cfg->wifi_ssid_2));
    rd_str(h, "wifi_pass_2",   cfg->wifi_pass_2,   sizeof(cfg->wifi_pass_2));
    rd_str(h, "timezone",      cfg->timezone,      sizeof(cfg->timezone));
    rd_str(h, "web_password",  cfg->web_password,  sizeof(cfg->web_password));
    rd_u8(h, "cam_framesize", &cfg->cam_framesize);
    rd_u8(h, "cam_fps",       &cfg->cam_fps);
    rd_u8(h, "cam_quality",   &cfg->cam_quality);
    rd_u8(h, "cam_vflip",     &cfg->cam_vflip);
    rd_u8(h, "cam_hmirror",   &cfg->cam_hmirror);
    rd_u8(h, "xclk_freq_mhz", &cfg->xclk_freq_mhz);
    rd_u8(h, "onvif_enable",  &cfg->onvif_enable);
    rd_u8(h, "motion_enabled", &cfg->motion_enabled);
    rd_u8(h, "motion_sens",   &cfg->motion_sensitivity);
    rd_u16(h, "motion_cool_s", &cfg->motion_cooldown_s);
    rd_u8(h, "motion_act_s", &cfg->motion_active_interval_s);
    rd_u8(h, "webhook_en",    &cfg->alert_webhook_enabled);
    rd_str(h, "webhook_url",  cfg->alert_webhook_url, sizeof(cfg->alert_webhook_url));
    rd_str(h, "webhook_secret", cfg->webhook_secret, sizeof(cfg->webhook_secret));
    rd_u8(h, "ws_enable",     &cfg->ws_enable);
    rd_str(h, "server_url",   cfg->server_url,     sizeof(cfg->server_url));
    rd_str(h, "mdns_hostname", cfg->mdns_hostname, sizeof(cfg->mdns_hostname));
}

static void write_keys_to_nvs(nvs_handle_t h, const cam_config_t *cfg)
{
    wr_str(h, "device_name",  cfg->device_name);
    wr_str(h, "wifi_ssid",    cfg->wifi_ssid);
    wr_str(h, "wifi_pass",    cfg->wifi_pass);
    wr_str(h, "wifi_ssid_2",  cfg->wifi_ssid_2);
    wr_str(h, "wifi_pass_2",  cfg->wifi_pass_2);
    wr_str(h, "timezone",     cfg->timezone);
    wr_str(h, "web_password", cfg->web_password);
    wr_u8(h, "cam_framesize", cfg->cam_framesize);
    wr_u8(h, "cam_fps",      cfg->cam_fps);
    wr_u8(h, "cam_quality",  cfg->cam_quality);
    wr_u8(h, "cam_vflip",    cfg->cam_vflip);
    wr_u8(h, "cam_hmirror",  cfg->cam_hmirror);
    wr_u8(h, "xclk_freq_mhz", cfg->xclk_freq_mhz);
    wr_u8(h, "onvif_enable", cfg->onvif_enable);
    wr_u8(h, "motion_enabled", cfg->motion_enabled);
    wr_u8(h, "motion_sens",  cfg->motion_sensitivity);
    wr_u16(h, "motion_cool_s", cfg->motion_cooldown_s);
    wr_u8(h, "motion_act_s", cfg->motion_active_interval_s);
    wr_u8(h, "webhook_en",   cfg->alert_webhook_enabled);
    wr_str(h, "webhook_url", cfg->alert_webhook_url);
    wr_str(h, "webhook_secret", cfg->webhook_secret);
    wr_u8(h, "ws_enable",    cfg->ws_enable);
    wr_str(h, "server_url",  cfg->server_url);
    wr_str(h, "mdns_hostname", cfg->mdns_hostname);
    wr_u16(h, KEY_SCHEMA_VER, CONFIG_SCHEMA_VERSION);
}

/* ── pw_seed_v1 标记查询：新旧两个命名空间都查（已种过的设备不得重种）── */
static bool pw_seed_marker_present(void)
{
    static const char *namespaces[] = { NVS_NS, NVS_NS_LEGACY };
    for (size_t i = 0; i < sizeof(namespaces) / sizeof(namespaces[0]); i++) {
        nvs_handle_t h;
        uint8_t flag = 0;
        if (nvs_open(namespaces[i], NVS_READONLY, &h) == ESP_OK) {
            bool have = (nvs_get_u8(h, KEY_PW_SEED, &flag) == ESP_OK && flag == 1);
            nvs_close(h);
            if (have) {
                return true;
            }
        }
    }
    return false;
}

/* ── Legacy blob → 逐键一次性迁移（幂等：schema_ver 存在即跳过）──
 * 梯子语义照搬旧 config_init：v3 直读 / v2、v1 逐级补种；blob 优先取
 * mibee_cfg/config，不存在再查前代 device_cfg/config。 */
static bool migrate_legacy_blob(void)
{
    nvs_handle_t h;
    uint16_t schema_ver = 0;

    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        bool have = nvs_get_u16(h, KEY_SCHEMA_VER, &schema_ver) == ESP_OK;
        nvs_close(h);
        if (have && schema_ver >= CONFIG_SCHEMA_VERSION) {
            return false;   /* 已迁移或本就逐键 */
        }
    }

    /* 读 legacy blob（原文字节，mibee_cfg 优先，device_cfg 兜底）*/
    const char *blob_ns = NULL;
    uint8_t *raw = NULL;
    size_t blob_len = 0;
    static const char *candidates[] = { NVS_NS, NVS_NS_LEGACY };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        nvs_handle_t h_old;
        if (nvs_open(candidates[i], NVS_READONLY, &h_old) != ESP_OK) {
            continue;
        }
        size_t len = 0;
        if (nvs_get_blob(h_old, KEY_BLOB, NULL, &len) == ESP_OK && len >= 8 && len <= 4096) {
            raw = malloc(len);
            if (raw && nvs_get_blob(h_old, KEY_BLOB, raw, &len) == ESP_OK) {
                blob_len = len;
                blob_ns = candidates[i];
                nvs_close(h_old);
                break;
            }
            free(raw);
            raw = NULL;
        }
        nvs_close(h_old);
    }
    if (blob_ns == NULL) {
        return false;   /* 无 legacy blob（真空出厂或已迁移）*/
    }

    /* 按 magic/version/size 识别版本并翻译（同旧梯子的识别规则）*/
    cam_config_t migrated;
    bool mapped = false;
    uint32_t magic = 0, version = 0;
    /* 尾部 magic+version 对齐读（各版本布局末尾一致）*/
    memcpy(&magic, raw + blob_len - 8, 4);
    memcpy(&version, raw + blob_len - 4, 4);

    if (magic == LEGACY_MAGIC && version == 3 && blob_len == sizeof(legacy_v3_t)) {
        legacy_v3_t lc;
        memcpy(&lc, raw, sizeof(lc));
        map_legacy_v3(&lc, &migrated);
        mapped = true;
        ESP_LOGI(TAG, "Legacy v3 blob detected (%u bytes)", (unsigned)blob_len);
    } else if (magic == LEGACY_MAGIC && version == 2 && blob_len == sizeof(legacy_v2_t)) {
        legacy_v2_t lc;
        memcpy(&lc, raw, sizeof(lc));
        map_legacy_v2(&lc, &migrated);
        mapped = true;
        ESP_LOGI(TAG, "Legacy v2 blob detected (%u bytes)", (unsigned)blob_len);
    } else if (magic == LEGACY_MAGIC && version == 1 && blob_len == sizeof(legacy_v1_t)) {
        legacy_v1_t lc;
        memcpy(&lc, raw, sizeof(lc));
        map_legacy_v1(&lc, &migrated);
        mapped = true;
        ESP_LOGI(TAG, "Legacy v1 blob detected (%u bytes)", (unsigned)blob_len);
    }

    if (!mapped) {
        ESP_LOGW(TAG, "Legacy blob unrecognized (magic=0x%08x ver=%u len=%u), skipping",
                 (unsigned)magic, (unsigned)version, (unsigned)blob_len);
        free(raw);
        return false;
    }

    /* 密码一次性种子（契约 v1.1）：标记缺失 = 旧固件时代设备可能带未知历史
     * 密码 → 统一种为家族默认；已种过则保留 blob 中的密码 */
    bool pw_seeded = pw_seed_marker_present();
    if (!pw_seeded) {
        ESP_LOGW(TAG, "One-shot password seed: unifying web_password to family default");
        strncpy(migrated.web_password, DEFAULT_WEB_PASSWORD, sizeof(migrated.web_password) - 1);
        migrated.web_password[sizeof(migrated.web_password) - 1] = '\0';
    }
    if (migrated.web_password[0] == '\0') {
        strncpy(migrated.web_password, DEFAULT_WEB_PASSWORD, sizeof(migrated.web_password) - 1);
    }

    /* 翻译结果落逐键 */
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open %s for migration write", NVS_NS);
        free(raw);
        return false;
    }
    write_keys_to_nvs(h, &migrated);
    if (!pw_seeded) {
        nvs_set_u8(h, KEY_PW_SEED, 1);
    }
    esp_err_t ret = nvs_commit(h);
    nvs_close(h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Migration commit failed: %s", esp_err_to_name(ret));
        free(raw);
        return false;
    }

    /* legacy 键改名备份：原文写 config_bak，删 config（留存只读回滚）*/
    {
        nvs_handle_t h_old;
        if (nvs_open(blob_ns, NVS_READWRITE, &h_old) == ESP_OK) {
            if (nvs_set_blob(h_old, KEY_LEGACY_BAK, raw, blob_len) == ESP_OK) {
                nvs_erase_key(h_old, KEY_BLOB);
                nvs_commit(h_old);
            } else {
                ESP_LOGW(TAG, "config_bak backup failed, legacy blob left in place");
            }
            nvs_close(h_old);
        }
    }
    free(raw);

    ESP_LOGI(TAG, "Legacy blob (%s/%s, %u bytes) migrated to per-key NVS (schema_ver=%d)",
             blob_ns, KEY_BLOB, (unsigned)blob_len, CONFIG_SCHEMA_VERSION);
    return true;
}

/* ──────────────────────────────────────────────────────────────────
 * Public API
 * ────────────────────────────────────────────────────────────────── */

esp_err_t config_init(void)
{
    if (s_config_initialized) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "Failed to create config mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    /* 初始化 NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, erasing...");
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 一次性迁移（幂等）*/
    migrate_legacy_blob();

    /* 加载逐键配置（缺键 = 默认值）*/
    apply_defaults(&s_config);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        load_keys_from_nvs(h, &s_config);
        nvs_close(h);
    } else {
        /* 首次出厂：落一份默认值（含 schema_ver）*/
        ESP_LOGW(TAG, "No per-key config yet, saving defaults");
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            write_keys_to_nvs(h, &s_config);
            nvs_commit(h);
            nvs_close(h);
        }
    }

    /* 板级边界钳制（契约 §4）：旧固件可能存过越界值（如 q<10 撑爆 JPEG fb，
     * PIT-021；fps 此前无校验），加载时钳回，保证运行值与上报一致 */
    if (s_config.cam_quality < CAMERA_QUALITY_MIN) {
        ESP_LOGW(TAG, "cam_quality=%u clamped to %d on load",
                 s_config.cam_quality, CAMERA_QUALITY_MIN);
        s_config.cam_quality = CAMERA_QUALITY_MIN;
    } else if (s_config.cam_quality > CAMERA_QUALITY_MAX) {
        s_config.cam_quality = CAMERA_QUALITY_MAX;
    }
    if (s_config.cam_framesize > (uint8_t)FRAMESIZE_UXGA) {
        ESP_LOGW(TAG, "cam_framesize=%u out of family scale, defaulting to VGA",
                 s_config.cam_framesize);
        s_config.cam_framesize = CAMERA_RES_VGA;
    }
    if (s_config.cam_framesize > (uint8_t)CAMERA_RES_BOARD_MAX) {
        ESP_LOGW(TAG, "cam_framesize=%u clamped to VGA (board cap, DRAM constraint)",
                 s_config.cam_framesize);
        s_config.cam_framesize = CAMERA_RES_BOARD_MAX;
    }
    if (s_config.cam_fps < 1 || s_config.cam_fps > 30) {
        ESP_LOGW(TAG, "cam_fps=%u out of range 1-30, defaulting to 15", s_config.cam_fps);
        s_config.cam_fps = 15;
    }
    if (s_config.motion_sensitivity > 100) {
        s_config.motion_sensitivity = 100;
    }
    if (s_config.motion_cooldown_s < 1 || s_config.motion_cooldown_s > 300) {
        s_config.motion_cooldown_s = 10;
    }
    if (s_config.motion_active_interval_s < 1 || s_config.motion_active_interval_s > 30) {
        s_config.motion_active_interval_s = 5;
    }
    if (s_config.xclk_freq_mhz != 10 && s_config.xclk_freq_mhz != 16 && s_config.xclk_freq_mhz != 20) {
        s_config.xclk_freq_mhz = 20;
    }
    s_config.onvif_enable = s_config.onvif_enable ? 1 : 0;
    s_config.ws_enable = s_config.ws_enable ? 1 : 0;
    s_config.motion_enabled = s_config.motion_enabled ? 1 : 0;
    s_config.alert_webhook_enabled = s_config.alert_webhook_enabled ? 1 : 0;
    /* 契约 v1.1：空密码迁移到家族统一默认 */
    if (s_config.web_password[0] == '\0') {
        strncpy(s_config.web_password, DEFAULT_WEB_PASSWORD, sizeof(s_config.web_password) - 1);
    }
    if (s_config.mdns_hostname[0] == '\0') {
        strncpy(s_config.mdns_hostname, CONFIG_DEFAULT_MDNS_HOST,
                sizeof(s_config.mdns_hostname) - 1);
    }

    ESP_LOGI(TAG, "Config loaded (device=%s, wifi_ssid='%s', schema v%d)",
             s_config.device_name, s_config.wifi_ssid, CONFIG_SCHEMA_VERSION);
    s_config_initialized = true;
    return ESP_OK;
}

const cam_config_t *config_get(void)
{
    return &s_config;
}

void config_get_copy(cam_config_t *out)
{
    if (out == NULL) return;
    config_lock();
    *out = s_config;
    config_unlock();
}

esp_err_t config_save(const cam_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 更新全局配置（互斥锁保护），再逐键落盘（单键失败只 WARN，PIT-022）*/
    config_lock();
    s_config = *config;
    write_keys_to_nvs(handle, &s_config);
    config_unlock();

    ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit config: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Config saved: wifi_ssid='%s' device=%s", s_config.wifi_ssid, s_config.device_name);
    return ESP_OK;
}

esp_err_t config_reset(void)
{
    ESP_LOGW(TAG, "Resetting config to defaults");
    cam_config_t def;
    apply_defaults(&def);
    return config_save(&def);
}

bool config_is_valid(const cam_config_t *config)
{
    if (config == NULL) {
        return false;
    }

    /* server_url 为可选板级扩展（契约 §7——旧版强制非空是 bug，已修）；
     * wifi_ssid 允许为空（首次出厂 = AP 配网模式）。 */

    if (config->cam_framesize < (uint8_t)CAMERA_RES_VGA ||
        config->cam_framesize > (uint8_t)CAMERA_RES_BOARD_MAX) {
        ESP_LOGW(TAG, "Invalid config: cam_framesize=%u (board allows VGA only)",
                 config->cam_framesize);
        return false;
    }
    if (config->cam_fps < 1 || config->cam_fps > 30) {
        ESP_LOGW(TAG, "Invalid config: cam_fps=%u (must be 1-30)", config->cam_fps);
        return false;
    }
    if (config->cam_quality < CAMERA_QUALITY_MIN || config->cam_quality > CAMERA_QUALITY_MAX) {
        ESP_LOGW(TAG, "Invalid config: cam_quality=%u (must be %d-%d)",
                 config->cam_quality, CAMERA_QUALITY_MIN, CAMERA_QUALITY_MAX);
        return false;
    }
    if (config->xclk_freq_mhz != 10 && config->xclk_freq_mhz != 16 && config->xclk_freq_mhz != 20) {
        ESP_LOGW(TAG, "Invalid config: xclk_freq_mhz=%u (must be 10/16/20)",
                 config->xclk_freq_mhz);
        return false;
    }
    if (config->onvif_enable > 1 || config->ws_enable > 1 ||
        config->motion_enabled > 1 || config->cam_vflip > 1 || config->cam_hmirror > 1 ||
        config->alert_webhook_enabled > 1) {
        ESP_LOGW(TAG, "Invalid config: boolean field not 0/1");
        return false;
    }
    if (config->motion_sensitivity > 100) {
        ESP_LOGW(TAG, "Invalid config: motion_sensitivity=%u (must be 0-100)",
                 config->motion_sensitivity);
        return false;
    }
    if (config->motion_cooldown_s < 1 || config->motion_cooldown_s > 300) {
        ESP_LOGW(TAG, "Invalid config: motion_cooldown_s=%u (must be 1-300)",
                 (unsigned)config->motion_cooldown_s);
        return false;
    }
    if (config->motion_active_interval_s < 1 || config->motion_active_interval_s > 30) {
        ESP_LOGW(TAG, "Invalid config: motion_active_interval_s=%u (must be 1-30)",
                 config->motion_active_interval_s);
        return false;
    }
    if (config->mdns_hostname[0] == '\0' || strlen(config->mdns_hostname) >= 32) {
        ESP_LOGW(TAG, "Invalid config: mdns_hostname is empty or too long");
        return false;
    }
    if (config->web_password[0] != '\0' && strlen(config->web_password) < 6) {
        ESP_LOGW(TAG, "Invalid config: web_password shorter than 6 chars");
        return false;
    }
    if (config->alert_webhook_url[0] != '\0' && strlen(config->alert_webhook_url) >= 256) {
        ESP_LOGW(TAG, "Invalid config: alert_webhook_url too long");
        return false;
    }
    return true;
}

esp_err_t config_set(const cam_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return config_save(config);
}

const char *config_get_timezone(void)
{
    return s_config.timezone;
}

cJSON *config_get_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    config_lock();
    cam_config_t cfg = s_config;
    config_unlock();

    /* 契约字段名（§3）；敏感字段掩码（§3 敏感清单）*/
    cJSON_AddStringToObject(root, "device_name", cfg.device_name);
    cJSON_AddStringToObject(root, "wifi_ssid", cfg.wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_pass", cfg.wifi_pass[0] ? "****" : "");
    cJSON_AddStringToObject(root, "wifi_ssid_2", cfg.wifi_ssid_2);
    cJSON_AddStringToObject(root, "wifi_pass_2", cfg.wifi_pass_2[0] ? "****" : "");
    cJSON_AddStringToObject(root, "timezone", cfg.timezone);
    cJSON_AddStringToObject(root, "web_password", cfg.web_password[0] ? "****" : "");
    cJSON_AddNumberToObject(root, "cam_framesize", (double)cfg.cam_framesize);
    cJSON_AddNumberToObject(root, "cam_fps", (double)cfg.cam_fps);
    cJSON_AddNumberToObject(root, "cam_quality", (double)cfg.cam_quality);
    cJSON_AddNumberToObject(root, "cam_vflip", (double)cfg.cam_vflip);
    cJSON_AddNumberToObject(root, "cam_hmirror", (double)cfg.cam_hmirror);
    cJSON_AddNumberToObject(root, "xclk_freq_mhz", (double)cfg.xclk_freq_mhz);
    cJSON_AddNumberToObject(root, "onvif_enable", (double)cfg.onvif_enable);
    cJSON_AddNumberToObject(root, "motion_enabled", (double)cfg.motion_enabled);
    cJSON_AddNumberToObject(root, "motion_sensitivity", (double)cfg.motion_sensitivity);
    cJSON_AddNumberToObject(root, "motion_cooldown_s", (double)cfg.motion_cooldown_s);
    cJSON_AddNumberToObject(root, "motion_active_interval_s", (double)cfg.motion_active_interval_s);
    cJSON_AddNumberToObject(root, "alert_webhook_enabled", (double)cfg.alert_webhook_enabled);
    cJSON_AddStringToObject(root, "alert_webhook_url", cfg.alert_webhook_url);
    cJSON_AddStringToObject(root, "webhook_secret", cfg.webhook_secret[0] ? "****" : "");
    cJSON_AddNumberToObject(root, "ws_enable", (double)cfg.ws_enable);
    cJSON_AddStringToObject(root, "server_url", cfg.server_url);
    cJSON_AddStringToObject(root, "mdns_hostname", cfg.mdns_hostname);
    cJSON_AddNumberToObject(root, "schema_version", (double)CONFIG_SCHEMA_VERSION);

    return root;
}

const char *config_get_web_password(void)
{
    return s_config.web_password;
}
