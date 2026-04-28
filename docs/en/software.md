# Software Architecture - MiBeeCam ESP32-S3-A10

This document provides comprehensive information about the software architecture, modules, and system design of the MiBeeCam smart camera system.

## System Architecture Overview

The MiBeeCam software is designed as a real-time embedded system with multiple concurrent services. The architecture follows a modular approach with clear separation of concerns, utilizing FreeRTOS for multitasking and efficient resource management.

### Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                        │
├─────────────────────────────────────────────────────────────┤
│  Web Server  │ MJPEG Streamer  │ Motion Detection  │ API │
├─────────────────────────────────────────────────────────────┤
│                    Service Layer                            │
├─────────────────────────────────────────────────────────────┤
│  Time Sync  │ Health Monitor  │ Status LED  │ Config Manager│
├─────────────────────────────────────────────────────────────┤
│                    Hardware Layer                           │
├─────────────────────────────────────────────────────────────┤
│  Camera Driver  │ WiFi Manager  │ SPIFFS  │ NVS         │
├─────────────────────────────────────────────────────────────┤
│                    Platform Layer                           │
├─────────────────────────────────────────────────────────────┤
│                 FreeRTOS + ESP-IDF v5.4.3                   │
└─────────────────────────────────────────────────────────────┘
```

## Core System Modules

### 1. Main Module (`main.c`)
**File**: `main/main.c`  
**Lines**: 14-step startup sequence  
**Purpose**: System entry point and initialization orchestration

**Startup Sequence**:
1. NVS initialization
2. Configuration loading (v1→v2 auto-migration)
3. Status LED initialization (GPIO 10)
4. SPIFFS mounting
5. Camera initialization (BEFORE WiFi to avoid I2C conflict)
6. WiFi subsystem initialization
7. Health monitor initialization
8. WiFi mode selection (STA preferred, AP fallback)
9. WiFi connection (STA) or hotspot start (AP)
10. MJPEG streamer initialization (STA mode)
11. Web server startup
12. NTP time synchronization (STA mode)
13. Motion detection start (STA mode)
14. BOOT button monitoring

**Key Features**:
- 14-step boot sequence with proper error handling
- Camera initialization before WiFi to prevent I2C bus conflicts
- Comprehensive system health monitoring
- Graceful degradation to AP mode if WiFi fails

### 2. Camera Driver (`camera_driver.c/h`)
**File**: `camera_driver/camera_driver.c`  
**Purpose**: OV2640 camera initialization and frame capture

**Configuration**:
- **Default Resolution**: VGA (640x480) - NOT SVGA
- **Supported Resolutions**: VGA=0, SVGA=1, XGA=2, UXGA=3
- **Default Frame Rate**: 15 FPS (range 1-30)
- **Default JPEG Quality**: 12 (range 1-63, lower = better)
- **Frame Buffer**: CAMERA_FB_IN_DRAM, fb_count=1 (single buffer)
- **Camera Model**: CAMERA_MODEL_Air_ESP32S3

**Key Features**:
- XCLK generation using LEDC method (not GPIO matrix)
- Single frame buffer in DRAM (memory efficiency)
- JPEG compression with adjustable quality
- Comprehensive error handling

### 3. Configuration Manager (`config_manager.c/h`)
**File**: `config_manager/config_manager.c`  
**Purpose**: Configuration management with version migration

**Configuration Parameters**:
- `motion_threshold`: 5 (NOT 20, range 1-100)
- `motion_cooldown`: 10 seconds
- `timezone`: "CST-8"
- `device_name`: "MiBeeCam"
- `Config version`: v2, magic: 0xA5B6C7D8
- **Auto-migration**: v1 to v2 support

**Features**:
- NVS-based persistent storage
- Automatic configuration migration
- Versioned configuration system
- Default values with runtime overrides

### 4. Web Server (`web_server.c/h`)
**File**: `web_server/web_server.c`  
**Purpose**: HTTP server with REST API and static file serving

**HTTP Server Configuration**:
- **Port**: 80
- **Max Header Length**: 2048 bytes (CONFIG_HTTPD_MAX_REQ_HDR_LEN)
- **CORS Support**: Enabled for cross-origin requests
- **Static Files**: SPIFFS-based file serving

**10 API Endpoints**:

| Method | Path | Description | Features |
|--------|------|-------------|----------|
| GET | / | Dashboard | SPIFFS index.html served |
| GET | /preview.html | Live Preview | MJPEG preview interface |
| GET | /config.html | Configuration | Web configuration UI |
| GET | /stream | MJPEG Stream | multipart/x-mixed-replace |
| GET | /capture | Capture + Upload | Single JPEG capture and remote upload |
| GET | /api/status | System Status | JSON system information |
| GET | /api/config | Get Config | Current configuration JSON |
| POST | /api/config | Update Config | Modify configuration parameters |
| POST | /api/reboot | Reboot Device | System restart (returns 202) |
| GET | /metrics | Metrics | Prometheus-compatible metrics |

### 5. MJPEG Streamer (`mjpeg_streamer.c/h`)
**File**: `mjpeg_streamer/mjpeg_streamer.c`  
**Purpose**: MJPEG video streaming service

**Stream Configuration**:
- **Format**: multipart/x-mixed-replace
- **Max Clients**: 2 concurrent connections
- **Chunk Size**: 4KB
- **Default Resolution**: VGA (640x480)
- **Frame Rate**: 15 FPS
- **Content-Type**: multipart/x-mixed-replace; boundary=frame

**Features**:
- Efficient streaming with low latency
- Client connection limiting
- Frame buffer management
- Stream interruption handling

### 6. Motion Detection (`motion_detect.c/h`)
**File**: `motion_detect/motion_detect.c`  
**Purpose**: Intelligent motion detection and image upload

**Motion Detection Parameters**:
- `SAMPLE_STEP`: 10 (pixel sampling step)
- `PIXEL_DELTA`: 20 (pixel difference threshold)
- `COMPARE_INTERVAL`: 500ms (comparison interval)
- `UPLOAD_MAX_RETRIES`: 3 (upload attempts)
- `UPLOAD_RETRY_DELAY`: 2000ms (retry delay)

**Algorithm**:
- JPEG byte comparison (not pixel-level)
- Efficient motion detection
- Automatic image upload on detection
- Configurable sensitivity and cooldown

### 7. WiFi Manager (`wifi_manager.c/h`)
**File**: `wifi_manager/wifi_manager.c`  
**Purpose**: WiFi connection management with STA/AP fallback

**WiFi Configuration**:
- **Preferred Mode**: STA mode
- **Fallback Mode**: AP mode
- **AP Default SSID**: "MiBeeCam"
- **AP Default Password**: "12345678"
- **AP Default IP**: 192.168.4.1
- **Frequency**: 2.4GHz only

**Features**:
- Automatic mode selection
- Network state monitoring
- Connection retry logic
- AP hotspot capability

### 8. Health Monitor (`health_monitor.c/h`)
**File**: `health_monitor/health_monitor.c`  
**Purpose**: System health monitoring and metrics collection

**Monitoring Features**:
- **Interval**: 60 seconds
- **Metrics**: CPU usage, memory, WiFi status, camera status
- **Prometheus**: Compatible metrics export
- **Alert System**: Critical condition detection

### 9. Status LED (`status_led.c/h`)
**File**: `status_led/status_led.c`  
**Purpose**: System status indication via GPIO 10 LED

**LED Patterns**:
- **LED_STARTING**: Boot up sequence
- **LED_WIFI_CONNECTING**: WiFi connection in progress
- **LED_RUNNING**: System normal operation
- **LED_ERROR**: System error state
- **LED_AP_MODE**: AP hotspot active

### 10. Time Sync (`time_sync.c/h`)
**File**: `time_sync/time_sync.c`  
**Purpose**: NTP time synchronization

**Features**:
- Automatic NTP sync in STA mode
- Configurable timezone support
- Time maintenance during offline periods

### 11. cJSON Parser (`cJSON.c/h`)
**File**: `cJSON/cJSON.c`  
**Purpose**: JSON parsing and generation
- Lightweight JSON implementation
- Used for API responses and configuration

## FreeRTOS Task Architecture

### Task Configuration

| Task | Priority | Stack Size | Purpose | Key Functions |
|------|----------|------------|---------|---------------|
| Main task | default | 8192 | System startup and coordination | app_main() |
| WiFi event | 3 | default | WiFi state monitoring | wifi_event_handler() |
| Health monitor | 4 | 4096 | System health check | health_monitor_task() |
| MJPEG stream | 5 | 8192 | Video streaming | mjpeg_streamer_task() |
| Motion detect | 5 | 8192 | Motion detection | motion_detection_task() |
| Time sync | 4 | default | NTP synchronization | time_sync_task() |
| BOOT button | 5 | 4096 | Factory reset monitoring | boot_button_task() |

### Task Communication
- **Message Queues**: Inter-task communication
- **Semaphores**: Resource synchronization
- **FreeRTOS Flags**: Event notification
- **Static Allocation**: Deterministic memory usage

## File System Structure

### SPIFFS Configuration
- **Offset**: 0x392000
- **Size**: 0x3CE000 (~3.94MB)
- **Source**: `main/web_ui/`
- **Files**: index.html, preview.html, config.html
- **Max Files**: 5
- **Auto Format**: Enabled (format_if_mount_failed=true)

### Web Interface Files
- **index.html**: Main dashboard interface
- **preview.html**: Live MJPEG preview page
- **config.html**: Configuration management interface

## Memory Management

### RAM Usage
- **Frame Buffer**: Single buffer in DRAM (~60KB for VGA)
- **Task Stacks**: Allocated per task requirements
- **NVS Storage**: 24KB for configuration
- **SPIFFS**: 3.94MB for web files

### Memory Optimization
- **PSRAM Disabled**: Due to timing issues causing boot loops
- **Single Buffer**: Memory-efficient frame capture
- **Static Allocation**: Prevents fragmentation

## Network Architecture

### WiFi Stack
- **Protocol**: IEEE 802.11 b/g/n
- **Security**: WPA2-PSK
- **Mode Selection**: STA preferred, AP fallback
- **Band**: 2.4GHz only

### HTTP Server
- **Protocol**: HTTP/1.1
- **Max Connections**: Configurable limit
- **Timeouts**: Configurable connection timeouts
- **CORS**: Enabled for web interface

## Upload Protocol

### Image Upload
```http
POST {server_url}/upload
Content-Type: image/jpeg
X-Device-ID: {device_name}

