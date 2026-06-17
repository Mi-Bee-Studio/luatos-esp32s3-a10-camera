# MiBeeCam 软件架构文档

## 🏗️ 系统架构概述

MiBeeCam 采用模块化设计，基于 ESP-IDF v5.5.4 和 FreeRTOS 实时操作系统，构建了完整的智能摄像头软件架构。

## 📁 项目结构

```
luatos-esp32s3-a10-camera/
├── main/                    # 主程序目录
│   ├── main.c              # 系统入口，15 步启动流程
│   ├── at_command.c/h      # UART0 AT 命令接口（20 个命令）
│   ├── camera_driver.c/h   # OV2640 摄像头驱动
│   ├── config_manager.c/h  # NVS 配置存储
│   ├── event_bus.c/h       # 内存发布/订阅事件总线
│   ├── frame_broadcaster.c/h # DRAM 帧缓存（引用计数）
│   ├── health_monitor.c/h  # 健康监控 + Prometheus 指标
│   ├── motion_detect.c/h   # 运动检测
│   ├── mjpeg_streamer.c/h  # MJPEG 流媒体
│   ├── onvif_discovery.c/h # ONVIF WS-Discovery
│   ├── onvif_service.c/h   # ONVIF SOAP 服务
│   ├── status_led.c/h      # 状态 LED 控制
│   ├── time_sync.c/h       # NTP 时间同步
│   ├── web_server.c/h      # HTTP 服务器 + REST API
│   ├── webhook.c/h         # Webhook 事件转发
│   ├── wifi_manager.c/h    # WiFi STA/AP 管理
│   ├── cJSON.c/h           # JSON 解析器（第三方）
│   ├── web_ui/             # SPIFFS 静态文件
│   │   ├── index.html      # 主界面
│   │   ├── preview.html    # 预览界面
│   │   └── config.html     # 配置界面
│   └── CMakeLists.txt      # 组件注册
├── docs/                   # 双语文档（en/ + zh-CN/）
├── .github/workflows/      # CI/CD
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
└── idf_component.yml       # ESP-IDF 组件依赖
```

## 🔧 核心模块详解

### 1. 主程序模块 (main.c)

**功能**: 系统初始化和 ~19 步启动流程

**启动顺序**:
1. NVS 初始化
2. 配置加载 (v1→v2→v3 自动迁移)
3. LED 初始化
3.5 事件总线初始化 (模块间 pub/sub 通信)
4. SPIFFS 挂载
5. 摄像头初始化 (WiFi 之前，避免 I2C 冲突)
5.5 帧广播器初始化 (DRAM 帧缓存，用于运动检测和视频流)
6. WiFi 子系统初始化
7. 健康监控初始化
8. 模式选择 (STA 如果已配置 WiFi，否则 AP)
9. STA: WiFi 连接 / AP: 启动热点
10. MJPEG 流媒体初始化
11. Web 服务器启动
12. NTP 时间同步 (STA 模式)
13. 运动检测启动 (STA 模式)
13.5 Webhook 初始化 (STA 模式，按配置 URL 条件启动)
13.7 ONVIF 启动 (WS-Discovery + SOAP 服务，按配置条件启动)
14. BOOT 按钮监控

### 2. 摄像头驱动模块 (camera_driver.c/h)

**功能**: OV2640 传感器驱动和图像采集

**关键特性**:
- 固定使用 `CAMERA_MODEL_Air_ESP32S3` 模型
- 支持多种分辨率: VGA (640x480), SVGA (800x600), XGA (1024x768), UXGA (1600x1200)
- 默认分辨率: **VGA (640x480)**
- 默认帧率: 15 FPS (范围 1-30)
- 默认 JPEG 质量: 12 (范围 1-63，数值越低质量越好)
- 单帧缓冲: `CAMERA_FB_IN_DRAM`, `fb_count=1`

**图像格式**:
- 格式: JPEG
- 质量等级: 1-63
- 存储位置: DRAM (不是 PSRAM)
- 帧缓冲大小: VGA ~60KB, SVGA ~480KB

### 3. 配置管理模块 (config_manager.c/h)

**功能**: NVS 配置管理和版本迁移

**配置项**:
```c
typedef struct {
    uint32_t version;          // 配置版本 v2
    uint32_t magic;            // 魔数 0xA5B6C7D8
    char device_name[32];      // 设备名称 "MiBeeCam"
    char server_url[256];      // 服务器 URL
    char wifi_ssid[32];        // WiFi SSID
    char wifi_pass[64];        // WiFi 密码
    int motion_threshold;      // 运动检测阈值 5
    int motion_cooldown;      // 冷却时间 10 秒
    char timezone[16];         // 时区 "CST-8"
} system_config_t;
```

