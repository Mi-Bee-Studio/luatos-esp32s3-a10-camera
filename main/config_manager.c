/*
 * 配置管理模块实现
 * 功能：管理设备配置的存储和加载，支持 v1→v2 自动迁移
 */

#include "config_manager.h"
#include "cJSON.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "config_manager";
static const char *NVS_NAMESPACE = "device_cfg";
static const char *NVS_CONFIG_KEY = "config";

// 旧版配置结构体 (v1) — 仅用于 NVS 迁移
typedef struct {
    char wifi_ssid[32];
    char wifi_pass[64];
    char server_url[128];
    char device_name[32];
    uint32_t magic;
    uint32_t version;
} device_config_t_v1;

// 全局配置实例
static cam_config_t s_config = {0};

// 默认配置
static const cam_config_t s_default_config = {
    .wifi_ssid = "",
    .wifi_pass = "",
    .server_url = "",
    .device_name = CONFIG_DEFAULT_DEVICE_NAME,
    .resolution = 0,         // VGA
    .fps = 15,
    .jpeg_quality = 12,
    .web_password = "",
    .timezone = CONFIG_DEFAULT_TIMEZONE,
    .motion_threshold = 5,
    .motion_cooldown = 10,
    .magic = CONFIG_MAGIC,
    .version = CONFIG_VERSION,
};

/**
 * @brief 设置配置为默认值
 */
static void config_set_defaults(cam_config_t *config)
{
    memcpy(config, &s_default_config, sizeof(cam_config_t));
}

/**
 * @brief 从 NVS 读取原始 blob 数据
 */
static esp_err_t config_read_blob(void *buf, size_t buf_size, size_t *out_size)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t size = buf_size;
    ret = nvs_get_blob(handle, NVS_CONFIG_KEY, buf, &size);
    if (ret == ESP_OK && out_size != NULL) {
        *out_size = size;
    }

    nvs_close(handle);
    return ret;
}

/**
 * @brief 尝试从 NVS 加载旧版 v1 配置并迁移到 v2
 */
static esp_err_t config_migrate_v1_to_v2(cam_config_t *config)
{
    device_config_t_v1 old_cfg;
    memset(&old_cfg, 0, sizeof(old_cfg));

    esp_err_t ret = config_read_blob(&old_cfg, sizeof(old_cfg), NULL);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "No v1 config found for migration");
        return ret;
    }

    // 验证 v1 配置有效性
    if (old_cfg.magic != CONFIG_MAGIC || old_cfg.version != 1) {
        ESP_LOGW(TAG, "v1 config has invalid magic/version, skipping migration");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // 迁移字段
    memset(config, 0, sizeof(cam_config_t));
    // v1 字符串没有 null terminator 保证，需要截断
    strncpy(config->wifi_ssid, old_cfg.wifi_ssid, sizeof(config->wifi_ssid) - 1);
    config->wifi_ssid[sizeof(config->wifi_ssid) - 1] = '\0';
    strncpy(config->wifi_pass, old_cfg.wifi_pass, sizeof(config->wifi_pass) - 1);
    config->wifi_pass[sizeof(config->wifi_pass) - 1] = '\0';
    strncpy(config->server_url, old_cfg.server_url, sizeof(config->server_url) - 1);
    config->server_url[sizeof(config->server_url) - 1] = '\0';
    strncpy(config->device_name, old_cfg.device_name, sizeof(config->device_name) - 1);
    config->device_name[sizeof(config->device_name) - 1] = '\0';

    // 新字段使用默认值
    config->resolution = s_default_config.resolution;
    config->fps = s_default_config.fps;
    config->jpeg_quality = s_default_config.jpeg_quality;
    strncpy(config->web_password, s_default_config.web_password, sizeof(config->web_password));
    strncpy(config->timezone, s_default_config.timezone, sizeof(config->timezone));
    config->motion_threshold = s_default_config.motion_threshold;
    config->motion_cooldown = s_default_config.motion_cooldown;

    // 更新版本标记
    config->magic = CONFIG_MAGIC;
    config->version = CONFIG_VERSION;

    ESP_LOGI(TAG, "Config migrated from v1 to v2 (ssid=%s, name=%s)",
             config->wifi_ssid, config->device_name);
    return ESP_OK;
}

