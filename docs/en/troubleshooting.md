# Troubleshooting Guide - MiBeeCam ESP32-S3-A10

This document provides comprehensive troubleshooting information for the MiBeeCam smart camera system, including common issues encountered during development and their solutions.

## Getting Started with Troubleshooting

### Basic Debug Commands
```bash
# Monitor serial output
idf.py monitor

# Build check
idf.py build

# Check WiFi connection
idf.py monitor

# Test camera
idf.py menuconfig -> Camera -> Test configuration
```

### Common Symptoms
- **Boot Loop**: Device continuously restarts
- **No Camera Output**: Black screen or no image
- **WiFi Connection Failure**: Cannot connect to network
- **Stream Not Working**: MJPEG stream fails
- **Upload Issues**: Image upload fails
- **Web Interface Not Loading**: Cannot access dashboard

## Critical Hardware Issues

### 1. PSRAM Timing Boot Loop
**Problem**: Device continuously reboots during startup  
**Root Cause**: ESP-IDF v6.0 PSRAM timing tuning all failed (14/14 bad) causing boot loop  
**Solution**: Disable PSRAM entirely

**Symptoms**:
- Continuous boot messages
- "PSRAM tuning failed" errors
- Device never reaches normal operation

**Fix**:
1. Ensure PSRAM is disabled in sdkconfig.defaults:
   ```c
   # CONFIG_ESP_TASK_WDT_TIMEOUT_S is not set
   # CONFIG_SPIRAM is not set
   # CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 is not set
   ```

2. Use correct ESP-IDF version:
   ```bash
   # Use ESP-IDF v5.4.3 (v6.0 has PSRAM bugs)
   ./install.sh -f
   source export.sh
   ```

3. Verify PSRAM is disabled in menuconfig:
   ```bash
   idf.py menuconfig
   # Camera -> PSRAM -> Disabled
   # Component config -> ESP32-specific -> PSRAM interface -> Not used
   ```

**Verification**:
```bash
idf.py monitor
# Should show: "PSRAM disabled due to timing issues"
```

### 2. Camera Pin Mismatch
**Problem**: Camera not working with incorrect pin configuration  
**Root Cause**: Wrong camera model definition, pins don't match LuatOS documentation  
**Solution**: Use CAMERA_MODEL_Air_ESP32S3 model definition

**Symptoms**:
- "Camera init failed" messages
- No camera output
- I2C communication errors

**Fix**:
1. Update camera driver with correct pins:
   ```c
   // In camera_driver.c, use:
   #define CAMERA_MODEL_Air_ESP32S3
   ```

2. Verify pin configuration matches actual hardware:
   ```c
   // Camera pin mapping:
   #define XCLK_PIN     39
   #define SIOD_PIN     21
   #define SIOC_PIN     46
   #define D0_PIN      34
   #define D1_PIN      47
   #define D2_PIN      48
   #define D3_PIN      33
   #define D4_PIN      35
   #define D5_PIN      37
   #define D6_PIN      38
   #define D7_PIN      40
   #define VSYNC_PIN   42
   #define HREF_PIN    41
   #define PCLK_PIN    36
   ```

**Verification**:
```bash
idf.py menuconfig -> Camera -> Test camera configuration
idf.py monitor
```

### 3. XCLK GPIO Matrix Issue
**Problem**: Camera initialization fails due to incorrect XCLK method  
**Root Cause**: ESP32-S3 can't use GPIO matrix for XCLK, requires LEDC method  
**Solution**: Use LEDC method for XCLK generation

**Symptoms**:
- Camera init fails with GPIO errors
- XCLK signal not working
- "Clock generation failed" messages

**Fix**:
```c
// Use LEDC method instead of GPIO matrix
#include "driver/ledc.h"

void s3_enable_xclk() {
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_DUTY_16BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 10000000,  // 10MHz
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t ledc_conf = {
        .gpio_num = 39,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 32768,  // 50% duty cycle
        .hpoint = 0
    };
    ledc_channel_config(&ledc_conf);
}
```

