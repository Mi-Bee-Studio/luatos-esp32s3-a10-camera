/*
 * WiFi 管理模块头文件
 * 功能：管理 WiFi 的 AP 模式和 STA 模式，状态机驱动
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"

// WiFi states
typedef enum {
    WIFI_STATE_AP,               // AP mode active
    WIFI_STATE_STA_CONNECTING,   // STA connecting to router
    WIFI_STATE_STA_CONNECTED,    // STA connected
    WIFI_STATE_STA_DISCONNECTED, // STA disconnected (will retry)
} wifi_state_t;

// Callback type for state changes
typedef void (*wifi_state_callback_t)(wifi_state_t state, void *user_data);

/**
 * @brief 初始化 WiFi 子系统（创建 netif、事件循环、注册 handler）
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t wifi_init(void);

/**
 * @brief 启动 AP 模式 (SSID: MiBeeCam, password: 12345678)
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t wifi_start_ap(void);

/**
 * @brief 启动 STA 模式并连接到指定 WiFi
 * @param ssid WiFi SSID
 * @param pass WiFi 密码
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t wifi_start_sta(const char *ssid, const char *pass);

/**
 * @brief 获取当前 WiFi 状态
 * @return 当前 wifi_state_t
 */
wifi_state_t wifi_get_state(void);

/**
 * @brief 获取当前 IP 地址字符串（静态缓冲区，无需 free）
 * @return IP 字符串指针，无 IP 时返回 "0.0.0.0"
 */
const char* wifi_get_ip_str(void);

/**
 * @brief 注册 WiFi 状态变更回调
 * @param cb 回调函数
 * @param user_data 透传给回调的用户数据
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t wifi_register_callback(wifi_state_callback_t cb, void *user_data);

/**
 * @brief 停止 WiFi
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t wifi_stop(void);

/**
 * @brief 反初始化 WiFi 子系统（注销 handler、销毁 netif、deinit）
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t wifi_manager_deinit(void);

#endif // WIFI_MANAGER_H