**版本迁移**:
- 自动从 v1 迁移到 v2
- 保持向后兼容性
- 魔数验证确保数据完整性

### 4. WiFi 管理模块 (wifi_manager.c/h)

**功能**: WiFi 连接管理和 AP 模式 fallback

**连接策略**:
1. 优先尝试 STA 模式连接已配置 WiFi
2. 连接失败时启动 AP 模式热点
3. 支持事件驱动的状态管理

**AP 模式配置**:
- SSID: "MiBeeCam"
- 密码: "12345678"
- IP: 192.168.4.1
- 2.4GHz 频段 only

### 5. Web 服务器模块 (web_server.c/h)

**功能**: HTTP 服务器和 REST API 服务

**配置**:
- 端口: 80
- 最大请求头长度: 2048 字节
- SPIFFS 静态文件服务
- CORS 支持
- 10 个 API 端点

**API 端点**:
1. GET / — 主页面 (SPIFFS index.html)
2. GET /preview.html — 预览页面
3. GET /config.html — 配置页面
4. GET /stream — MJPEG 视频流
5. GET /capture — 单帧捕获 + 上传
6. GET /api/status — JSON 系统状态
7. GET /api/config — JSON 当前配置
8. POST /api/config — 更新配置
9. POST /api/reboot — 重启设备
10. GET /metrics — Prometheus 指标

### 6. MJPEG 流媒体模块 (mjpeg_streamer.c/h)

**功能**: 实时 MJPEG 视频流传输

**流媒体特性**:
- 格式: `multipart/x-mixed-replace`
- 最大并发客户端: 2
- 分块大小: 4KB
- 默认分辨率: VGA (640x480)
- 帧率: 15 FPS

**性能优化**:
- 双缓冲机制
- 帧率控制
- 内存管理优化

### 7. 运动检测模块 (motion_detect.c/h)

**功能**: JPEG 帧差运动检测和图像上传

**检测算法**:
- 使用 JPEG 字节比较，不是像素级比较
- 采样步长: `SAMPLE_STEP = 10`
- 像素变化阈值: `PIXEL_DELTA = 20`
- 比较间隔: 500ms

**上传配置**:
- 最大重试次数: 3 次
- 重试间隔: 2000ms
- 上传协议: POST image/jpeg

```c
// 运动检测算法参数
#define SAMPLE_STEP 10          // 采样步长
#define PIXEL_DELTA 20          // 像素变化阈值
#define COMPARE_INTERVAL 500     // 比较间隔 (ms)
#define UPLOAD_MAX_RETRIES 3    // 最大重试次数
#define UPLOAD_RETRY_DELAY 2000  // 重试间隔 (ms)
```

### 8. 状态 LED 模块 (status_led.c/h)

**功能**: 系统状态指示灯控制

**LED 引脚**: GPIO 10
**控制方式**: 50ms 定时器
**状态模式**:
- LED_STARTING — 启动中
- LED_WIFI_CONNECTING — WiFi 连接中
- LED_RUNNING — 正常运行
- LED_ERROR — 错误状态
- LED_AP_MODE — AP 模式

### 9. 健康监控模块 (health_monitor.c/h)

**功能**: 系统健康状态监控和 Prometheus 指标

**监控内容**:
- CPU 使用率
- 内存使用情况
- WiFi 连接状态
- 摄像头状态
- 任务状态

**指标格式**: Prometheus 兼容 `/metrics` 端点

### 10. 时间同步模块 (time_sync.c/h)

**功能**: NTP 时间同步和时区管理

**同步配置**:
- NTP 服务器: pool.ntp.org
- 同步间隔: 1 小时
- 时区: CST-8
- 仅在 STA 模式下工作

### 11. 事件总线模块 (event_bus.c/h)
**文件**: `main/event_bus.c`  
**功能**: 轻量级内存发布/订阅事件总线

**特性**:
- 9 种事件类型: 运动检测、WiFi 状态、流客户端、健康告警、上传结果
- 最多 8 个订阅者，同步分发
- 互斥锁保护订阅/取消订阅操作，线程安全
- 被 webhook、WebSocket 事件推送、健康监控、WiFi 管理器、流媒体模块使用

### 12. 帧广播器模块 (frame_broadcaster.c/h)
**文件**: `main/frame_broadcaster.c`  
**功能**: 单消费者 DRAM 帧缓存（引用计数）

**特性**:
- 50KB 静态 DRAM 缓冲区（PSRAM 已禁用）
- 引用计数帧访问，acquire/release 模式
- 丢弃策略：上一帧未释放时新帧被丢弃（绝不阻塞生产者）
- 被运动检测和 MJPEG 流媒体模块作为主要帧源