Body: JPEG binary data
```

### Upload Features
- **Automatic Upload**: Triggered by motion detection
- **Retry Logic**: 3 attempts with 2s intervals
- **Device Identification**: Custom header for server routing
- **Error Handling**: Comprehensive error management

## System Boot Sequence

### Detailed Boot Flow
1. **NVS Initialization**: Load configuration from flash
2. **Config Migration**: Auto-migrate from v1 to v2 if needed
3. **LED Initialization**: Status LED setup
4. **SPIFFS Mount**: Web file system mount
5. **Camera Init**: OV2640 sensor initialization
6. **WiFi Setup**: Network subsystem initialization
7. **Health Monitor**: System health service start
8. **Mode Selection**: STA or AP mode decision
9. **WiFi Connection**: Network establishment
10. **Stream Service**: MJPEG streaming initialization
11. **Web Server**: HTTP service startup
12. **Time Sync**: NTP synchronization
13. **Motion Detect**: Motion detection service start
14. **Boot Monitor**: Factory reset button monitoring

## Error Handling

### Critical Error Recovery
- **Camera Failure**: Switch to AP mode, log error
- **WiFi Failure**: Automatically switch to AP mode
- **SPIFFS Failure**: Auto-mount with format if needed
- **Memory Issues**: Graceful degradation with error logging

### Error Logging
- **System Events**: Boot sequence progress
- **Error Conditions**: Detailed error messages
- **Performance Metrics**: System health monitoring
- **Debug Information**: Comprehensive debugging support

## Performance Optimization

### Frame Rate Management
- **VGA Resolution**: 15 FPS default (optimal balance)
- **Dynamic Adjustment**: Frame rate based on network conditions
- **Buffer Management**: Single buffer for memory efficiency

### Network Optimization
- **Compression**: JPEG quality optimization
- **Bandwidth**: Chunked streaming for efficient transfer
- **Connection**: Maximum concurrent client limits

## Security Considerations

### Network Security
- **WiFi Encryption**: WPA2-PSK mandatory
- **API Access**: Local network access only
- **Configuration**: Protected from unauthorized access

### System Security
- **Factory Reset**: Secure configuration reset
- **Boot Protection**: Boot sequence validation
- **Error Handling**: Secure error state management