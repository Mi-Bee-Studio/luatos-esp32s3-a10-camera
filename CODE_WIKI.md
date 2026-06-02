# MiBeeCam 项目代码文档

## 1 项目概述

### 1.1 项目简介

MiBeeCam 是基于 ESP-IDF v5.4.3 开发的智能摄像头系统，专为 LuatOS ESP32-S3-A10 开发板设计，配备 OV2640 图像传感器（8225N 模块）。该项目集成了丰富的功能特性，包括实时视频流、运动检测、远程上传、Web 配置界面等，所有功能均集成在单个固件中。

### 1.2 核心技术栈

| 技术领域 | 技术选型 |
|---------|---------|
| 开发框架 | ESP-IDF v5.4.3 |
| 主控芯片 | ESP32-S3 双核 240 MHz |
| 图像传感器 | OV2640（8225N 模块） |
| 无线通信 | WiFi 2.4 GHz（802.11 b/g/n） |
| 固件存储 | 16 MB Flash |
| 文件系统 | SPIFFS |
| 配置存储 | NVS（非易失性存储） |
| HTTP 服务器 | esp_http_server |

### 1.3 主要功能特性

MiBeeCam 系统提供了完整的功能套件，涵盖图像采集、网络通信、视频流传输、智能检测等多个方面。摄像头捕获模块支持 OV2640 传感器，默认采用 VGA 分辨率（640×480），同时支持 SVGA、XGA 和 UXGA 等更高分辨率，输出格式为 JPEG。WiFi 管理模块实现了 STA 和 AP 两种工作模式，STA 模式下支持自动重连功能，AP 模式作为首次配置的热点备用方案。Web 界面模块通过 SPIFFS 文件系统提供仪表盘、实时 MJPEG 预览和配置页面，用户可以通过浏览器直接访问和控制设备。MJPEG 流媒体模块通过 /stream 端点实现最高 15 FPS 的实时视频传输，支持多客户端并发连接。运动检测模块采用帧差算法，可配置检测阈值和冷却时间，检测到运动后自动将 JPEG 图片上传至远程服务器，并支持 3 次重试机制。配置管理模块使用 NVS 实现配置的持久化存储，支持通过 Web UI 或 REST API 进行配置更新。健康监控模块提供 Prometheus 兼容的 /metrics 端点，定期上报系统堆内存、任务状态和芯片温度等指标。状态 LED 模块通过 GPIO 10 的 LED 指示灯显示设备当前状态，包括启动、WiFi 连接、运行、错误和 AP 模式等。时间同步模块通过 NTP 服务器（pool.ntp.org）实现系统时间同步，支持可配置的时区设置。

## 2 项目架构

### 2.1 系统架构图

