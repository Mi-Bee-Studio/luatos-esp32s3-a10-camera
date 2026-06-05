# MiBeeCam — ESP32-S3-A10 Smart Camera

<p align="center">
  <img src="https://img.shields.io/github/actions/workflow/status/Mi-Bee-Studio/luatos-esp32s3-a10-camera/build.yml?branch=main&style=for-the-badge&logo=githubactions&logoColor=white" alt="Build Status">
  <img src="https://img.shields.io/github/license/Mi-Bee-Studio/luatos-esp32s3-a10-camera?style=for-the-badge&color=blue" alt="License">
  <img src="https://img.shields.io/badge/ESP--IDF-v5.5.4-00A3E0?style=for-the-badge" alt="ESP-IDF">
  <img src="https://img.shields.io/badge/platform-ESP32--S3-EA6726?style=for-the-badge" alt="Platform">
  <img src="https://img.shields.io/badge/language-C-00599C?style=for-the-badge&logo=c&logoColor=white" alt="Language">
  <img src="https://img.shields.io/github/v/release/Mi-Bee-Studio/luatos-esp32s3-a10-camera?style=for-the-badge&color=brightgreen" alt="Release">
</p>

<p align="center">
  <em>Compact, WiFi-enabled smart camera with real-time MJPEG streaming, motion detection, and web-based configuration — all running on a single ESP32-S3 chip.</em>
</p>

<p align="center">
  <a href="README_CN.md">中文文档</a> · <a href="https://github.com/Mi-Bee-Studio/luatos-esp32s3-a10-camera/issues">Report Bug</a> · <a href="https://github.com/Mi-Bee-Studio/luatos-esp32s3-a10-camera/issues">Request Feature</a>
</p>

---

## 📋 Overview

<p align="center">
  <img src="docs/images/luatos-esp32s3-a10.png" alt="MiBeeCam ESP32-S3-A10" width="480">
</p>

MiBeeCam is an open-source smart camera firmware for the **LuatOS ESP32-S3-A10** development board with an **OV2640 (8225N)** image sensor. Built entirely on **ESP-IDF v5.5.4**, it packs a complete surveillance system into a single firmware image:

- 🖥️ **Built-in web UI** for live viewing and configuration
- 📡 **MJPEG video streaming** at up to 15 FPS
- 🚨 **Motion detection** with automatic image upload
- ⚙️ **NVS-persisted settings** managed via browser or REST API
- 📊 **Prometheus-compatible metrics** for health monitoring

No cloud dependencies. No subscription. Just a WiFi camera that works on your LAN.

---

## ✨ Features

| Icon | Feature | Description |
|------|---------|-------------|
| 📷 | **Camera Capture** | OV2640 sensor (8225N), default VGA (640×480), supports SVGA/XGA/UXGA, JPEG output |
| 🌐 | **WiFi Management** | STA mode with auto-reconnect, AP hotspot fallback for first-time setup |
| 🎨 | **Web Interface** | Dashboard, live MJPEG preview, and configuration pages served from SPIFFS |
| 📺 | **MJPEG Streaming** | Real-time video at up to 15 FPS via `/stream` endpoint, up to 2 concurrent clients |

<p align="center">
  <img src="docs/images/index-dashboard.png" alt="MiBeeCam Dashboard" width="640">
</p>

| Icon | Feature | Description |
|------|---------|-------------|
| 🚨 | **Motion Detection** | Frame-difference algorithm with configurable threshold and cooldown |
| ☁️ | **Remote Upload** | Automatic JPEG upload on motion trigger, 3 retries with 2 s interval |
| ⚙️ | **Configuration** | NVS-persisted settings managed through web UI or REST API |
| ❤️ | **Health Monitoring** | Prometheus-compatible `/metrics` endpoint, 60 s collection interval |
| 💡 | **Status LED** | GPIO 10 LED indicates startup / WiFi connecting / running / error / AP mode |
| 🕐 | **Time Sync** | NTP via `pool.ntp.org`, configurable timezone with POSIX format |

---

## 🚀 Quick Start

### Prerequisites

- **ESP-IDF v5.5.4** (do not use v6.0 — known PSRAM issues with this board)
- **Python 3.8+**
- **esptool.py**
- Dependency: `espressif/esp32-camera ^2.0.0` (resolved via `idf_component.yml`)

### 1. Build

