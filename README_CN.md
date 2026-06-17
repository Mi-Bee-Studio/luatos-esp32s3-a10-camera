# MiBeeCam — ESP32-S3-A10 智能摄像头

<p align="center">
  <img src="https://img.shields.io/github/actions/workflow/status/Mi-Bee-Studio/luatos-esp32s3-a10-camera/build.yml?branch=main&style=for-the-badge&logo=githubactions&logoColor=white" alt="构建状态">
  <img src="https://img.shields.io/github/license/Mi-Bee-Studio/luatos-esp32s3-a10-camera?style=for-the-badge&color=blue" alt="许可证">
  <img src="https://img.shields.io/badge/ESP--IDF-v5.5.4-00A3E0?style=for-the-badge" alt="ESP-IDF">
  <img src="https://img.shields.io/badge/platform-ESP32--S3-EA6726?style=for-the-badge" alt="平台">
  <img src="https://img.shields.io/badge/language-C-00599C?style=for-the-badge&logo=c&logoColor=white" alt="语言">
  <img src="https://img.shields.io/github/v/release/Mi-Bee-Studio/luatos-esp32s3-a10-camera?style=for-the-badge&color=brightgreen" alt="发布版本">
</p>

<p align="center">
  <em>功能紧凑、支持 WiFi 的智能摄像头固件，具备实时 MJPEG 视频流、运动检测和 Web 配置界面 — 全部运行在单颗 ESP32-S3 芯片上。</em>
</p>

<p align="center">
  <a href="README.md">English</a> · <a href="https://github.com/Mi-Bee-Studio/luatos-esp32s3-a10-camera/issues">报告问题</a> · <a href="https://github.com/Mi-Bee-Studio/luatos-esp32s3-a10-camera/issues">请求功能</a>
</p>

---

## 📋 概述

<p align="center">
  <img src="docs/images/luatos-esp32s3-a10.png" alt="MiBeeCam ESP32-S3-A10" width="480">
</p>

MiBeeCam 是专为 **LuatOS ESP32-S3-A10** 开发板和 **OV2640（8225N）** 图像传感器设计的开源智能摄像头固件。基于 **ESP-IDF v5.5.4** 构建，将完整的监控系统打包在单个固件镜像中：

- 🖥️ **内置 Web UI**，支持实时预览和配置
- 📡 **MJPEG 视频流**，最高 15 FPS
- 🚨 **运动检测**，支持自动图像上传
- ⚙️ **NVS 持久化设置**，通过浏览器或 REST API 管理
- 📊 **兼容 Prometheus 的指标**，用于健康监控
- 💻 **AT 指令接口**，支持无头 WiFi 配置

无需云服务，无需订阅。一个可以在局域网中独立工作的 WiFi 摄像头。

---

## ✨ 功能特性

| 图标 | 功能 | 描述 |
|------|------|------|
| 📷 | **摄像头捕获** | OV2640 传感器（8225N），默认 VGA（640×480），支持 SVGA/XGA/UXGA，JPEG 输出 |
| 🌐 | **WiFi 管理** | STA 模式自动重连，首次配置 AP 热点备用 |
| 🎨 | **Web 界面** | 仪表盘、实时 MJPEG 预览和配置页面，通过 SPIFFS 提供 |
| 📺 | **MJPEG 流媒体** | 通过 `/stream` 端点最高 15 FPS 实时视频，支持 2 个并发客户端 |
| 🌐 | **mDNS 发现** | 通过 `http://mibee.local` 访问设备，可配置主机名，_http._tcp 服务广播 |
| 📶 | **WiFi 扫描** | 通过 REST API 和设置向导扫描附近网络 |
| 🔄 | **WebSocket 推送** | 实时事件推送到 Web UI，9 种事件类型，最多 5 个客户端 |
| 🔗 | **Webhook 客户端** | 将事件转发到外部 HTTP 端点，JSON 负载，基于队列的异步发送 |
| 🔀 | **Backup SSID** | 主 WiFi 失败时自动回退到备用网络（3 次重试） |