```
┌─────────────────────────────────────────────────────────────┐
│                      MiBeeCam 系统架构                        │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐      │
│  │  Web UI    │    │  REST API   │    │ MJPEG Stream│      │
│  │ (SPIFFS)   │    │  (HTTP)     │    │  (/stream)  │      │
│  └──────┬──────┘    └──────┬──────┘    └──────┬──────┘      │
│         │                  │                  │             │
│         └──────────────────┼──────────────────┘             │
│                            │                                │
│                   ┌────────▼────────┐                      │
│                   │   Web Server    │                      │
│                   │  (Port 80)      │                      │
│                   └────────┬────────┘                      │
│                            │                                │
│  ┌─────────────────────────┼─────────────────────────┐     │
│  │                         │                          │     │
│  ▼                         ▼                          ▼     │
│ ┌────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐      │
│ │ Camera │  │  Motion  │  │  Health  │  │  Time    │      │
│ │ Driver │  │  Detect  │  │ Monitor  │  │  Sync    │      │
│ └────────┘  └──────────┘  └──────────┘  └──────────┘      │
│      │                                                        │
│      ▼                                                        │
│ ┌────────────────────────────────────────┐                   │
│ │           Config Manager (NVS)          │                   │
│ └────────────────────────────────────────┘                   │
│                            │                                │
│  ┌─────────────────────────┼─────────────────────────┐     │
│  │                         │                          │     │
│  ▼                         ▼                          ▼     │
│ ┌────────┐          ┌──────────┐             ┌──────────┐   │
│ │ WiFi   │          │  Status  │             │  cJSON   │   │
│ │Manager │          │   LED    │             │ (Parser) │   │
│ └────────┘          └──────────┘             └──────────┘   │
│      │                                                        │
│      ▼                                                        │
│ ┌────────────────────────────────────────┐                   │
│ │         ESP32-S3 Hardware               │                   │
│ │    (WiFi / Camera / GPIO / SPI)         │                   │
│ └────────────────────────────────────────┘                   │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 模块依赖关系

系统采用分层架构设计，各模块之间存在清晰的依赖关系。最底层是硬件抽象层，包括 ESP-IDF 驱动和底层外设接口。核心管理层包含配置管理器和状态 LED 管理器，这两个模块在系统初始化早期启动，为其他模块提供基础服务。通信层包括 WiFi 管理器和 Web 服务器，其中 WiFi 管理器负责网络连接，Web 服务器负责 HTTP 服务。功能层包含摄像头驱动、MJPEG 流媒体、运动检测、健康监控和时间同步等模块，这些模块依赖下层服务实现具体功能。最顶层是用户界面层，包括 SPIFFS 文件系统和 HTML/CSS/JavaScript 编写的 Web UI。

模块间的具体依赖关系如下：主程序（main.c）依赖所有功能模块并协调初始化顺序；摄像头驱动（camera_driver）被 MJPEG 流媒体、运动检测和主程序使用；WiFi 管理器（wifi_manager）被主程序调用，提供网络连接服务；Web 服务器（web_server）依赖配置管理器和所有功能模块，提供 HTTP 接口；配置管理器（config_manager）被多个模块访问，提供配置存储服务；MJPEG 流媒体（mjpeg_streamer）依赖摄像头驱动；运动检测（motion_detect）依赖摄像头驱动和配置管理器；健康监控（health_monitor）独立运行，收集系统状态；时间同步（time_sync）依赖 WiFi 连接状态。

## 3 主要模块详解

### 3.1 系统入口模块（main.c）

#### 3.1.1 模块概述

main.c 是整个固件的入口点，负责系统的初始化和协调各模块的启动顺序。该文件实现了严格的 14 步初始化流程，确保各模块按照正确的依赖顺序启动。

#### 3.1.2 核心数据结构

| 数据结构 | 类型 | 说明 |
|---------|------|------|
| TAG | static const char* | 日志标签，值为"main" |
| s_web_server_started | static bool | 标记 Web 服务器是否已启动 |
| s_motion_started | static bool | 标记运动检测是否已启动 |
| s_time_sync_started | static bool | 标记时间同步是否已启动 |

#### 3.1.3 关键函数

**app_main 函数**

函数原型：void app_main(void)

函数功能：系统主入口，实现 14 步初始化序列。初始化流程严格遵循依赖关系，从底层到上层依次启动。

初始化步骤详解：第一步初始化 NVS 闪存，如果遇到 NVS 分区需要擦除或版本不匹配的情况，会自动擦除并重新初始化。第二步初始化配置系统，加载或迁移配置数据。第三步初始化 LED 状态指示灯，设置初始状态为 LED_STARTING。第四步挂载 SPIFFS 文件系统用于存储 Web UI 文件。第五步初始化 OV2640 摄像头（必须在 WiFi 初始化之前执行，以避免 I2C 总线冲突）。第六步初始化 WiFi 子系统，包括创建网络接口和事件循环。第七步初始化健康监控模块。第八步根据配置决定工作模式（STA 或 AP）。第九步启动 WiFi 连接或 AP 热点。第十步初始化 MJPEG 流媒体（在 STA 模式下）。第十一步启动 Web 服务器。第十二步启动 NTP 时间同步（仅在 STA 模式下）。第十三步启动运动检测（仅在 STA 模式下）。第十四步创建 BOOT 按钮监控任务，长按 5 秒触发恢复出厂设置。

**wifi_state_cb 回调函数**

函数原型：static void wifi_state_cb(wifi_state_t state, void *user_data)

函数功能：WiFi 状态变化回调函数，根据状态变化触发后续服务的启动。该函数在 WiFi 连接成功后被调用，依次启动时间同步、Web 服务器和运动检测功能，确保这些依赖网络的功能只在有效连接建立后才启动。

**boot_btn_task 任务函数**

函数原型：static void boot_btn_task(void *arg)

函数功能：监控 BOOT 按钮（GPIO 0）的长按状态。持续检测按钮电平，当检测到低电平时等待 5 秒，如果 5 秒后仍为低电平则触发恢复出厂设置操作。

### 3.2 摄像头驱动模块（camera_driver）

#### 3.2.1 模块概述

camera_driver 模块封装了对 OV2640 摄像头的所有操作，包括初始化、帧捕获、分辨率控制和资源管理。该模块基于 ESP32-Camera 组件实现，提供了简洁的上层接口。

#### 3.2.2 支持的分辨率

| 枚举值 | 分辨率 | 像素尺寸 |
|-------|--------|---------|
| CAMERA_RES_VGA | 640×480 | VGA |
| CAMERA_RES_SVGA | 800×600 | SVGA |
| CAMERA_RES_XGA | 1024×768 | XGA |
| CAMERA_RES_UXGA | 1600×1200 | UXGA |

#### 3.2.3 核心 API

**camera_init 函数**

函数原型：esp_err_t camera_init(camera_resolution_t resolution, uint8_t fps, uint8_t jpeg_quality)

参数说明：resolution 参数指定摄像头分辨率，可选值为 CAMERA_RES_VGA、CAMERA_RES_SVGA、CAMERA_RES_XGA 或 CAMERA_RES_UXGA，默认值为 VGA。fps 参数指定帧率，范围为 1-30，默认值为 15。jpeg_quality 参数指定 JPEG 压缩质量，范围为 1-63，值越小质量越高，默认值为 12。

返回值：成功返回 ESP_OK，失败返回相应错误码。

功能说明：该函数配置并初始化 OV2640 传感器，设置输出分辨率、帧率和 JPEG 压缩质量。初始化过程包括配置引脚映射、DMA 通道和帧缓冲区。

**camera_capture 函数**

函数原型：esp_err_t camera_capture(camera_fb_t **fb)

参数说明：fb 是输出参数，返回指向帧缓冲区的指针。

返回值：成功返回 ESP_OK，未初始化返回 ESP_ERR_NOT_SUPPORTED。

功能说明：捕获单帧图像。调用者使用完毕后必须调用 camera_return_fb 释放帧缓冲区。

**camera_deinit 函数**

函数原型：esp_err_t camera_deinit(void)

返回值：成功返回 ESP_OK。

功能说明：反初始化摄像头，释放所有相关资源。

### 3.3 WiFi 管理模块（wifi_manager）

#### 3.3.1 模块概述

wifi_manager 模块负责 WiFi 子系统的初始化和 STA/AP 两种工作模式的管理。模块内部维护状态机，通过回调机制通知其他模块连接状态的变化。

#### 3.3.2 WiFi 状态枚举

| 状态值 | 说明 |
|-------|------|
| WIFI_STATE_AP | AP 模式已激活 |
| WIFI_STATE_STA_CONNECTING | STA 模式正在连接路由器 |
| WIFI_STATE_STA_CONNECTED | STA 模式已连接 |
| WIFI_STATE_STA_DISCONNECTED | STA 模式断开连接（将自动重连） |

#### 3.3.3 核心 API

**wifi_init 函数**

函数原型：esp_err_t wifi_init(void)

返回值：成功返回 ESP_OK。

功能说明：初始化 WiFi 子系统，包括创建网络接口和事件循环。

**wifi_start_ap 函数**

函数原型：esp_err_t wifi_start_ap(void)

返回值：成功返回 ESP_OK。

功能说明：启动 AP 模式，创建名为"MiBeeCam"的热点，默认密码为"12345678"。该模式用于设备首次配置，此时设备作为 WiFi 热点允许用户连接进行配置。

**wifi_start_sta 函数**

函数原型：esp_err_t wifi_start_sta(const char *ssid, const char *pass)

参数说明：ssid 参数指定要连接的 WiFi 网络名称，pass 参数指定密码。

返回值：成功返回 ESP_OK。

功能说明：启动 STA 模式并连接到指定的 WiFi 网络。如果连接失败会自动重试。

**wifi_register_callback 函数**

函数原型：esp_err_t wifi_register_callback(wifi_state_callback_t cb, void *user_data)

参数说明：cb 是状态变化回调函数，user_data 是传递给回调的用户数据。

返回值：成功返回 ESP_OK。

功能说明：注册 WiFi 状态变化回调。当 WiFi 连接状态发生变化时，已注册的回调函数会被调用。

**wifi_get_ip_str 函数**

函数原型：const char* wifi_get_ip_str(void)

返回值：返回当前 IP 地址字符串，未获取到 IP 时返回"0.0.0.0"。

功能说明：获取设备当前 IP 地址，返回静态缓冲区，无需释放。

### 3.4 配置管理模块（config_manager）

#### 3.4.1 模块概述

config_manager 模块负责管理系统配置数据，实现配置的持久化存储和版本迁移。该模块使用 ESP-IDF 的 NVS（非易失性存储）系统存储配置，支持从旧版本配置自动迁移到新版本。

#### 3.4.2 配置结构体

```c
typedef struct {
    char wifi_ssid[33];        // WiFi SSID（32字符+结束符）
    char wifi_pass[65];        // WiFi 密码（64字符+结束符）
    char server_url[129];      // 上传服务器 URL（128字符+结束符）
    char device_name[33];      // 设备名称（32字符+结束符）
    uint8_t resolution;        // 摄像头分辨率（0=VGA, 1=SVGA, 2=XGA, 3=UXGA）
    uint8_t fps;              // 摄像头帧率（1-30）
    uint8_t jpeg_quality;     // JPEG 质量（1-63，值越小质量越高）
    char web_password[33];     // Web UI 密码（32字符+结束符）
    char timezone[33];         // 时区字符串（POSIX 格式）
    uint8_t motion_threshold; // 运动检测阈值（1-255）
    uint8_t motion_cooldown;  // 运动检测冷却时间（秒）
    uint32_t magic;           // 配置魔数（用于验证有效性）
    uint32_t version;         // 配置版本号
} cam_config_t;
```

#### 3.4.3 配置常量定义

| 常量名 | 值 | 说明 |
|-------|-----|------|
| CONFIG_VERSION | 2 | 当前配置版本号 |
| CONFIG_MAGIC | 0xA5B6C7D8 | 配置魔数 |
| CONFIG_DEFAULT_TIMEZONE | "CST-8" | 默认时区（中国标准时间） |
| CONFIG_DEFAULT_DEVICE_NAME | "MiBeeCam" | 默认设备名称 |

#### 3.4.4 核心 API

**config_init 函数**

函数原型：esp_err_t config_init(void)

返回值：成功返回 ESP_OK。

功能说明：初始化配置管理系统，加载现有配置或使用默认值。如果检测到旧版本配置（v1），会自动执行 v1 到 v2 的迁移。

**config_get 函数**

函数原型：const cam_config_t* config_get(void)

返回值：返回指向内部静态配置实例的指针。

功能说明：获取当前配置。返回的指针指向模块内部的静态实例，不要尝试释放。

**config_save 函数**

函数原型：esp_err_t config_save(const cam_config_t *config)

参数说明：config 是要保存的配置数据指针。

返回值：成功返回 ESP_OK。

功能说明：将配置保存到 NVS 持久化存储。

**config_reset 函数**

函数原型：esp_err_t config_reset(void)

返回值：成功返回 ESP_OK。

功能说明：重置配置为默认值并保存到 NVS。通常在恢复出厂设置时调用。

**config_is_valid 函数**

函数原型：bool config_is_valid(const cam_config_t *config)

参数说明：config 是要验证的配置指针。

返回值：配置有效返回 true，否则返回 false。

功能说明：验证配置的有效性，检查魔数和版本号是否匹配。

### 3.5 Web 服务器模块（web_server）

#### 3.5.1 模块概述

web_server 模块实现了 HTTP 服务器功能，提供 REST API 接口和 SPIFFS 静态文件服务。该模块是系统对外交互的主要接口，支持配置查看、配置更新、设备控制等功能。

#### 3.5.2 API 端点列表

| 方法 | 路径 | 说明 |
|-----|------|------|
| GET | `/` | 仪表盘页面（index.html） |
| GET | `/preview.html` | 实时预览页面 |
| GET | `/config.html` | 配置页面 |
| GET | `/stream` | MJPEG 视频流 |
| GET | `/capture` | 单次 JPEG 捕获并上传 |
| GET | `/api/status` | 获取系统状态（JSON） |
| GET | `/api/config` | 获取当前配置（JSON） |
| POST | `/api/config` | 更新配置（JSON） |
| POST | `/api/reboot` | 重启设备 |
| GET | `/metrics` | Prometheus 指标 |

#### 3.5.3 核心 API

**web_server_start 函数**

函数原型：esp_err_t web_server_start(uint16_t port)

参数说明：port 指定 HTTP 服务器监听的端口号，通常为 80。

返回值：成功返回 ESP_OK。

功能说明：启动 HTTP 服务器，注册所有 API 端点和 SPIFFS 文件处理器。

**web_server_stop 函数**

函数原型：void web_server_stop(void)

功能说明：停止 HTTP 服务器并释放相关资源。

**web_server_get_handle 函数**

函数原型：httpd_handle_t web_server_get_handle(void)

返回值：返回当前 HTTP 服务器句柄，未运行返回 NULL。

功能说明：获取 HTTP 服务器句柄，用于注册自定义 URI 处理器。

### 3.6 MJPEG 流媒体模块（mjpeg_streamer）

#### 3.6.1 模块概述

mjpeg_streamer 模块实现实时 MJPEG 视频流功能。该模块通过 multipart/x-mixed-replace 协议实现持续的流媒体传输，支持最多 2 个并发客户端连接。

#### 3.6.2 核心 API

**mjpeg_streamer_init 函数**

函数原型：esp_err_t mjpeg_streamer_init(void)

返回值：成功返回 ESP_OK，内存分配失败返回 ESP_ERR_NO_MEM。

功能说明：初始化 MJPEG 流媒体模块，创建内部互斥锁并重置客户端计数。该函数必须在注册 URI 处理器之前调用一次。

**mjpeg_streamer_register 函数**

函数原型：esp_err_t mjpeg_streamer_register(httpd_handle_t server)

参数说明：server 是 HTTP 服务器句柄，不能为 NULL。

返回值：成功返回 ESP_OK，参数无效返回 ESP_ERR_INVALID_ARG。

功能说明：注册 /stream URI 处理器到 HTTP 服务器。

**mjpeg_streamer_http_handler 函数**

函数原型：esp_err_t mjpeg_streamer_http_handler(httpd_req_t *req)

参数说明：req 是 HTTP 请求对象。

返回值：正常结束返回 ESP_OK，发生错误返回 ESP_FAIL。

功能说明：处理 GET /stream 请求，发送 MJPEG 流直到客户端断开或捕获失败。如果已连接的客户端数达到上限，返回 503 状态码。

**mjpeg_streamer_get_client_count 函数**

函数原型：int mjpeg_streamer_get_client_count(void)

返回值：返回当前连接的流媒体客户端数量。

功能说明：查询当前活动的流媒体客户端数量。

### 3.7 运动检测模块（motion_detect）

#### 3.7.1 模块概述

motion_detect 模块实现基于帧差法的运动检测功能。该模块通过比较连续两帧图像的像素差异来判断是否有运动发生，检测到运动后会自动捕获并上传 JPEG 图片到配置的服务器。

#### 3.7.2 核心 API

**motion_detect_start 函数**

函数原型：esp_err_t motion_detect_start(void)

返回值：成功返回 ESP_OK。

功能说明：启动运动检测任务。任务会持续捕获帧、比较像素差异、判断运动，并在检测到运动时上传图片。

**motion_detect_stop 函数**

函数原型：void motion_detect_stop(void)

功能说明：停止运动检测任务。

**motion_detect_is_running 函数**

函数原型：bool motion_detect_is_running(void)

返回值：运动检测正在运行返回 true，否则返回 false。

功能说明：检查运动检测任务是否正在运行。

### 3.8 健康监控模块（health_monitor）

#### 3.8.1 模块概述

health_monitor 模块实现系统健康状态监控功能。该模块定期收集系统堆内存使用情况、任务状态和芯片温度等指标，并通过 Prometheus 兼容的 /metrics 端点对外暴露。

#### 3.8.2 核心 API

**health_monitor_init 函数**

函数原型：esp_err_t health_monitor_init(void)

返回值：成功返回 ESP_OK。

功能说明：初始化健康监控系统，启动定期状态收集任务。

**health_monitor_deinit 函数**

函数原型：esp_err_t health_monitor_deinit(void)

返回值：成功返回 ESP_OK。

功能说明：反初始化健康监控系统。

**get_chip_temp 函数**

函数原型：float get_chip_temp(void)

返回值：返回芯片温度（摄氏度）。

功能说明：获取 ESP32-S3 芯片当前温度。

### 3.9 状态 LED 模块（status_led）

#### 3.9.1 模块概述

status_led 模块通过 GPIO 10 的 LED 指示灯显示系统当前状态。用户可以通过 LED 的亮灭模式判断设备工作状态。

#### 3.9.2 LED 状态枚举

| 状态值 | 闪烁模式 | 说明 |
|-------|---------|------|
| LED_STARTING | 快闪（200ms 亮/灭） | 系统启动中 |
| LED_AP_MODE | 慢闪（1000ms 亮/灭） | AP 模式 |
| LED_WIFI_CONNECTING | 双闪（200ms亮/200ms灭/200ms亮/600ms灭） | WiFi 连接中 |
| LED_RUNNING | 常亮 | 系统正常运行 |
| LED_ERROR | SOS 模式（...---...） | 系统错误 |

#### 3.9.3 核心 API

**led_init 函数**

函数原型：esp_err_t led_init(void)

返回值：成功返回 ESP_OK。

功能说明：初始化 LED GPIO，配置为输出模式。

**led_set_status 函数**

函数原型：void led_set_status(led_status_t status)

参数说明：status 指定要设置的 LED 状态。

功能说明：设置 LED 显示状态，模块会切换到相应的闪烁模式。

**led_deinit 函数**

函数原型：void led_deinit(void)

功能说明：反初始化 LED，停止闪烁任务。

### 3.10 时间同步模块（time_sync）

#### 3.10.1 模块概述

time_sync 模块通过 NTP 协议实现系统时间同步。该模块连接到 pool.ntp.org 服务器获取 UTC 时间，并根据配置的时区进行转换。

#### 3.10.2 核心 API

**time_sync_init 函数**

函数原型：esp_err_t time_sync_init(const char *timezone)

参数说明：timezone 指定时区字符串（POSIX 格式），例如"CST-8"表示中国标准时间。

返回值：成功返回 ESP_OK。

功能说明：初始化 NTP 客户端并开始时间同步。

**time_is_synced 函数**

函数原型：bool time_is_synced(void)

返回值：时间已同步返回 true，否则返回 false。

功能说明：检查 NTP 时间同步是否成功。

**time_get_str 函数**

函数原型：void time_get_str(char *buf, size_t len)

参数说明：buf 是输出缓冲区，len 是缓冲区大小。

功能说明：获取当前时间的字符串表示。

**time_set_manual 函数**

函数原型：esp_err_t time_set_manual(int year, int month, int day, int hour, int min, int sec)

参数说明：分别指定年、月、日、时、分、秒。

返回值：成功返回 ESP_OK。

功能说明：手动设置系统时间（不依赖 NTP）。

## 4 系统配置

### 4.1 分区表配置

| 分区名称 | 类型 | 偏移地址 | 大小 | 说明 |
|---------|------|---------|------|------|
| nvs | data/nvs | 0x9000 | 24 KB | 非易失性存储 |
| phy_init | data/phy | 0xf000 | 4 KB | 物理层参数 |
| factory | app/factory | 0x10000 | 3.5 MB | 应用程序 |
| otadata | data/ota | 0x390000 | 8 KB | OTA 数据 |
| spiffs | data/spiffs | 0x392000 | ~3.94 MB | SPIFFS 文件系统 |

### 4.2 SDK 配置要点

系统禁用了 PSRAM（物理存在但固件中禁用），原因是时序调优失败会导致启动循环。摄像头使用 DRAM 帧缓冲区（CAMERA_FB_IN_DRAM，fb_count=1）。CPU 主频设置为 240 MHz。任务看门狗超时设置为 30 秒，超时后触发 panic。主任务栈大小设置为 8192 字节。HTTP 服务器请求头最大长度设置为 2048 字节，URI 最大长度设置为 1024 字节。

### 4.3 摄像头引脚映射

| 引脚功能 | GPIO 编号 | 说明 |
|---------|----------|------|
| XCLK | 39 | 主时钟输出 |
| SIOD | 21 | I2C 数据线 |
| SIOC | 46 | I2C 时钟线 |
| D0-D7 | 34, 47, 48, 33, 35, 37, 38, 40 | 并行数据总线 |
| VSYNC | 42 | 垂直同步 |
| HREF | 41 | 水平参考 |
| PCLK | 36 | 像素时钟 |
| PWDN | -1 | 掉电控制（禁用） |
| RESET | -1 | 复位控制（禁用） |

## 5 项目构建与运行

### 5.1 开发环境要求

构建 MiBeeCam 项目需要以下软件环境。ESP-IDF 版本必须是 v5.4.3，不要使用 v6.0，因为该开发板存在已知的 PSRAM 问题。Python 版本需要 3.8 或更高版本。esptool.py 工具用于烧录固件。另外需要安装 espressif/esp32-camera 组件，版本为 ^2.0.0，该依赖通过 idf_component.yml 自动解析。

### 5.2 构建步骤

首先设置目标芯片，执行 idf.py set-target esp32s3。然后编译项目，执行 idf.py build。编译完成后会在 build 目录下生成固件文件。

### 5.3 烧录步骤

固件烧录使用命令 idf.py -p COMx flash monitor，其中 COMx 需要替换为实际的串口设备名。Windows 系统通常为 COM3 等，Linux 系统为 /dev/ttyUSB0 等。烧录完成后会自动打开串口监视器显示日志输出。

### 5.4 SPIFFS 烧录

Web UI 文件需要单独烧录到 SPIFFS 分区。首先生成 SPIFFS 镜像文件，执行 python $IDF_PATH/components/spiffs/spiffsgen.py 0x3CE000 main/web_ui build/spiffs.bin。然后烧录到 SPIFFS 分区，执行 python -m esptool --chip esp32s3 -p COMx write_flash 0x392000 build/spiffs.bin。

### 5.5 首次配置流程

首次使用时，设备因为没有存储 WiFi 凭证而自动进入 AP 模式。用户需要连接名为"MiBeeCam"的 WiFi 网络，密码为"12345678"。然后在浏览器中打开 http://192.168.4.1 进入配置页面，输入 WiFi SSID 和密码后保存。设备会自动重启并连接到指定的 WiFi 网络。

### 5.6 恢复出厂设置

按住 BOOT 按钮（GPIO 0）持续 5 秒，设备会清除 NVS 配置并重启进入 AP 模式。

## 6 依赖关系

### 6.1 外部依赖

| 依赖组件 | 版本 | 来源 | 用途 |
|---------|------|------|------|
| esp32-camera | ^2.0.0 | espressif/esp32-camera | OV2640 摄像头驱动 |
| esp-http-server | 内置 | ESP-IDF | HTTP 服务器 |
| nvs_flash | 内置 | ESP-IDF | 非易失性存储 |
| esp_spiffs | 内置 | ESP-IDF | SPIFFS 文件系统 |
| freertos | 内置 | ESP-IDF | 实时操作系统 |

### 6.2 内部模块依赖图

```
main.c
  ├─> config_manager.h
  ├─> wifi_manager.h
  ├─> camera_driver.h
  ├─> mjpeg_streamer.h
  ├─> web_server.h
  ├─> status_led.h
  ├─> time_sync.h
  ├─> health_monitor.h
  └─> motion_detect.h

