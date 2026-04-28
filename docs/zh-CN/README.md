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

MiBeeCam 是基于 ESP32-S3-A10 开发的高性能智能摄像头项目，采用 ESP-IDF v5.4.3 框架开发。本项目支持实时视频流、运动检测、图像上传和 Web 配置界面，适用于智能家居监控、物联网应用等场景。
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

### 🎯 技术栈

- **开发框架**: ESP-IDF v5.4.3
- **编程语言**: C/C++
- **网络协议**: HTTP/HTTPS, TCP/IP
- **图像格式**: JPEG
- **数据格式**: JSON
- **文件系统**: SPIFFS
- **实时系统**: FreeRTOS

## 🚀 快速开始

### 环境要求

- ESP-IDF v5.4.3
- ESP32-S3 开发板
- OV2640 摄像头模块
- 支持的编译器: xtensa-esp32s3-elf-gcc v12.2.0+

### 编译步骤

```bash
# 1. 设置 ESP-IDF 环境变量
. $HOME/esp/esp-idf/export.sh

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

在 `menuconfig` 中可配置以下选项：
- 摄像头类型 (OV2640)
- WiFi SSID 和密码
- 设备名称
- 服务器地址
- 运动检测阈值
- JPEG 质量
- 时区设置

## 🔧 基本使用

### Web 界面访问

1. 连接 WiFi 网络（STA 模式）
2. 访问摄像头 IP 地址（默认: 192.168.4.1）
3. 浏览器打开即可查看实时视频流

### 主要功能

1. **实时预览**: `/preview.html` - MJPEG 实时视频流
2. **配置管理**: `/config.html` - Web 配置界面
3. **状态监控**: `/api/status` - JSON 系统状态
4. **图像捕获**: `/capture` - 单帧捕获并上传

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
luatos-esp32s3-a10-base/
├── main/
│   ├── main.c              # 系统入口
│   └── CMakeLists.txt      # 组件注册
├── demo/main/main.lua      # LuatOS 备用方案
├── components/
│   ├── camera_driver/      # 摄像头驱动
│   ├── config_manager/     # 配置管理
│   ├── health_monitor/     # 健康监控
│   ├── mjpeg_streamer/    # MJPEG 流媒体
│   ├── motion_detect/      # 运动检测
│   ├── status_led/        # LED 状态指示
│   ├── time_sync/         # 时间同步
│   ├── web_server/        # Web 服务器
│   └── wifi_manager/      # WiFi 管理
├── docs/zh-CN/            # 中文文档
└── CMakeLists.txt          # 项目配置
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