<p align="center">
  <img src="docs/images/index-dashboard.png" alt="MiBeeCam 仪表盘" width="640">
</p>

| 图标 | 功能 | 描述 |
|------|------|------|
| 🚨 | **运动检测** | 帧差算法，可配置阈值和冷却时间 |
| ☁️ | **远程上传** | 运动触发时自动 JPEG 上传，3 次重试间隔 2 秒 |
| ⚙️ | **配置管理** | NVS 持久化设置，通过 Web UI 或 REST API 管理 |
| ❤️ | **健康监控** | 兼容 Prometheus 的 `/metrics` 端点，60 秒采集间隔 |
| 💡 | **状态 LED** | GPIO 10 LED 显示启动 / WiFi 连接中 / 运行中 / 错误 / AP 模式 |
| 🕐 | **时间同步** | 通过 `pool.ntp.org` 的 NTP，可配置 POSIX 时区 |
| 📡 | **ONVIF Profile S** | WS-Discovery + SOAP 设备/媒体服务（默认禁用） |
| 🔄 | **事件总线** | 内存发布/订阅，用于模块间通信，9 种事件类型 |
| 🖼️ | **帧广播器** | DRAM 帧缓存，带引用计数（运动检测 + 流媒体） |
| 💻 | **AT 指令** | UART0 串口 AT 指令接口，20 条指令，用于 WiFi 配置、系统信息、摄像头控制 |

---

## 🚀 快速开始

### 环境要求

- **ESP-IDF v5.5.4**（不要使用 v6.0 — 此开发板存在已知 PSRAM 问题）
- **Python 3.8+**
- **esptool.py**
- 依赖：`espressif/esp32-camera ^2.0.0`（通过 `idf_component.yml` 自动解析）

### 1. 构建

```bash
idf.py set-target esp32s3
idf.py build
```

### 2. 烧录固件

```bash
idf.py -p COMx flash monitor
```

将 `COMx` 替换为您的串口（例如 Windows 上的 `COM3`，Linux 上的 `/dev/ttyUSB0`）。

### 3. 烧录 SPIFFS（Web UI）

Web UI 文件必须在首次设置时单独烧录：

```bash
# 生成 SPIFFS 镜像
python $IDF_PATH/components/spiffs/spiffsgen.py 0x3CE000 main/web_ui build/spiffs.bin

# 烧录到 SPIFFS 分区
python -m esptool --chip esp32s3 -p COMx write_flash 0x392000 build/spiffs.bin
```

### 4. 首次设置（AP 模式）

首次启动时，设备因为没有存储 WiFi 凭证而进入 AP 模式：

1. 连接 WiFi 网络 **MiBeeCam**（密码：`12345678`）
2. 在浏览器中打开 **http://192.168.4.1**
3. 进入配置页面，输入您的 WiFi 名称和密码
4. 保存 — 设备重启并在 STA 模式下连接

### 5. 恢复出厂设置

按住 **BOOT** 按钮（GPIO 0）**5 秒**。这将清除 NVS 配置并重启进入 AP 模式。

---

## 🏗️ 项目结构