**Verification**:
```bash
idf.py monitor
# Should show: "XCLK enabled via LEDC"
```

## Software Configuration Issues

### 1. MJPEG Stream 404 Error
**Problem**: /stream endpoint returns 404 not found  
**Root Cause**: Wildcard URI intercepts /stream endpoint  
**Solution**: Register /stream before /*

**Symptoms**:
- Accessing /stream gives 404 error
- Stream client shows connection failed
- Other endpoints work fine

**Fix**:
```c
// In web_server.c, ensure /stream is registered before /*
uri_handler_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = mjpeg_stream_handler,
    .user_ctx  = NULL
};

// Register stream URI first
esp_err_t ret = esp_httpd_register_uri_handler(server, &stream_uri);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "/stream URI registration failed");
    return ret;
}

// Then register wildcard
uri_handler_t wildcard_uri = {
    .uri       = "/*",
    .method    = HTTP_GET,
    .handler   = static_file_get_handler,
    .user_ctx  = NULL
};

ret = esp_httpd_register_uri_handler(server, &wildcard_uri);
```

**Verification**:
```bash
curl -I http://192.168.1.100/stream
# Should return 200 OK with multipart content type
```

### 2. HTTP Header Too Long Error
**Problem**: Web interface loads partially or fails  
**Root Cause**: Default HTTP header length too small (512 bytes)  
**Solution**: Increase CONFIG_HTTPD_MAX_REQ_HDR_LEN to 2048

**Symptoms**:
- "Header too long" errors
- Web interface not loading
- API requests failing

**Fix**:
1. Update sdkconfig.defaults:
   ```
   CONFIG_HTTPD_MAX_REQ_HDR_LEN=2048
   ```

2. Rebuild project:
   ```bash
   idf.py clean
   idf.py build
   ```

**Verification**:
```bash
curl -I http://192.168.1.100/
# Should load without header errors
```

### 3. HTML Chinese Character Display Issue
**Problem**: Chinese text displays as \uXXXX escapes  
**Root Cause**: HTML encoding issues  
**Solution**: Use Python to replace escapes with UTF-8

**Fix**:
```python
# Process HTML files with Chinese characters
import re

def process_html_file(input_file, output_file):
    with open(input_file, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Replace Unicode escapes with actual characters
    content = re.sub(r'\\u([0-9a-fA-F]{4})', lambda m: chr(int(m.group(1), 16)), content)
    
    with open(output_file, 'w', encoding='utf-8') as f:
        f.write(content)

# Process all HTML files
for file in ['index.html', 'preview.html', 'config.html']:
    process_html_file(f'main/web_ui/{file}', f'build/spiffs/{file}')
```

**Verification**:
```bash
curl http://192.168.1.100/
# Chinese characters should display correctly
```

## Network and Connectivity Issues

### 1. WiFi 5GHz Connection Failure
**Problem**: Device cannot connect to 5GHz WiFi networks  
**Root Cause**: ESP32-S3 only supports 2.4GHz WiFi  
**Solution**: Force 2.4GHz only mode

**Symptoms**:
- Cannot find 5GHz networks
- Connection attempts fail on 5GHz
- Limited WiFi performance

**Fix**:
```c
// In wifi_manager.c, force 2.4GHz only
wifi_config_t wifi_config = {
    .sta = {
        .ssid = WIFI_SSID,
        .password = WIFI_PASS,
        .threshold.authmode = WIFI_AUTH_WPA2_PSK,
    },
};

// Force 2.4GHz mode
wifi_country_t country = {
    .cc = "CN",  // Country code
    .schan = 1,  // Start channel (2.4GHz)
    .nchan = 13, // End channel (2.4GHz)
    .policy = WIFI_COUNTRY_POLICY_MANUAL
};
esp_wifi_set_country(&country);
```

**Verification**:
```bash
idf.py monitor
# Should show only 2.4GHz networks available
```

### 2. Memory Frame Buffer Resolution Limits
**Problem**: Higher camera resolutions cause memory errors  
**Root Cause**: DRAM frame buffer size limits resolution  
**Solution**: Use VGA resolution (640x480) default

**Symptoms**:
- "Frame buffer too large" errors
- Camera initialization fails
- Memory allocation failures

**Fix**:
```c
// Use VGA resolution (640x480) default
#define FRAME_WIDTH   640
#define FRAME_HEIGHT  480
#define FRAME_SIZE     (FRAME_WIDTH * FRAME_HEIGHT * 2)  // ~614KB

// Avoid SVGA or higher resolutions on single buffer
// SVGA (800x600) requires ~480KB buffer
// Higher resolutions exceed memory limits
```

**Verification**:
```bash
idf.py monitor
# Should show successful VGA frame capture
```

### 3. xQueueGenericSend Crash on First Boot
**Problem**: System crashes on first boot, works on second boot  
**Root Cause**: Race condition in FreeRTOS task initialization  
**Solution**: Add proper synchronization

**Symptoms**:
- Task initialization errors
- "xQueueGenericSend failed" messages
- System unstable on first boot

**Fix**:
```c
// Add proper task synchronization
SemaphoreHandle_t xSystemReadySemaphore;

void system_init_task(void *pvParameters) {
    // Initialize all components first
    camera_init();
    wifi_init();
    web_server_init();
    
    // Signal that system is ready
    xSemaphoreGive(xSystemReadySemaphore);
    
    vTaskDelete(NULL);
}

void app_main() {
    // Create synchronization semaphore
    xSystemReadySemaphore = xSemaphoreCreateBinary();
    
    // Create init task
    xTaskCreate(system_init_task, "system_init", 4096, NULL, 5, NULL);
    
    // Wait for system to be ready
    if (xSemaphoreTake(xSystemReadySemaphore, pdMS_TO_TICKS(10000)) == pdTRUE) {
        ESP_LOGI(TAG, "System ready, starting services");
    } else {
        ESP_LOGE(TAG, "System initialization timeout");
    }
}
```

## Storage and File System Issues

### 1. SPIFFS Partition Offset Mismatch
**Problem**: SPIFFS files not accessible after flashing  
**Root Cause**: Flash partition offset mismatch  
**Solution**: Ensure offset 0x392000 matches partitions.csv

**Symptoms**:
- "SPIFFS mount failed" errors
- Web interface files not loading
- "File not found" errors

**Fix**:
1. Verify partitions.csv:
   ```csv
   spiffs,data,spiffs,0x392000,0x3CE000
   ```

2. Ensure build process uses correct offset:
   ```python
   # Generate SPIFFS image with correct offset
   import subprocess
   subprocess.run([
       'python', '$IDF_PATH/components/spiffs/spiffsgen.py',
       '0x3CE000', 'main/web_ui', 'build/spiffs.bin'
   ])
   ```

3. Flash with correct addresses:
   ```bash
   python -m esptool --chip esp32s3 -p COMx write_flash 0x392000 build/spiffs.bin
   ```

**Verification**:
```bash
curl http://192.168.1.100/
# Should load web interface files
```

## Development Environment Issues

### 1. ESP-IDF Version Compatibility
**Problem**: Build errors with ESP-IDF v6.0  
**Root Cause**: ESP-IDF v6.0 has PSRAM bugs and compatibility issues  
**Solution**: Use ESP-IDF v5.4.3

**Fix**:
```bash
# Switch to ESP-IDF v5.4.3
cd $HOME/esp/esp-idf
git checkout v5.4.3

# Update submodules
git submodule update --init --recursive

# Clean and rebuild
source export.sh
cd $PROJECT_PATH
idf.py clean
idf.py build
```

### 2. Serial Monitor Connection Issues
**Problem**: Cannot connect serial monitor  
**Root Cause**: Wrong COM port or baud rate  
**Solution**: Check port configuration

**Fix**:
```bash
# Find correct COM port
idf.py --port COMx monitor

# Common baud rates: 115200, 921600
# Try different ports: COM3, COM4, COM5
```

## Common Runtime Issues

### 1. Camera Initialization Failed
**Problem**: Camera fails to initialize  
**Troubleshooting Steps**:
1. Check camera pins match hardware
2. Verify camera model selection
3. Check power supply (3.3V stable)
4. Look for I2C communication errors

**Debug Commands**:
```bash
idf.py monitor
# Look for: "I2C scan results", "Camera init"
```

### 2. WiFi Connection Timeout
**Problem**: Cannot connect to WiFi network  
**Troubleshooting Steps**:
1. Verify SSID and password
2. Check WiFi mode (STA/AP)
3. Check signal strength
4. Try different channel

**Debug Commands**:
```bash
idf.py monitor
# Look for: "WiFi scan", "Connecting to SSID"
```

### 3. MJPEG Stream Not Working
**Problem**: Live video stream fails  
**Troubleshooting Steps**:
1. Check web interface path
2. Verify stream endpoint registration
3. Check client count limit
4. Check network bandwidth

**Debug Commands**:
```bash
curl -I http://192.168.1.100/stream
# Should return 200 with multipart content type
```

### 4. Image Upload Failure
**Problem**: Motion detection uploads fail  
**Troubleshooting Steps**:
1. Check server URL accessibility
2. Verify device ID configuration
3. Check network connectivity
4. Check server response

**Debug Commands**:
```bash
# Test server connectivity
curl -X POST http://server.com/upload -H "X-Device-ID: test" --data-binary @test.jpg
```

## Performance Issues

### 1. Slow Frame Rate
**Problem**: Camera frame rate lower than expected  
**Root Cause**: Network bandwidth or CPU constraints  
**Solution**: Optimize JPEG quality and frame rate

**Fix**:
```c
// Reduce JPEG quality for better performance
#define JPEG_QUALITY 20  // Higher number = smaller file, lower quality

// Reduce frame rate
#define FRAME_RATE 10  // FPS instead of 15
```

### 2. Memory Fragmentation
**Problem**: System becomes unstable over time  
**Root Cause**: Memory fragmentation in long-running system  
**Solution**: Use static memory allocation

**Fix**:
```c
// Use static allocation for critical components
static camera_config_t camera_config = {
    .pin_pwdn = -1,
    .pin_reset = -1,
    .pin_xclk = 39,
    // ... other config
};
```

## Emergency Recovery

### Factory Reset via BOOT Button
**Problem**: System configuration corrupted  
**Solution**: Factory reset using BOOT button

**Steps**:
1. Power off the device
2. Press and hold BOOT button
3. Power on the device
4. Continue holding BOOT button for 5 seconds
5. Release button when LED shows rapid blinking

**Verification**:
```bash
idf.py monitor
# Should show: "Factory reset initiated"
```

### Safe Mode Boot
**Problem**: System fails to boot normally  
**Solution**: Boot in safe mode to diagnose issues

**Steps**:
1. Remove camera module
2. Boot with minimal configuration
3. Check serial output for error messages
4. Reconfigure system step by step

## System Health Monitoring

### Monitoring Commands
```bash
# Check memory usage
idf.py monitor
# Look for: "Free heap", "Min heap"

# Check WiFi status
curl http://192.168.1.100/api/status

# Check camera status
curl http://192.168.1.100/api/status

# Check system uptime
curl http://192.168.1.100/metrics
```

### Performance Metrics
```bash
# Monitor frame rate
curl http://192.168.1.100/api/status | grep fps

# Check memory usage
curl http://192.168.1.100/api/status | grep heap

# Monitor motion events
curl http://192.168.1.100/metrics | grep motion
```

## Getting Additional Help

### Log Analysis
```bash
# Save log output
idf.py monitor > mibeecam.log 2>&1

# Search for errors
grep -i "error" mibeecam.log
grep -i "fail" mibeecam.log
```

### System Information Dump
```bash
# Get comprehensive system status
curl http://192.168.1.100/api/status | python -m json.tool

# Get configuration dump
curl http://192.168.1.100/api/config | python -m json.tool
```

## Contributing to Documentation

If you encounter issues not covered in this guide:
1. Document the problem and symptoms
2. Include error messages and serial logs
3. Note hardware configuration and environment
4. Submit to the project repository for inclusion in future versions