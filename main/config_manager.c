/*
 * 配置管理模块实现
 * 功能：管理设备配置的存储和加载，支持 v1→v2 自动迁移
 */

#include "config_manager.h"
#include "camera_driver.h"
#include "cJSON.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "config_manager";
static const char *OLD_NVS_NAMESPACE = "device_cfg";
static const char *NVS_NAMESPACE = "mibee_cfg";
static const char *NVS_CONFIG_KEY = "config";
static const char *NVS_MIGRATION_DONE_KEY = "migration_done";

// 旧版配置结构体 (v1) — 仅用于 NVS 迁移
typedef struct {
    char wifi_ssid[32];
    char wifi_pass[64];
    char server_url[128];
    char device_name[32];
    uint32_t magic;
    uint32_t version;
} device_config_t_v1;

// v2 配置结构体 — 仅用于 NVS 迁移 v2→v3
typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char server_url[129];
    char device_name[33];
    uint8_t resolution;
    uint8_t fps;
    uint8_t jpeg_quality;
    char web_password[33];
    char timezone[33];
    uint8_t motion_threshold;
    uint8_t motion_cooldown;
    uint32_t magic;
    uint32_t version;
} cam_config_t_v2;

// 全局配置实例
static cam_config_t s_config = {0};

// 配置访问互斥锁（保护多任务并发读写）
static SemaphoreHandle_t s_config_mutex = NULL;

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
    // v3 defaults
    .wifi_ssid2 = "",
    .wifi_pass2 = "",
    .mdns_hostname = "mibee",
    .webhook_url = "",
    .webhook_secret = "",
    .onvif_enabled = 0,
    .ws_enabled = 1,
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
 * @brief 检查命名空间迁移是否已完成
 * @return true 已完成迁移，false 需要迁移
 */
static bool config_is_migration_done(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return false;
    }
    uint8_t migration_done = 0;
    ret = nvs_get_u8(handle, NVS_MIGRATION_DONE_KEY, &migration_done);
    nvs_close(handle);
    return (ret == ESP_OK && migration_done == 1);
}

/**
 * @brief 将配置从旧命名空间迁移到新命名空间
 * @param config 输出参数：迁移后的配置
 * @return ESP_OK 成功迁移，其他值失败
 */