camera_driver.h
  └─> esp_camera.h (esp32-camera)

wifi_manager.h
  └─> esp_netif.h, esp_wifi.h (ESP-IDF)

web_server.h
  ├─> esp_http_server.h
  ├─> config_manager.h
  └─> 其他功能模块头文件

mjpeg_streamer.h
  └─> camera_driver.h

motion_detect.h
  ├─> camera_driver.h
  └─> config_manager.h

health_monitor.h
  └─> (独立模块)

status_led.h
  └─> driver/gpio.h (ESP-IDF)

time_sync.h
  └─> esp_netif.h (ESP-IDF)
```

## 7 扩展指南

### 7.1 添加新的 API 端点

在 web_server.c 中定义新的 URI 处理器函数，然后使用 httpd_register_uri_handler 注册到服务器。处理器函数原型为 esp_err_t handler(httpd_req_t *req)。

### 7.2 添加新的配置项

在 config_manager.h 的 cam_config_t 结构体中添加新字段。在 config_manager.c 中更新默认值、验证逻辑和迁移代码（如果需要）。

### 7.3 修改分区表

编辑 partitions.csv 文件定义新的分区布局。更新 sdkconfig.defaults 中的 CONFIG_PARTITION_TABLE_CUSTOM_FILENAME（如果文件名改变）。重新编译并烧录整个固件。

### 7.4 调整运动检测参数

通过 Web UI 的配置页面调整运动阈值和冷却时间。阈值范围为 1-255，值越低越敏感。冷却时间范围为 1-255 秒，用于防止连续触发。

### 7.5 更换摄像头型号

如果使用不同的 OV2640 模块（如不同的引脚配置），需要修改 camera_driver.c 中的引脚定义。如果使用其他型号的摄像头传感器，需要更换 esp32-camera 组件并修改初始化代码。
