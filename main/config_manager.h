/*
 * 配置管理模块头文件
 * 功能：管理设备配置的存储和加载
 * 版本：v2 — cam_config_t，支持自动迁移 v1→v2
 */

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include "esp_err.h"
#include <stdbool.h>

// 配置版本号
#define CONFIG_VERSION 2

// 配置魔数（用于验证配置有效性）
#define CONFIG_MAGIC 0xA5B6C7D8

// 字符串字段最大长度（用于验证）
#define CONFIG_MAX_STRING_LEN 129

// 默认值
#define CONFIG_DEFAULT_TIMEZONE "CST-8"
#define CONFIG_DEFAULT_DEVICE_NAME "MiBeeCam"

// 新版配置结构体 (v2)
typedef struct {
    char wifi_ssid[33];        // WiFi SSID (32 chars + null)
    char wifi_pass[65];        // WiFi password (64 chars + null)
    char server_url[129];      // Upload server URL (128 chars + null)
    char device_name[33];      // Device name (32 chars + null)
    uint8_t resolution;        // Camera resolution (0=VGA, 1=SVGA, 2=XGA, 3=UXGA)
    uint8_t fps;               // Camera FPS (1-30)
    uint8_t jpeg_quality;      // JPEG quality (1-63, lower=better)
    char web_password[33];     // Web UI password (32 chars + null)
    char timezone[33];         // Timezone string (32 chars + null)
    uint8_t motion_threshold;  // Motion detection threshold
    uint8_t motion_cooldown;   // Motion detection cooldown (seconds)
    uint32_t magic;            // Config magic number
    uint32_t version;          // Config version number
} cam_config_t;

/*
 * 旧版配置结构体 (v1) — 保留用于 NVS 迁移参考
 *
 * typedef struct {
 *     char wifi_ssid[32];
 *     char wifi_pass[64];
 *     char server_url[128];
 *     char device_name[32];
 *     uint32_t magic;
 *     uint32_t version;
 * } device_config_t;
 */

/**
 * @brief 初始化配置管理系统
 *        初始化 NVS，加载或迁移配置（v1→v2 自动迁移）
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t config_init(void);

/**
 * @brief 获取当前配置
 * @return 配置结构体指针（指向静态实例）
 */
const cam_config_t* config_get(void);

/**
 * @brief 保存配置到 NVS
 * @param config 配置结构体指针
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t config_save(const cam_config_t *config);

/**
 * @brief 重置配置为默认值并保存到 NVS
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t config_reset(void);

/**
 * @brief 验证配置有效性
 * @param config 配置结构体指针
 * @return true 配置有效，false 配置无效
 */
bool config_is_valid(const cam_config_t *config);

/**
 * @brief 更新并保存配置（便捷函数）
 * @param config 新的配置值
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t config_set(const cam_config_t *config);

/**
 * @brief 获取时区字符串
 * @return 时区字符串指针
 */
const char* config_get_timezone(void);

/**
 * @brief 从 NVS 加载配置到提供的结构体
 * @param config 配置结构体指针
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t config_load_from_nvs(cam_config_t *config);

#endif // CONFIG_MANAGER_H