static esp_err_t config_migrate_namespace(cam_config_t *config)
{
    nvs_handle_t old_handle;
    nvs_handle_t new_handle;
    esp_err_t ret;
    size_t required_size = 0;
    cam_config_t temp_config;
    
    // 1. 打开旧命名空间
    ret = nvs_open(OLD_NVS_NAMESPACE, NVS_READWRITE, &old_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No old namespace found, skipping migration");
        return ret; // 没有旧配置，无需迁移
    }
    
    // 2. 读取旧配置大小
    ret = nvs_get_blob(old_handle, NVS_CONFIG_KEY, NULL, &required_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read old config size: %s", esp_err_to_name(ret));
        nvs_close(old_handle);
        return ret;
    }
    
    if (required_size != sizeof(cam_config_t)) {
        ESP_LOGE(TAG, "Old config size mismatch: expected %zu, got %zu",
                 sizeof(cam_config_t), required_size);
        nvs_close(old_handle);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // 3. 读取旧配置数据
    ret = nvs_get_blob(old_handle, NVS_CONFIG_KEY, &temp_config, &required_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read old config blob: %s", esp_err_to_name(ret));
        nvs_close(old_handle);
        return ret;
    }
    
    // 4. 验证旧配置
    if (temp_config.magic != CONFIG_MAGIC || temp_config.version != CONFIG_VERSION) {
        ESP_LOGW(TAG, "Old config has invalid magic/version, skipping migration");
        nvs_close(old_handle);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    nvs_close(old_handle);
    
    // 5. 打开新命名空间
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &new_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open new namespace: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 6. 写入新配置
    ret = nvs_set_blob(new_handle, NVS_CONFIG_KEY, &temp_config, sizeof(cam_config_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write config to new namespace: %s", esp_err_to_name(ret));
        nvs_close(new_handle);
        return ret;
    }
    
    // 7. 提交新配置
    ret = nvs_commit(new_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit new config: %s", esp_err_to_name(ret));
        nvs_close(new_handle);
        return ret;
    }
    
    // 8. 写入迁移完成标志
    ret = nvs_set_u8(new_handle, NVS_MIGRATION_DONE_KEY, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write migration flag: %s", esp_err_to_name(ret));
        nvs_close(new_handle);
        return ret;
    }
    
    // 9. 再次提交
    ret = nvs_commit(new_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit migration flag: %s", esp_err_to_name(ret));
        nvs_close(new_handle);
        return ret;
    }
    
    nvs_close(new_handle);
    
    // 10. 验证：重新读取新命名空间中的配置
    nvs_handle_t verify_handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &verify_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open new namespace for verification: %s", esp_err_to_name(ret));
        return ret;
    }
    
    cam_config_t verify_config;
    memset(&verify_config, 0, sizeof(verify_config));
    size_t verify_size = sizeof(verify_config);
    ret = nvs_get_blob(verify_handle, NVS_CONFIG_KEY, &verify_config, &verify_size);
    if (ret != ESP_OK || verify_size != sizeof(cam_config_t)) {
        ESP_LOGE(TAG, "Verification failed: could not read migrated config");
        nvs_close(verify_handle);
        return ESP_FAIL;
    }
    
    if (verify_config.magic != CONFIG_MAGIC || verify_config.version != CONFIG_VERSION) {
        ESP_LOGE(TAG, "Verification failed: migrated config has invalid magic/version");
        nvs_close(verify_handle);
        return ESP_FAIL;
    }
    
    nvs_close(verify_handle);
    
    // 11. 删除旧命名空间中的所有键
    ret = nvs_open(OLD_NVS_NAMESPACE, NVS_READWRITE, &old_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open old namespace for deletion, but migration is done");
        // 迁移成功，即使删除失败也不影响
        ESP_LOGI(TAG, "Namespace migration completed successfully (old keys not deleted)");
        memcpy(config, &temp_config, sizeof(cam_config_t));
        return ESP_OK;
    }
    
    ret = nvs_erase_key(old_handle, NVS_CONFIG_KEY);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to erase old config key: %s (but migration is done)", esp_err_to_name(ret));
    } else {
        ret = nvs_commit(old_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to commit old namespace deletion: %s (but migration is done)", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Old namespace keys deleted successfully");
        }
    }
    nvs_close(old_handle);
    
    ESP_LOGI(TAG, "Namespace migration completed successfully: %s -> %s",
             OLD_NVS_NAMESPACE, NVS_NAMESPACE);
    memcpy(config, &temp_config, sizeof(cam_config_t));
    return ESP_OK;
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
 * @brief 尝试从 NVS 加载旧版 v2 配置并迁移到 v3
 */
static esp_err_t config_migrate_v2_to_v3(cam_config_t *config)
{
    cam_config_t_v2 old_cfg;
    memset(&old_cfg, 0, sizeof(old_cfg));

    esp_err_t ret = config_read_blob(&old_cfg, sizeof(old_cfg), NULL);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "No v2 config found for migration");
        return ret;
    }

    // 验证 v2 配置有效性
    if (old_cfg.magic != CONFIG_MAGIC || old_cfg.version != 2) {
        ESP_LOGW(TAG, "v2 config has invalid magic/version, skipping migration");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // 迁移字段
    memset(config, 0, sizeof(cam_config_t));
    strncpy(config->wifi_ssid, old_cfg.wifi_ssid, sizeof(config->wifi_ssid) - 1);
    config->wifi_ssid[sizeof(config->wifi_ssid) - 1] = '\0';
    strncpy(config->wifi_pass, old_cfg.wifi_pass, sizeof(config->wifi_pass) - 1);
    config->wifi_pass[sizeof(config->wifi_pass) - 1] = '\0';
    strncpy(config->server_url, old_cfg.server_url, sizeof(config->server_url) - 1);
    config->server_url[sizeof(config->server_url) - 1] = '\0';
    strncpy(config->device_name, old_cfg.device_name, sizeof(config->device_name) - 1);
    config->device_name[sizeof(config->device_name) - 1] = '\0';
    config->resolution = old_cfg.resolution;
    config->fps = old_cfg.fps;
    config->jpeg_quality = old_cfg.jpeg_quality;
    strncpy(config->web_password, old_cfg.web_password, sizeof(config->web_password) - 1);
    config->web_password[sizeof(config->web_password) - 1] = '\0';
    strncpy(config->timezone, old_cfg.timezone, sizeof(config->timezone) - 1);
    config->timezone[sizeof(config->timezone) - 1] = '\0';
    config->motion_threshold = old_cfg.motion_threshold;
    config->motion_cooldown = old_cfg.motion_cooldown;

    // v3 新字段使用默认值
    config->wifi_ssid2[0] = '\0';
    config->wifi_pass2[0] = '\0';
    strncpy(config->mdns_hostname, "mibee", sizeof(config->mdns_hostname) - 1);
    config->mdns_hostname[sizeof(config->mdns_hostname) - 1] = '\0';
    config->webhook_url[0] = '\0';
    config->webhook_secret[0] = '\0';
    config->onvif_enabled = 0;
    config->ws_enabled = 1;

    // 更新版本标记
    config->magic = CONFIG_MAGIC;
    config->version = CONFIG_VERSION;

    ESP_LOGI(TAG, "Config migrated from v2 to v3 (ssid=%s, hostname=%s)",
             config->wifi_ssid, config->mdns_hostname);
    return ESP_OK;
}