```bash
idf.py set-target esp32s3
idf.py build
```

### 2. Flash Firmware

```bash
idf.py -p COMx flash monitor
```

Replace `COMx` with your serial port (e.g. `COM3` on Windows, `/dev/ttyUSB0` on Linux).

### 3. Flash SPIFFS (Web UI)

The web UI files must be flashed separately on first setup:

```bash
# Generate SPIFFS image
python $IDF_PATH/components/spiffs/spiffsgen.py 0x3CE000 main/web_ui build/spiffs.bin

# Flash to SPIFFS partition
python -m esptool --chip esp32s3 -p COMx write_flash 0x392000 build/spiffs.bin
```

### 4. First-Time Setup (AP Mode)

On first boot the device enters AP mode because no WiFi credentials are stored:

1. Connect to WiFi network **MiBeeCam** (password: `12345678`)
2. Open **http://192.168.4.1** in a browser
3. Go to the configuration page and enter your WiFi SSID and password
4. Save — the device reboots and connects in STA mode

### 5. Factory Reset

Hold the **BOOT** button (GPIO 0) for **5 seconds**. This clears NVS config and reboots into AP mode.

---

## 🏗️ Project Structure

```
luatos-esp32s3-a10-camera/
├── main/
│   ├── main.c              # System entry, 14-step startup sequence
│   ├── camera_driver.c/h   # OV2640 init, frame capture, resolution control
│   ├── config_manager.c/h  # NVS config storage, v1→v2 auto-migration
│   ├── health_monitor.c/h  # Heap/task monitoring, Prometheus metrics
│   ├── mjpeg_streamer.c/h  # MJPEG multipart/x-mixed-replace stream
│   ├── motion_detect.c/h   # Frame-difference detection + upload
│   ├── status_led.c/h      # GPIO 10 LED status indicator
│   ├── time_sync.c/h       # NTP time synchronization
│   ├── web_server.c/h      # HTTP server (port 80), REST API, SPIFFS
│   ├── wifi_manager.c/h    # WiFi STA/AP management
│   ├── cJSON.c/h           # JSON parser
│   └── web_ui/             # SPIFFS static files
│       ├── index.html      # Dashboard
│       ├── preview.html    # Live preview
│       └── config.html     # Configuration
├── .github/workflows/
│   └── build.yml           # CI/CD — build + auto-release on tag push
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
└── docs/                   # Bilingual documentation (en/ + zh-CN/)
```

---

## 🔌 Hardware Specifications

### Board & Sensor

| Item | Specification |
|------|---------------|
| **Board** | LuatOS ESP32-S3-A10 |
| **Camera** | OV2640 (8225N module) |
| **Flash** | 16 MB |
| **PSRAM** | 8 MB Octal (physically present, **disabled** in firmware — timing tuning failed) |
| **CPU** | ESP32-S3 dual-core 240 MHz |
| **Connectivity** | WiFi 2.4 GHz (802.11 b/g/n) |
| **Power** | 5 V / 2 A via USB-C |

### GPIO Pin Mapping

| Pin | GPIO | Function |
|-----|------|----------|
| XCLK | 39 | Master clock |
| SIOD | 21 | I2C data |
| SIOC | 46 | I2C clock |
| D0–D7 | 34, 47, 48, 33, 35, 37, 38, 40 | Parallel data |
| VSYNC | 42 | Vertical sync |
| HREF | 41 | Horizontal reference |
| PCLK | 36 | Pixel clock |
| PWDN | −1 | Disabled |
| RESET | −1 | Disabled |

> **Note** — These pins use the `CAMERA_MODEL_Air_ESP32S3` definition from the `esp32-camera` component, **not** the LuatOS documentation pinout.

### Memory Partition

| Partition | Type | Offset | Size | Content |
|-----------|------|--------|------|---------|
| nvs | data/nvs | 0x9000 | 24 KB | Device config (`device_cfg/config`) |
| phy_init | data/phy | 0xf000 | 4 KB | WiFi PHY calibration |
| factory | app/factory | 0x10000 | 3.5 MB | Application firmware |
| otadata | data/ota | 0x390000 | 8 KB | OTA metadata |
| spiffs | data/spiffs | 0x392000 | ~3.94 MB | Web UI files |

---

