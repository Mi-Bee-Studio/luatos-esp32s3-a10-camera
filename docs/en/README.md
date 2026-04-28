# MiBeeCam ESP32-S3-A10 Smart Camera

A smart camera system based on ESP32-S3 with OV2640 sensor, featuring WiFi connectivity, motion detection, live streaming, and remote image upload capabilities.

## Overview

MiBeeCam is a compact, intelligent camera system built with ESP32-S3 and OV2640 sensor. It provides comprehensive camera functionality including live MJPEG streaming, motion detection, WiFi connectivity, and remote image upload capabilities. The system is designed for IoT applications requiring intelligent video surveillance and monitoring.
<p align="center">
    <img src="../images/index-dashboard.png" alt="MiBeeCam Dashboard" width="640">
</p>

## Key Features

- **High-quality imaging**: OV2640 camera sensor with VGA resolution (640x480)
- **Live streaming**: MJPEG video stream up to 15 FPS
- **Motion detection**: Intelligent motion detection with configurable sensitivity
- **WiFi connectivity**: STA mode preferred with AP fallback
- **Remote upload**: Automatic image upload to configurable server
- **Web interface**: Built-in web dashboard for configuration and monitoring
- **Real-time metrics**: Prometheus-compatible system metrics
- **Compact design**: Optimized for embedded deployment

## Quick Start

### Hardware Requirements

- ESP32-S3 development board
- OV2640 camera module (8225N/GC2053)
- MicroSD card (optional for additional storage)

### Software Requirements

- ESP-IDF v5.4.3
- ESP32-S3 target support
- ESP32 camera driver component

### Build and Flash

```bash
# Set target
idf.py set-target esp32s3

# Configure project
idf.py menuconfig

# Build the project
idf.py build

# Flash to device
idf.py -p COMx flash monitor
```

## Documentation

### Core Documentation

- [Hardware Specification](hardware.md) - Detailed hardware specifications and pin mappings
- [Software Architecture](software.md) - System architecture, modules, and boot sequence
- [API Reference](api.md) - Complete API documentation for web endpoints
- [Troubleshooting Guide](troubleshooting.md) - Common issues and solutions

### Quick Configuration

### WiFi Setup

Configure WiFi credentials in `main/main.c`:

```c
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASS "your-wifi-password"
```

### Server Configuration

Set image upload server:

```c
#define SERVER_URL "https://your-server.com/upload"
#define DEVICE_ID "camera-device-001"
```

### Motion Detection

Adjust motion sensitivity:

```c
#define MOTION_THRESHOLD 5        // Sensitivity (1-100)
#define MOTION_COOLDOWN 10       // Cooldown in seconds
```

## Project Structure

```
luatos-esp32s3-a10-camera/
├── main/
│   ├── main.c              # System entry and startup
│   └── CMakeLists.txt      # Component registration
├── camera_driver/          # OV2640 camera driver
├── config_manager/         # Configuration management
├── health_monitor/         # System health monitoring
├── mjpeg_streamer/         # MJPEG streaming service
├── motion_detect/          # Motion detection and upload
├── status_led/             # Status LED control
├── time_sync/             # NTP time synchronization
├── web_server/            # HTTP server and web interface
├── wifi_manager/           # WiFi connection management
├── cJSON/                  # JSON parser
├── main/web_ui/            # Web interface files
├── partitions.csv          # Flash partition table
├── CMakeLists.txt          # Project configuration
└── sdkconfig.defaults      # Default configuration
```

## Technical Specifications

- **Processor**: ESP32-S3 dual-core @ 240MHz
- **Memory**: 16MB Flash, 8MB PSRAM (disabled)
- **Camera**: OV2640 sensor (VGA 640x480)
- **WiFi**: 2.4GHz IEEE 802.11 b/g/n
- **HTTP Server**: Port 80, CORS enabled
- **Video Format**: MJPEG streaming
- **Frame Rate**: Up to 15 FPS
- **JPEG Quality**: Adjustable (1-63, default 12)

## License

MIT License - Copyright © Mi&Bee Studio

## Support

For technical support and feature requests, please refer to the [troubleshooting guide](troubleshooting.md) or create an issue in the project repository.