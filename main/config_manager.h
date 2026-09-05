/*
 * 配置管理模块头文件 — 家族配置契约 v1.0（docs/config-contract.md）
 *
 * 持久化：mibee_cfg 命名空间逐键 NVS + schema_ver 版本键（2026-09-05 起，
 * 取代旧的整块 blob+magic 方案）。legacy blob（mibee_cfg/config 或更早
 * device_cfg/config，v1/v2/v3）由 config_manager.c 的一次性迁移函数翻译。
 *
 * 字段命名/取值域与契约 §3 对齐；本板无 SD 卡，SD provisioning 组不适用。
 */

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

/* 家族 schema 版本（契约 §1）：独立于各仓历史 blob 版本号，迁移后一律 =1 */
#define CONFIG_SCHEMA_VERSION 1

/* 默认值 */
#define CONFIG_DEFAULT_TIMEZONE    "CST-8"
#define CONFIG_DEFAULT_DEVICE_NAME "MiBeeCam"
#define CONFIG_DEFAULT_MDNS_HOST   "mibee"

/* 契约 v1.0（内存模型；持久化为 mibee_cfg 逐键 NVS）
 * 字段名 = 契约 JSON 名（§3）；板级扩展（§7）：server_url（可选）、mdns_hostname。 */
typedef struct {
    /* 核心字段（契约 §3.1，四板必备）*/
    char     device_name[33];            /* str<=32 */
    char     wifi_ssid[33];              /* str<=32 */
    char     wifi_pass[65];              /* str<=64，敏感 */
    char     wifi_ssid_2[33];            /* 备用网络，空=禁用 */
    char     wifi_pass_2[65];            /* 敏感 */
    char     timezone[48];               /* POSIX TZ，str<=47（契约 §3.1）*/
    char     web_password[33];           /* >=6 位，敏感 */
    uint8_t  cam_framesize;              /* framesize_t 刻度（10-15）；本板上限 VGA=10 */
    uint8_t  cam_fps;                    /* 1-30，默认 15 */
    uint8_t  cam_quality;                /* 10-63（PIT-021），默认 12 */
    uint8_t  cam_vflip;                  /* 0/1 */
    uint8_t  cam_hmirror;                /* 0/1 */
    uint8_t  xclk_freq_mhz;              /* {10,16,20}，本板值 20（契约 §5）*/
    uint8_t  onvif_enable;               /* 0/1，家族默认 1，重启生效 */
    /* 移动侦测家族超集（契约 §3.2/§6.1）：sensitivity 越大越灵敏；
     * 旧 motion_threshold 迁移为 sensitivity = 100 - threshold */
    uint8_t  motion_enabled;             /* 默认 1 */
    uint8_t  motion_sensitivity;         /* 0-100，默认 95（=旧 threshold 5）*/
    uint16_t motion_cooldown_s;          /* 1-300，两次触发最小间隔，默认 10 */
    uint8_t  motion_active_interval_s;   /* 1-30，持续活动期再触发间隔，默认 5 */
    /* Webhook 组（契约 §3.2）*/
    uint8_t  alert_webhook_enabled;      /* 旧 url+secret 无开关 → 迁移按 url 非空补种 */
    char     alert_webhook_url[257];     /* str<=256 */
    char     webhook_secret[65];         /* str<=64，敏感 */
    /* WebSocket 组（契约 §3.2）*/
    uint8_t  ws_enable;                  /* 0/1，默认 1；0 = 不注册 /ws */
    /* 板级扩展（契约 §7 登记；server_url 自契约起为可选字段）*/
    char     server_url[129];            /* 遗留上传目标，可空 */
    char     mdns_hostname[33];          /* 默认 "mibee" */
} cam_config_t;

/**
 * @brief 初始化配置管理系统
 *        初始化 NVS，执行一次性 legacy blob 迁移（幂等），加载逐键配置
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t config_init(void);

/**
 * @brief 获取当前配置（只读指针，指向静态实例）
 */
const cam_config_t *config_get(void);

/**
 * @brief 线程安全地获取配置快照（契约 §1 统一访问层）
 */
void config_get_copy(cam_config_t *out);

/**
 * @brief 保存配置到 NVS（逐键写入，单键失败只 WARN 不中断，PIT-022）
 */
esp_err_t config_save(const cam_config_t *config);

/**
 * @brief 重置配置为默认值并保存到 NVS
 */
esp_err_t config_reset(void);

/**
 * @brief 验证配置有效性（契约 §4 校验矩阵；server_url 可选——契约 §7 修复）
 */
bool config_is_valid(const cam_config_t *config);

/**
 * @brief 更新并保存配置（便捷别名）
 */
esp_err_t config_set(const cam_config_t *config);

/**
 * @brief 获取时区字符串
 */
const char *config_get_timezone(void);

/**
 * @brief 获取配置为 JSON 对象（契约字段名；密码类掩码 "****"）
 * @return cJSON* 对象，调用者负责释放内存
 */
cJSON *config_get_json(void);

/**
 * @brief 获取 Web UI 密码
 */
const char *config_get_web_password(void);

#endif /* CONFIG_MANAGER_H */