/**
 * @brief 初始化配置管理系统
 *        初始化 NVS，加载或迁移配置（v1→v2 自动迁移）
 */


esp_err_t config_init(void)
{
    static bool s_config_initialized = false;
    if (s_config_initialized) {
        return ESP_OK;
    }

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

    /* 创建配置互斥锁 */
    s_config_mutex = xSemaphoreCreateMutex();
    if (s_config_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create config mutex");
        return ESP_ERR_NO_MEM;
    }

    // 首先检查命名空间迁移
    if (!config_is_migration_done()) {
        ESP_LOGW(TAG, "Namespace migration not done, attempting %s -> %s...",
                 OLD_NVS_NAMESPACE, NVS_NAMESPACE);
        ret = config_migrate_namespace(&s_config);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Namespace migration successful");
            // 迁移成功，保存配置到新命名空间
            ret = config_save(&s_config);
            if (ret == ESP_OK) {
                s_config_initialized = true;
            }
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save migrated config: %s", esp_err_to_name(ret));
            }
            return ret;
        } else if (ret != ESP_ERR_NOT_FOUND) {
            // 迁移失败但不是因为缺少旧配置，记录错误
            ESP_LOGW(TAG, "Namespace migration failed: %s", esp_err_to_name(ret));
            // 继续尝试其他迁移方法
        }
        // ESP_ERR_NOT_FOUND 表示没有旧命名空间，继续正常流程
    }
    
    // 尝试加载 v3 配置（从新命名空间）
    memset(&s_config, 0, sizeof(s_config));
    ret = config_read_blob(&s_config, sizeof(cam_config_t), NULL);
    
    if (ret == ESP_OK && s_config.magic == CONFIG_MAGIC && s_config.version == CONFIG_VERSION) {
        ESP_LOGI(TAG, "Config v3 loaded successfully from %s namespace", NVS_NAMESPACE);
        /* 旧固件可能存过超出当前板级边界的值（如 q<10 会撑爆 JPEG fb）——
         * 加载时钳制，保证运行值与 /api/camera 上报一致 */
        if (s_config.jpeg_quality < CAMERA_QUALITY_MIN) {
            ESP_LOGW(TAG, "Legacy jpeg_quality=%u clamped to %d on load",
                     s_config.jpeg_quality, CAMERA_QUALITY_MIN);
            s_config.jpeg_quality = CAMERA_QUALITY_MIN;
        } else if (s_config.jpeg_quality > CAMERA_QUALITY_MAX) {
            s_config.jpeg_quality = CAMERA_QUALITY_MAX;
        }
        if (s_config.resolution != 0) {
            ESP_LOGW(TAG, "Legacy resolution=%u clamped to VGA (DRAM constraint)",
                     s_config.resolution);
            s_config.resolution = 0;
        }
        s_config_initialized = true;
        return ESP_OK;
    }
    
    // v3 加载失败，尝试从 v2 迁移（从旧命名空间）
    if (ret == ESP_OK && s_config.magic == CONFIG_MAGIC && s_config.version == 2) {
        ESP_LOGW(TAG, "v2 config detected, attempting v2→v3 migration...");
        ret = config_migrate_v2_to_v3(&s_config);
        if (ret == ESP_OK) {
            esp_err_t save_ret = config_save(&s_config);
            if (save_ret == ESP_OK) {
                s_config_initialized = true;
            }
            if (save_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save v2→v3 migrated config: %s", esp_err_to_name(save_ret));
            }
            return (save_ret == ESP_OK) ? ESP_OK : save_ret;
        }
    }
    
    // v2 迁移也失败或不存在，尝试从 v1 迁移
    ESP_LOGW(TAG, "v3 config not valid, attempting v1 migration...");
    ret = config_migrate_v1_to_v2(&s_config);
    if (ret == ESP_OK) {
        // Set v3 defaults for new fields
        s_config.wifi_ssid2[0] = '\0';
        s_config.wifi_pass2[0] = '\0';
        strncpy(s_config.mdns_hostname, "mibee", sizeof(s_config.mdns_hostname) - 1);
        s_config.mdns_hostname[sizeof(s_config.mdns_hostname) - 1] = '\0';
        s_config.webhook_url[0] = '\0';
        s_config.webhook_secret[0] = '\0';
        s_config.onvif_enabled = 0;
        s_config.ws_enabled = 1;
        s_config.magic = CONFIG_MAGIC;
        s_config.version = CONFIG_VERSION;
    
        esp_err_t save_ret = config_save(&s_config);
        if (save_ret == ESP_OK) {
            s_config_initialized = true;
        }
        if (save_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save migrated config: %s", esp_err_to_name(save_ret));
        }
        return (save_ret == ESP_OK) ? ESP_OK : save_ret;
    }
    
    // 所有迁移都失败，使用默认值
    ESP_LOGW(TAG, "No valid config found, using defaults");
    config_set_defaults(&s_config);
    s_config_initialized = true;
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

    /* 更新全局配置（在互斥锁保护下） */
    if (s_config_mutex) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    }
    memcpy(&s_config, config, sizeof(cam_config_t));
    if (s_config_mutex) {
        xSemaphoreGive(s_config_mutex);
    }

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
    // v3 new field: mdns_hostname must not be empty
    if (config->mdns_hostname[0] == '\0' || strlen(config->mdns_hostname) >= 32) {
        ESP_LOGW(TAG, "Invalid config: mdns_hostname is empty or too long");
        return false;
    }

    // v3 new field: webhook_url length check if non-empty
    if (config->webhook_url[0] != '\0' && strlen(config->webhook_url) >= 128) {
        ESP_LOGW(TAG, "Invalid config: webhook_url too long");
        return false;
    }

    // v3 new field: onvif_enabled must be 0 or 1
    if (config->onvif_enabled != 0 && config->onvif_enabled != 1) {
        ESP_LOGW(TAG, "Invalid config: onvif_enabled=%u (must be 0 or 1)", config->onvif_enabled);
        return false;
    }

    // v3 new field: ws_enabled must be 0 or 1
    if (config->ws_enabled != 0 && config->ws_enabled != 1) {
        ESP_LOGW(TAG, "Invalid config: ws_enabled=%u (must be 0 or 1)", config->ws_enabled);
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

void config_get_copy(cam_config_t *out)
{
    if (out == NULL) return;
    if (s_config_mutex) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    }
    memcpy(out, &s_config, sizeof(cam_config_t));
    if (s_config_mutex) {
        xSemaphoreGive(s_config_mutex);
    }
}