### 13. Webhook 模块 (webhook.c/h)
**文件**: `main/webhook.c`  
**功能**: 异步 HTTP Webhook 客户端，用于事件转发

**特性**:
- 专用 FreeRTOS 任务（优先级 2，栈 6144，核心 1）
- 队列容量：8 个事件，满时丢弃最旧事件
- JSON 负载：设备名、事件类型、时间戳、数据
- 订阅所有 event_bus 事件实现自动转发
- 发送结果发布 EVENT_UPLOAD_SUCCESS / EVENT_UPLOAD_FAILED
- 默认禁用。需 CONFIG_MIBEECAM_ENABLE_WEBHOOK + 配置 webhook_url 非空

### 14. ONVIF 发现模块 (onvif_discovery.c/h)
**文件**: `main/onvif_discovery.c`  
**功能**: ONVIF WS-Discovery UDP 多播监听

**特性**:
- 监听 239.255.255.250:3702 WS-Discovery Probe 消息
- 回复 ProbeMatches，包含设备服务地址
- 专用任务（优先级 3，栈 4096，核心 1）
- 默认禁用。需 CONFIG_MIBEECAM_ENABLE_ONVIF + 配置 onvif_enabled=1

### 15. ONVIF 服务模块 (onvif_service.c/h)
**文件**: `main/onvif_service.c`  
**功能**: ONVIF Profile S SOAP 服务处理器

**特性**:
- 注册 /onvif/device_service 和 /onvif/media_service POST 处理器
- 实现 5 个 SOAP 方法: GetDeviceInformation, GetCapabilities, GetServices, GetProfiles, GetStreamUri
- Profile S 子集（无 RTSP 服务器 — GetStreamUri 仅用于发现返回 rtsp:// URL）
- 默认禁用。需 CONFIG_MIBEECAM_ENABLE_ONVIF + 配置 onvif_enabled=1



## 🔄 FreeRTOS 任务调度

### 任务表

| 任务 | 优先级 | 栈大小 | 用途 |
|------|--------|--------|------|
| Main task | default | 8192 | 系统启动 |
| WiFi event | 3 | default | WiFi 事件监控 |
| Health monitor | 4 | 4096 | 系统健康检查 (60s) |
| MJPEG stream | 5 | 8192 | 视频流 (最大 2 客户端) |
| Motion detect | 5 | 8192 | 运动检测 + 上传 |
| Time sync | 4 | default | NTP 时间同步 |
| BOOT button | 5 | 4096 | 5s 按键监控 |

### 任务协作机制

1. **WiFi 任务**: 事件驱动，监听 WiFi 状态变化
2. **MJPEG 任务**: 高优先级，保证视频流畅性
3. **运动检测**: 高优先级，保证及时响应
4. **健康监控**: 低优先级，定期检查系统状态

## 🗄️ 数据流架构

### 图像采集流
```
OV2640 → Camera Driver → Frame Buffer → MJPEG Streamer → HTTP Response
```

### 运动检测流
```
Frame Buffer → Motion Detect → Change Detection → Upload Task → HTTP POST
```

### 配置管理流
```
Web UI → HTTP POST → Config Manager → NVS Storage → System Update
```

## 🔌 接口协议

### HTTP 协议
- **版本**: HTTP/1.1
- **端口**: 80
- **支持**: GET, POST 方法
- **内容类型**: application/json, image/jpeg, multipart/x-mixed-replace

### JSON 数据格式
```json
{
  "status": "ok",
  "device": "MiBeeCam",
  "uptime": 12345,
  "wifi": "connected",
  "camera": "active"
}
```

### 上传协议
```http
POST {server_url}/upload
Content-Type: image/jpeg
X-Device-ID: {device_name}

[JPEG 二进制数据]
```

## 🛠️ 依赖管理

### 依赖项
- **唯一依赖**: `espressif/esp32-camera ^2.0.0`
- **管理方式**: 通过 `idf_component.yml` 配置

### 构建系统
- **构建工具**: CMake + ESP-IDF
- **编译器**: xtensa-esp32s3-elf-gcc v12.2.0+
- **组件注册**: 通过 CMakeLists.txt 自动注册

## 🔐 安全考虑

### 数据安全
- 配置数据存储在 NVS 中加密
- WiFi 密码安全存储
- 上传数据 HTTPS 推荐但非强制

### 网络安全
- 支持 CORS 跨域请求
- HTTP 头长度限制防止攻击
- 用户输入验证

---

*软件架构文档 - 最后更新: 2026-04-29*