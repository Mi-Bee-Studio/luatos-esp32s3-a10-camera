# MiBeeCam ESP32-S3-A10 智能摄像头项目

## 📖 目录

- [项目概述](#项目概述)
- [快速开始](#快速开始)
- [硬件配置](hardware.md)
- [软件架构](software.md)
- [API 接口](api.md)
- [故障排查](troubleshooting.md)
- [开发指南](#开发指南)
- [许可证](#许可证)

## 🏗️ 项目概述

MiBeeCam 是基于 ESP32-S3-A10 开发的高性能智能摄像头项目，采用 ESP-IDF v5.5.4 框架开发。本项目支持实时视频流、运动检测、图像上传和 Web 配置界面，适用于智能家居监控、物联网应用等场景。
<p align="center">
    <img src="../images/index-dashboard.png" alt="MiBeeCam 仪表盘" width="640">
</p>

### 📋 特性

- **硬件平台**: ESP32-S3 双核 240MHz 处理器
- **摄像头**: OV2640 传感器，支持多种分辨率
- **连接方式**: WiFi STA/AP 模式，2.4GHz 频段
- **视频流**: MJPEG 实时流传输，最高 15 FPS
- **运动检测**: JPEG 字节比较算法
- **图像上传**: 支持 HTTP POST 上传至云端服务器
- **Web 界面**: 响应式 Web 配置和预览界面
- **监控指标**: Prometheus 兼容的系统监控
- **mDNS 发现**: 通过 http://mibee.local 访问设备
- **WebSocket 实时推送**: 实时事件推送至 Web UI
- **Webhook 客户端**: 外部 HTTP 事件转发
- **ONVIF Profile S**: WS-Discovery 发现 + SOAP 服务
- **备用 SSID**: 主 WiFi 失败时自动回退
- **WiFi 扫描**: 通过 REST API 扫描附近网络
- **AT 命令接口**: 通过 UART0 串口支持 20 个命令

### 🎯 技术栈

- **开发框架**: ESP-IDF v5.5.4
- **编程语言**: C/C++
- **网络协议**: HTTP/HTTPS, TCP/IP
- **图像格式**: JPEG
- **数据格式**: JSON
- **文件系统**: SPIFFS
- **实时系统**: FreeRTOS

## 🚀 快速开始

### 环境要求

- ESP-IDF v5.5.4
- ESP32-S3 开发板
- OV2640 摄像头模块
- 支持的编译器: xtensa-esp32s3-elf-gcc v12.2.0+

### 编译步骤

```bash
# 1. 设置 ESP-IDF 环境变量
source <esp-idf-path>/export.sh

# 2. 设置目标芯片
idf.py set-target esp32s3

# 3. 配置项目选项
idf.py menuconfig

# 4. 编译项目
idf.py build

# 5. 烧录固件
idf.py -p COMx flash

# 6. 监控串口输出
idf.py -p COMx monitor
```

### 配置说明

WiFi 凭据和服务器配置在运行时设置，无需修改代码：

1. **Web UI**: 通过浏览器访问设备 IP，在配置页面设置
2. **AT 命令**: 通过 UART0 串口（115200 波特率）发送命令：
   - `AT+CWJAP=<ssid>,<pwd>` — 设置 WiFi 凭据
   - `AT+CFGSET=server_url,http://your-server.com/upload` — 设置服务器地址
   - `AT+CFGSET=motion_threshold,5` — 设置运动检测阈值
   - `AT+CFGSET=motion_cooldown,10` — 设置运动检测冷却时间

## 🔧 基本使用

### Web 界面访问

1. **AP 模式**（首次启动或无 WiFi 配置）：连接 WiFi 网络 **MiBeeCam**（密码: 12345678），访问 http://192.168.4.1
2. **STA 模式**（已配置 WiFi）：设备连接路由器后通过 DHCP 获取 IP，可在路由器管理界面查看设备 IP
3. 浏览器打开 IP 地址即可查看实时视频流

### 主要功能

1. **实时预览**: `/preview.html` - MJPEG 实时视频流
2. **配置管理**: `/config.html` - Web 配置界面
3. **状态监控**: `/api/status` - JSON 系统状态
4. **图像捕获**: `/capture` - 单帧捕获并上传
5. **WebSocket 实时推送**: `/ws` - 实时事件推送
6. **mDNS 访问**: http://mibee.local
7. **AT 命令控制**: 通过 UART0 串口发送 AT 命令

## 📚 文档导航

| 文档 | 描述 |
|------|------|
| [硬件配置](hardware.md) | 详细硬件连接和配置说明 |
| [软件架构](software.md) | 系统架构和模块说明 |
| [API 接口](api.md) | 所有 HTTP API 接口文档 |
| [故障排查](troubleshooting.md) | 常见问题解决方案 |

## 🛠️ 开发指南

### 代码结构

```
luatos-esp32s3-a10-camera/
├── main/
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
│   └── CMakeLists.txt      # 组件注册
├── docs/                   # 双语文档（en/ + zh-CN/）
├── .github/workflows/      # CI/CD
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
└── idf_component.yml       # ESP-IDF 组件依赖
```

### 调试技巧

- 使用串口监视器查看系统启动信息
- 通过 Web 界面查看实时状态和日志
- 使用 Prometheus 指标监控系统健康状态
- 查看运动检测和上传任务的执行情况

## 📄 许可证

本项目基于 **MIT License** 许可证开源，详见 [LICENSE](../LICENSE) 文件。

**版权所有**: Mi&Bee Studio

---

*最后更新: 2026-04-29*