```
luatos-esp32s3-a10-camera/
├── main/
│   ├── main.c              # 系统入口，15 步启动序列
│   ├── at_command.c/h      # UART0 串口 AT 指令接口（20 条指令）
│   ├── camera_driver.c/h   # OV2640 初始化，帧捕获，分辨率控制
│   ├── config_manager.c/h  # NVS 配置存储，v1→v2 自动迁移
│   ├── event_bus.c/h       # 内存发布/订阅，用于模块间通信
│   ├── frame_broadcaster.c/h # DRAM 帧缓存，带引用计数
│   ├── health_monitor.c/h  # 堆内存/任务监控，Prometheus 指标
│   ├── mjpeg_streamer.c/h  # MJPEG multipart/x-mixed-replace 流
│   ├── motion_detect.c/h   # 帧差检测 + 上传
│   ├── onvif_discovery.c/h # WS-Discovery（条件编译）
│   ├── onvif_service.c/h   # SOAP 设备/媒体服务
│   ├── status_led.c/h      # GPIO 10 LED 状态指示器
│   ├── time_sync.c/h       # NTP 时间同步
│   ├── web_server.c/h      # HTTP 服务器（端口 80），REST API，SPIFFS
│   ├── webhook.c/h         # HTTP 事件转发（条件编译）
│   ├── wifi_manager.c/h    # WiFi STA/AP 管理
│   ├── cJSON.c/h           # JSON 解析器
│   └── web_ui/             # SPIFFS 静态文件
│       ├── index.html      # 仪表盘
│       ├── preview.html    # 实时预览
│       └── config.html     # 配置页面
├── .github/workflows/
│   └── build.yml           # CI/CD — 构建 + 标签推送时自动发布
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
└── docs/
    ├── en/                 # 英文文档
    └── zh-CN/              # 中文文档

---

## 🔌 硬件规格

### 开发板与传感器

| 项目 | 规格 |
|------|------|
| **开发板** | LuatOS ESP32-S3-A10 |
| **摄像头** | OV2640（8225N 模块） |
| **闪存** | 16 MB |
| **PSRAM** | 8 MB 八线制（物理存在，**在固件中禁用** — 时序调谐失败） |
| **CPU** | ESP32-S3 双核 240 MHz |
| **连接** | WiFi 2.4 GHz（802.11 b/g/n） |
| **电源** | 5 V / 2 A 通过 USB-C |

### GPIO 引脚映射

| 引脚 | GPIO | 功能 |
|------|------|------|
| XCLK | 39 | 主时钟 |
| SIOD | 21 | I2C 数据 |
| SIOC | 46 | I2C 时钟 |
| D0–D7 | 34, 47, 48, 33, 35, 37, 38, 40 | 并行数据 |
| VSYNC | 42 | 垂直同步 |
| HREF | 41 | 水平参考 |
| PCLK | 36 | 像素时钟 |
| PWDN | −1 | 已禁用 |
| RESET | −1 | 已禁用 |

> **注意** — 这些引脚使用 `esp32-camera` 组件中的 `CAMERA_MODEL_Air_ESP32S3` 定义，**不是**来自 LuatOS 文档的引脚映射。

### 内存分区

| 分区 | 类型 | 偏移 | 大小 | 内容 |
|------|------|------|------|------|
| nvs | data/nvs | 0x9000 | 24 KB | 设备配置（`device_cfg/config`） |
| phy_init | data/phy | 0xf000 | 4 KB | WiFi PHY 校准 |
| factory | app/factory | 0x10000 | 3.5 MB | 应用程序固件 |
| otadata | data/ota | 0x390000 | 8 KB | OTA 元数据 |
| spiffs | data/spiffs | 0x392000 | ~3.94 MB | Web UI 文件 |

---

## 📡 REST API 参考

| 方法 | 路径 | 描述 |
|------|------|------|
| GET | `/` | 仪表盘（SPIFFS index.html） |
| GET | `/preview.html` | 实时 MJPEG 预览 |
| GET | `/config.html` | 配置页面 |
| GET | `/stream` | MJPEG 视频流（multipart/x-mixed-replace） |
| GET | `/capture` | 单次 JPEG 捕获 + 上传 |
| GET | `/api/status` | JSON 系统状态（WiFi、摄像头、运行时间、芯片温度） |
| GET | `/api/config` | JSON 当前配置（密码已屏蔽） |
| POST | `/api/config` | 部分 JSON 更新 |
| POST | `/api/reboot` | 重启设备 |

### 示例

```bash
# 获取系统状态
curl http://192.168.1.100/api/status

# 更新 WiFi 配置
curl -X POST http://192.168.1.100/api/config \
  -H "Content-Type: application/json" \
  -d '{"wifi_ssid":"MyNetwork","wifi_pass":"MyPassword"}'