/**
 * @brief 获取配置为 JSON 对象（cam_* 字段名前缀）
 * @return cJSON* 对象，调用者负责释放内存
 */
cJSON* config_get_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    
    cJSON_AddStringToObject(root, "cam_wifi_ssid", s_config.wifi_ssid);
    cJSON_AddStringToObject(root, "cam_wifi_pass", s_config.wifi_pass);
    cJSON_AddStringToObject(root, "cam_wifi_ssid2", s_config.wifi_ssid2);
    cJSON_AddStringToObject(root, "cam_wifi_pass2", s_config.wifi_pass2);
    cJSON_AddStringToObject(root, "cam_server_url", s_config.server_url);
    cJSON_AddStringToObject(root, "cam_device_name", s_config.device_name);
    cJSON_AddNumberToObject(root, "cam_framesize", s_config.resolution);
    cJSON_AddNumberToObject(root, "cam_fps", s_config.fps);
    cJSON_AddNumberToObject(root, "cam_quality", s_config.jpeg_quality);
    cJSON_AddStringToObject(root, "cam_web_password", s_config.web_password);
    cJSON_AddStringToObject(root, "cam_timezone", s_config.timezone);
    cJSON_AddNumberToObject(root, "cam_motion_threshold", s_config.motion_threshold);
    cJSON_AddNumberToObject(root, "cam_motion_cooldown", s_config.motion_cooldown);
    cJSON_AddStringToObject(root, "cam_mdns_hostname", s_config.mdns_hostname);
    cJSON_AddStringToObject(root, "cam_webhook_url", s_config.webhook_url);
    cJSON_AddStringToObject(root, "cam_webhook_secret", s_config.webhook_secret);
    cJSON_AddNumberToObject(root, "cam_onvif_enabled", s_config.onvif_enabled);
    cJSON_AddNumberToObject(root, "cam_ws_enabled", s_config.ws_enabled);
    
    return root;
}

/**
 * @brief 获取 Web UI 密码
 * @return Web UI 密码字符串指针（指向静态实例）
 */
const char* config_get_web_password(void)
{
    return s_config.web_password;
}
