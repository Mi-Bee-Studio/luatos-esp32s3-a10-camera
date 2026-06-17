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
- **mDNS discovery**: Access device via http://mibee.local
- **WebSocket real-time push**: Real-time event push to web UI
- **Webhook client**: Forward events to external HTTP endpoint
- **ONVIF Profile S**: WS-Discovery discovery + SOAP service
- **Backup SSID**: Auto-fallback to secondary WiFi on primary failure
- **WiFi scan**: Scan nearby networks via REST API
- **AT command interface**: 20 commands via UART0 serial

## Quick Start

### Hardware Requirements

- ESP32-S3 development board
- OV2640 camera module (8225N/GC2053)

### Software Requirements

- ESP-IDF v5.5.4
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

WiFi credentials are configured at runtime (no code changes needed):

1. **First boot**: Device enters AP mode (SSID: MiBeeCam, password: 12345678)
2. **Web UI**: Open http://192.168.4.1, enter WiFi credentials on config page
3. **AT Commands**: Use `AT+CWJAP=<ssid>,<pwd>` via UART0 serial (115200 baud)
4. **Backup WiFi**: Use `AT+CWJAP2=<ssid>,<pwd>` for fallback SSID

### Server Configuration

Configure image upload server via Web UI or AT commands:
- **Web UI**: Set server URL on config page
- **AT Commands**: `AT+CFGSET=server_url,http://your-server.com/upload`

### Motion Detection

Adjust motion sensitivity via Web UI or AT commands:
- **Web UI**: Set threshold and cooldown on config page
- **AT Commands**: `AT+CFGSET=motion_threshold,5` and `AT+CFGSET=motion_cooldown,10`

## Project Structure

```
luatos-esp32s3-a10-camera/
├── main/
│   ├── main.c              # System entry, 15-step startup
│   ├── at_command.c/h      # UART0 AT command interface (20 commands)
│   ├── camera_driver.c/h   # OV2640 camera driver
│   ├── config_manager.c/h  # NVS config storage
│   ├── event_bus.c/h       # In-memory pub/sub event bus
│   ├── frame_broadcaster.c/h # DRAM frame cache
│   ├── health_monitor.c/h  # Health monitoring + Prometheus
│   ├── motion_detect.c/h   # Motion detection
│   ├── mjpeg_streamer.c/h  # MJPEG streaming
│   ├── onvif_discovery.c/h # ONVIF WS-Discovery
│   ├── onvif_service.c/h   # ONVIF SOAP service
│   ├── status_led.c/h      # Status LED control
│   ├── time_sync.c/h       # NTP time sync
│   ├── web_server.c/h      # HTTP server + REST API
│   ├── webhook.c/h         # Webhook event forwarding
│   ├── wifi_manager.c/h    # WiFi STA/AP management
│   ├── cJSON.c/h           # JSON parser (third-party)
│   ├── web_ui/             # SPIFFS static files
│   └── CMakeLists.txt      # Component registration
├── docs/                   # Bilingual documentation (en/ + zh-CN/)
├── .github/workflows/      # CI/CD
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
└── idf_component.yml       # ESP-IDF component dependencies
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