/**
 * @brief 初始化配置管理系统
 *        初始化 NVS，加载或迁移配置（v1→v2 自动迁移）
 */
esp_err_t config_init(void)
{
    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS initialization failed, erasing...");
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

    // 尝试加载新版 v2 配置
    memset(&s_config, 0, sizeof(s_config));
    ret = config_read_blob(&s_config, sizeof(cam_config_t), NULL);

    if (ret == ESP_OK && s_config.magic == CONFIG_MAGIC && s_config.version == CONFIG_VERSION) {
        ESP_LOGI(TAG, "Config v2 loaded successfully");
        return ESP_OK;
    }

    // v2 加载失败，尝试从 v1 迁移
    ESP_LOGW(TAG, "v2 config not valid, attempting v1 migration...");
    ret = config_migrate_v1_to_v2(&s_config);
    if (ret == ESP_OK) {
        // 迁移成功，保存新格式到 NVS
        esp_err_t save_ret = config_save(&s_config);
        if (save_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save migrated config: %s", esp_err_to_name(save_ret));
        }
        return ESP_OK;
    }

    // v1 迁移也失败，使用默认值
    ESP_LOGW(TAG, "No valid config found, using defaults");
    config_set_defaults(&s_config);
    return ESP_OK;
}

/**
 * @brief 从 NVS 加载配置到提供的结构体
 */
esp_err_t config_load_from_nvs(cam_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return config_read_blob(config, sizeof(cam_config_t), NULL);
}

/**
 * @brief 获取当前配置
 */
const cam_config_t* config_get(void)
{
    return &s_config;
}

/**
 * @brief 保存配置到 NVS
 */
esp_err_t config_save(const cam_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_blob(handle, NVS_CONFIG_KEY, config, sizeof(cam_config_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write config: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit config: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    nvs_close(handle);

    // 更新全局配置
    memcpy(&s_config, config, sizeof(cam_config_t));

    ESP_LOGI(TAG, "Config saved successfully");
    return ESP_OK;
}

/**
 * @brief 重置配置为默认值并保存
 */
esp_err_t config_reset(void)
{
    config_set_defaults(&s_config);
    return config_save(&s_config);
}

/**
 * @brief 验证配置有效性
 */
bool config_is_valid(const cam_config_t *config)
{
    if (config == NULL) {
        return false;
    }

    // 检查魔数和版本
    if (config->magic != CONFIG_MAGIC || config->version != CONFIG_VERSION) {
        ESP_LOGW(TAG, "Invalid config: magic=0x%08x, version=%u",
                 (unsigned)config->magic, (unsigned)config->version);
        return false;
    }

    // 检查必要字段
    if (config->wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "Invalid config: WiFi SSID is empty");
        return false;
    }

    if (config->server_url[0] == '\0') {
        ESP_LOGW(TAG, "Invalid config: Server URL is empty");
        return false;
    }

    // 检查分辨率范围
    if (config->resolution > 3) {
        ESP_LOGW(TAG, "Invalid config: resolution=%u (must be 0-3)", config->resolution);
        return false;
    }

    // 检查 FPS 范围
    if (config->fps < 1 || config->fps > 30) {
        ESP_LOGW(TAG, "Invalid config: fps=%u (must be 1-30)", config->fps);
        return false;
    }

    // 检查 JPEG quality 范围
    if (config->jpeg_quality < 1 || config->jpeg_quality > 63) {
        ESP_LOGW(TAG, "Invalid config: jpeg_quality=%u (must be 1-63)", config->jpeg_quality);
        return false;
    }

    return true;
}

/**
 * @brief 更新并保存配置（便捷函数）
 */
esp_err_t config_set(const cam_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return config_save(config);
}

/**
 * @brief 获取时区字符串
 */
const char* config_get_timezone(void)
{
    return s_config.timezone;
}
