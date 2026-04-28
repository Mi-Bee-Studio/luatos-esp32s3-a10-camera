# MiBeeCam - ESP32-S3-A10 Smart Camera

[中文文档](README_CN.md)

## Overview

MiBeeCam is a smart camera system built on ESP-IDF v5.4.3 for the ESP32-S3-A10 development board with an OV2640 image sensor (8225N module). It features a built-in web UI for real-time monitoring, MJPEG video streaming, frame-difference motion detection with automatic image upload, and a complete configuration management system — all in a single firmware.
<p align="center">
    <img src="docs/images/luatos-esp32s3-a10.png" alt="MiBeeCam ESP32-S3-A10" width="480">
</p>

## Features

- **Camera Capture** — OV2640 sensor (8225N), default VGA (640×480), supports SVGA / XGA / UXGA, JPEG output
- **WiFi Management** — STA mode with auto-reconnect, AP hotspot fallback for first-time setup
- **Web Interface** — Dashboard, live MJPEG preview, and configuration pages served from SPIFFS
- **MJPEG Streaming** — Real-time video at up to 15 FPS via `/stream` endpoint
<p align="center">
    <img src="docs/images/index-dashboard.png" alt="MiBeeCam Dashboard" width="640">
</p>
- **Motion Detection** — Frame-difference algorithm with configurable threshold and cooldown
- **Remote Upload** — Automatic JPEG upload on motion trigger, 3 retries with 2 s interval
- **Configuration** — NVS-persisted settings managed through web UI or REST API
- **Health Monitoring** — Prometheus-compatible `/metrics` endpoint, 60 s interval
- **Status LED** — GPIO 10 LED indicates startup / WiFi connecting / running / error / AP mode
- **Time Sync** — NTP via `pool.ntp.org`, configurable timezone

## Project Structure

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

## Hardware

### Requirements

| Item | Specification |
|------|---------------|
| Board | LuatOS ESP32-S3-A10 |
| Camera | OV2640 (8225N module) |
| Flash | 16 MB |
| PSRAM | 8 MB Octal (physically present, **disabled** in firmware — timing tuning failed) |
| CPU | ESP32-S3 dual-core 240 MHz |
| Connectivity | WiFi 2.4 GHz (802.11 b/g/n) |
| Power | 5 V / 2 A via USB-C |

### Camera Pin Mapping

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

> **Note** — These pins come from the `CAMERA_MODEL_Air_ESP32S3` definition in the `esp32-camera` component, not from LuatOS documentation.

### Partition Table

| Partition | Type | Offset | Size |
|-----------|------|--------|------|
| nvs | data/nvs | 0x9000 | 24 KB |
| phy_init | data/phy | 0xf000 | 4 KB |
| factory | app/factory | 0x10000 | 3.5 MB |
| otadata | data/ota | 0x390000 | 8 KB |
| spiffs | data/spiffs | 0x392000 | ~3.94 MB |

## Software Requirements

- **ESP-IDF v5.4.3** — do not use v6.0 (known PSRAM issues with this board)
- **Python 3.8+**
- **esptool.py**
- **Dependency**: `espressif/esp32-camera ^2.0.0` (resolved via `idf_component.yml`)

## Quick Start

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

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Dashboard (SPIFFS index.html) |
| GET | `/preview.html` | Live MJPEG preview |
| GET | `/config.html` | Configuration page |
| GET | `/stream` | MJPEG video stream |
| GET | `/capture` | Single JPEG capture + upload |
| GET | `/api/status` | JSON system status |
| GET | `/api/config` | JSON current configuration |
| POST | `/api/config` | Update configuration |
| POST | `/api/reboot` | Reboot device |
| GET | `/metrics` | Prometheus-compatible metrics |

### Examples

```bash
# System status
curl http://192.168.1.100/api/status

# Update WiFi config
curl -X POST http://192.168.1.100/api/config \
  -H "Content-Type: application/json" \
  -d '{"wifi_ssid":"MyNetwork","wifi_pass":"MyPassword"}'

# Reboot
curl -X POST http://192.168.1.100/api/reboot
```

## Configuration

All settings are stored in NVS and managed via the web UI or REST API:

| Parameter | Default | Range |
|-----------|---------|-------|
| Resolution | VGA (0) | 0=VGA, 1=SVGA, 2=XGA, 3=UXGA |
| FPS | 15 | 1–30 |
| JPEG Quality | 12 | 1–63 (lower = better) |
| Motion Threshold | 5 | 1–255 |
| Motion Cooldown | 10 s | 1–255 |
| Device Name | MiBeeCam | string (32 chars) |
| Timezone | CST-8 | POSIX timezone string |

## Boot Sequence

The firmware follows a 14-step initialization:

1. NVS flash init
2. Config load (with v1→v2 auto-migration)
3. LED init (GPIO 10)
4. SPIFFS mount
5. Camera init (**before** WiFi to avoid I2C conflict)
6. WiFi subsystem init
7. Health monitor init
8. Mode selection (STA if WiFi configured, AP otherwise)
9. STA: WiFi connect / AP: start hotspot
10. MJPEG streamer init (STA mode)
11. Web server start (STA or AP)
12. NTP time sync (STA mode)
13. Motion detection start (STA mode)
14. BOOT button monitor (5 s hold = factory reset)

## Troubleshooting

| Problem | Cause | Solution |
|---------|-------|----------|
| Camera init fails | Wrong pin mapping | Verify 8225N module pins match `camera_driver.c` |
| Boot loop | PSRAM timing failure | PSRAM must stay disabled in `sdkconfig` |
| WiFi won't connect | 5 GHz network | ESP32-S3 only supports 2.4 GHz |
| /stream returns 404 | Wildcard URI handler intercepts | Register `/stream` before `/*` handler |
| SPIFFS mount fails | Partition mismatch | Ensure SPIFFS offset (0x392000) matches `partitions.csv` |
| Web UI shows garbled text | Unicode escapes in HTML | Replace `\uXXXX` with actual UTF-8 characters |
| False motion triggers | Threshold too low | Increase motion threshold in web UI config |

See [docs/en/troubleshooting.md](docs/en/troubleshooting.md) for detailed solutions.

## License

MIT License — Mi&Bee Studio