## 📡 REST API Reference

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Dashboard (SPIFFS index.html) |
| GET | `/preview.html` | Live MJPEG preview |
| GET | `/config.html` | Configuration page |
| GET | `/stream` | MJPEG video stream (multipart/x-mixed-replace) |
| GET | `/capture` | Single JPEG capture + upload |
| GET | `/api/status` | JSON system status (WiFi, camera, uptime, chip temp) |
| GET | `/api/config` | JSON current configuration (password masked) |
| POST | `/api/config` | Partial JSON update |
| POST | `/api/reboot` | Reboot device |

### Examples

```bash
# Get system status
curl http://192.168.1.100/api/status

# Update WiFi configuration
curl -X POST http://192.168.1.100/api/config \
  -H "Content-Type: application/json" \
  -d '{"wifi_ssid":"MyNetwork","wifi_pass":"MyPassword"}'

# Reboot the device
curl -X POST http://192.168.1.100/api/reboot
```

---

## ⚙️ Configuration Parameters

All settings are stored in NVS and managed via the web UI or REST API:

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| Resolution | VGA (0) | 0=VGA, 1=SVGA, 2=XGA, 3=UXGA | Camera output resolution |
| FPS | 15 | 1–30 | Target frames per second |
| JPEG Quality | 12 | 1–63 (lower = better) | JPEG compression quality |
| Motion Threshold | 5 | 1–255 | Sensitivity of motion detection |
| Motion Cooldown | 10 s | 1–255 | Min seconds between triggers |
| Device Name | MiBeeCam | string (32 chars) | Device display name |
| Timezone | CST-8 | POSIX timezone string | Timezone for NTP sync |

---

## 🔄 Boot Sequence

The firmware follows a carefully ordered 14-step initialization:

```
 NVS Init  →  Config Load  →  LED Init  →  SPIFFS Mount  →  Camera Init
     ↓
 WiFi Init  →  Health Monitor  →  Mode Selection
     ↓
 [STA Mode] WiFi Connect → Streamer → Web Server → NTP → Motion → Button
 [AP Mode]  Web Server Only
```

> **Camera must initialize before WiFi** — the I2C bus conflicts if WiFi subsystem grabs it first.

Detailed steps:

| # | Step | Description |
|---|------|-------------|
| 1 | NVS flash init | Initialize Non-Volatile Storage |
| 2 | Config load | Load with v1→v2 auto-migration |
| 3 | LED init | GPIO 10 status LED |
| 4 | SPIFFS mount | Mount web UI filesystem |
| 5 | Camera init | **Before WiFi** (I2C conflict avoidance) |
| 6 | WiFi subsystem init | Initialize WiFi driver |
| 7 | Health monitor init | Start health monitoring |
| 8 | Mode selection | STA if WiFi configured, AP otherwise |
| 9 | WiFi connect / AP start | Connect to router or start hotspot |
| 10 | MJPEG streamer init | Start streaming (STA only) |
| 11 | Web server start | HTTP server on port 80 |
| 12 | NTP time sync | Time synchronization (STA only) |
| 13 | Motion detection start | Frame-difference monitoring (STA only) |
| 14 | BOOT button monitor | 5 s hold = factory reset |

---

## 🐛 Troubleshooting

| Problem | Cause | Solution |
|---------|-------|----------|
| Camera init fails | Wrong pin mapping | Verify 8225N module pins match `camera_driver.c` |
| Boot loop | PSRAM timing failure | PSRAM must stay disabled in `sdkconfig` |
| WiFi won't connect | 5 GHz network | ESP32-S3 only supports 2.4 GHz |
| `/stream` returns 404 | Wildcard URI handler intercepts | Register `/stream` before `/*` handler |
| SPIFFS mount fails | Partition mismatch | Ensure SPIFFS offset (0x392000) matches `partitions.csv` |
| Web UI shows garbled text | Unicode escapes in HTML | Replace `\uXXXX` with actual UTF-8 characters |
| False motion triggers | Threshold too low | Increase motion threshold in web UI config |
| Preview freezes <1 s | Frame buffer contention (`fb_count=1`) | Use sample-and-release pattern; pause motion detection during streaming |

See [docs/en/troubleshooting.md](docs/en/troubleshooting.md) for detailed solutions.

---

## 📄 License

Distributed under the **MIT License**. See [LICENSE](LICENSE) for more information.

Copyright © 2026 [Mi&Bee Studio](https://github.com/Mi-Bee-Studio)