# 重启设备
curl -X POST http://192.168.1.100/api/reboot
```

---

## ⚙️ 配置参数

所有设置存储在 NVS 中，通过 Web UI 或 REST API 管理：

| 参数 | 默认值 | 范围 | 描述 |
|------|--------|------|------|
| 分辨率 | VGA (0) | 0=VGA, 1=SVGA, 2=XGA, 3=UXGA | 摄像头输出分辨率 |
| FPS | 15 | 1–30 | 目标帧率 |
| JPEG 质量 | 12 | 1–63（越低越好） | JPEG 压缩质量 |
| 运动阈值 | 5 | 1–255 | 运动检测灵敏度 |
| 运动冷却 | 10 秒 | 1–255 | 触发间隔最短秒数 |
| 设备名称 | MiBeeCam | 字符串（32 字符） | 设备显示名称 |
| 时区 | CST-8 | POSIX 时区字符串 | NTP 同步时区 |

---

## 🔄 启动序列

固件遵循精心排序的 15 步初始化流程：

```
 NVS 初始化 → 配置加载 → LED 初始化 → SPIFFS 挂载 → 摄像头初始化
     ↓
 WiFi 初始化 → 健康监控 → 模式选择
     ↓
 [STA 模式] WiFi 连接 → 流媒体 → Web 服务器 → NTP → 运动检测 → 按钮 → AT 指令
 [AP 模式] 仅 Web 服务器
```

> **摄像头必须在 WiFi 之前初始化** — 否则 I2C 总线会被 WiFi 子系统抢先占用。

详细步骤：

| # | 步骤 | 描述 |
|---|------|------|
| 1 | NVS 闪存初始化 | 初始化非易失性存储 |
| 2 | 配置加载 | 包含 v1→v2 自动迁移 |
| 3 | LED 初始化 | GPIO 10 状态 LED |
| 4 | SPIFFS 挂载 | 挂载 Web UI 文件系统 |
| 5 | 摄像头初始化 | **在 WiFi 之前**（避免 I2C 冲突） |
| 6 | WiFi 子系统初始化 | 初始化 WiFi 驱动 |
| 7 | 健康监控初始化 | 启动健康监控 |
| 8 | 模式选择 | 已配置 WiFi 则 STA，否则 AP |
| 9 | WiFi 连接 / AP 启动 | 连接路由器或启动热点 |
| 10 | MJPEG 流媒体初始化 | 启动流媒体（仅 STA） |
| 11 | Web 服务器启动 | HTTP 服务器监听端口 80 |
| 12 | NTP 时间同步 | 时间同步（仅 STA） |
| 13 | 运动检测启动 | 帧差监控（仅 STA） |
| 14 | BOOT 按钮监控 | 5 秒按住 = 恢复出厂设置 |
| 15 | AT 指令接口 | UART0 串口 AT 指令（20 条） |

---

## 🐛 故障排除

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| 摄像头初始化失败 | 引脚映射错误 | 验证 8225N 模块引脚与 `camera_driver.c` 匹配 |
| 启动循环 | PSRAM 时序失败 | PSRAM 必须在 `sdkconfig` 中保持禁用 |
| WiFi 无法连接 | 5 GHz 网络 | ESP32-S3 仅支持 2.4 GHz |
| `/stream` 返回 404 | 通配符 URI 处理器拦截 | 在 `/*` 处理器之前注册 `/stream` |
| SPIFFS 挂载失败 | 分区不匹配 | 确保 SPIFFS 偏移量（0x392000）与 `partitions.csv` 匹配 |
| Web UI 显示乱码 | HTML 中的 Unicode 转义 | 将 `\uXXXX` 替换为实际的 UTF-8 字符 |
| 误运动触发 | 阈值太低 | 在 Web UI 配置中提高运动阈值 |
| 预览不到 1 秒定格 | 帧缓冲区争用（`fb_count=1`） | 采用采样即释放模式；流传输时暂停运动检测 |

详细信息请参阅 [docs/zh-CN/troubleshooting.md](docs/zh-CN/troubleshooting.md)。

---

## 📄 许可证

基于 **MIT 许可证** 分发。详见 [LICENSE](LICENSE)。

版权所有 © 2026 [Mi&Bee Studio](https://github.com/Mi-Bee-Studio)
