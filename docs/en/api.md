# API Reference - MiBeeCam ESP32-S3-A10

This document provides comprehensive API documentation for all HTTP endpoints and web interfaces of the MiBeeCam smart camera system.

## Base URL

All API endpoints are accessible at:
```
http://<device-ip>:80/
```

Where `<device-ip>` is the IP address of your MiBeeCam device.

## HTTP Server Configuration

- **Port**: 80
- **Max Header Length**: 2048 bytes
- **CORS Support**: Enabled
- **Content Types**: JSON, HTML, JPEG, multipart
- **Character Encoding**: UTF-8

## Web Interface Endpoints

### 1. Main Dashboard
**Endpoint**: `GET /`  
**Content-Type**: `text/html`  
**Purpose**: Main web interface dashboard

**Response**:
```html
<!DOCTYPE html>
<html>
<head>
    <title>MiBeeCam Dashboard</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="styles.css">
</head>
<body>
    <div class="container">
        <h1>MiBeeCam Control Panel</h1>
        <div class="status" id="status">System Status: Running</div>
        <div class="camera-feed">
            <img src="/preview.html" alt="Camera Feed" id="camera">
        </div>
        <div class="controls">
            <button onclick="captureImage()">Capture Image</button>
            <button onclick="configure()">Configuration</button>
        </div>
    </div>
</body>
</html>
```

### 2. Live Preview
**Endpoint**: `GET /preview.html`  
**Content-Type**: `text/html`  
**Purpose**: Live MJPEG preview interface

**Features**:
- Real-time video streaming
- Auto-refresh mechanism
- Mobile responsive design
- Camera status indicators

### 3. Configuration Interface
**Endpoint**: `GET /config.html`  
**Content-Type**: `text/html`  
**Purpose**: Web configuration management interface

**Configuration Options**:
- WiFi settings
- Motion detection parameters
- Server upload configuration
- System preferences
- Device information

## REST API Endpoints

### 1. System Status API
**Endpoint**: `GET /api/status`  
**Content-Type**: `application/json`  
**Purpose**: Get current system status and health information

**Request**:
```http
GET /api/status
```

**Response**:
```json
{
    "status": "running",
    "uptime": 12345,
    "wifi": {
        "connected": true,
        "ssid": "MyWiFi",
        "ip": "192.168.1.100",
        "rssi": -45,
        "mode": "STA"
    },
    "camera": {
        "enabled": true,
        "resolution": "VGA",
        "fps": 15,
        "quality": 12
    },
    "memory": {
        "free_heap": 81920,
        "min_heap": 40960,
        "psram": false
    },
    "storage": {
        "spiffs_mounted": true,
        "total_space": 4128768,
        "used_space": 5120
    },
    "uptime_seconds": 12345,
    "timestamp": "2024-01-15T10:30:00Z"
}
```

### 2. Get Configuration
**Endpoint**: `GET /api/config`  
**Content-Type**: `application/json`  
**Purpose**: Retrieve current configuration parameters

**Request**:
```http
GET /api/config
```

**Response**:
```json
{
    "device_name": "MiBeeCam",
    "timezone": "CST-8",
    "motion_threshold": 5,
    "motion_cooldown": 10,
    "wifi": {
        "ssid": "MyWiFi",
        "password": "mypassword",
        "mode": "STA"
    },
    "upload": {
        "server_url": "https://api.example.com/upload",
        "device_id": "camera-device-001",
        "max_retries": 3,
        "retry_delay": 2000
    },
    "camera": {
        "resolution": 0,
        "fps": 15,
        "jpeg_quality": 12
    },
    "config_version": "v2",
    "timestamp": "2024-01-15T10:30:00Z"
}
```

### 3. Update Configuration
**Endpoint**: `POST /api/config`  
**Content-Type**: `application/json`  
**Purpose**: Update configuration parameters

**Request**:
```http
POST /api/config
Content-Type: application/json

{
    "device_name": "NewCameraName",
    "motion_threshold": 8,
    "motion_cooldown": 15,
    "wifi": {
        "ssid": "NewWiFi",
        "password": "newpassword"
    }
}
```

**Response**:
```json
{
    "success": true,
    "message": "Configuration updated successfully",
    "timestamp": "2024-01-15T10:30:00Z"
}
```

**Error Response**:
```json
{
    "success": false,
    "error": "Invalid configuration parameter",
    "details": "motion_threshold must be between 1 and 100",
    "timestamp": "2024-01-15T10:30:00Z"
}
```

### 4. Reboot Device
**Endpoint**: `POST /api/reboot`  
**Content-Type**: `application/json`  
**Purpose**: Reboot the MiBeeCam device

**Request**:
```http
POST /api/reboot
```

**Response**:
```json
{
    "success": true,
    "message": "Reboot initiated",
    "estimated_time": 30,
    "timestamp": "2024-01-15T10:30:00Z"
}
```

**Status Code**: 202 Accepted

## Streaming API

### 1. MJPEG Video Stream
**Endpoint**: `GET /stream`  
**Content-Type**: `multipart/x-mixed-replace; boundary=frame`  
**Purpose**: Live MJPEG video streaming

**Stream Format**:
```
HTTP/1.1 200 OK
Content-Type: multipart/x-mixed-replace; boundary=frame

--frame
Content-Type: image/jpeg

[JPEG image data]
--frame
Content-Type: image/jpeg

[JPEG image data]
...
```

**Features**:
- Continuous streaming
- VGA resolution (640x480)
- 15 FPS frame rate
- Maximum 2 concurrent clients
- Automatic client disconnection handling

## Capture API

### 1. Capture and Upload
**Endpoint**: `GET /capture`  
**Content-Type**: `image/jpeg`  
**Purpose**: Capture single frame and upload to server

**Request**:
```http
GET /capture
```

**Response**:
```html
<JPEG image data>
```

**Upload Process**:
1. Capture single JPEG frame
2. Upload to configured server URL
3. Return captured image to client

**Upload Protocol**:
```http
POST {server_url}/upload
Content-Type: image/jpeg
X-Device-ID: {device_name}

Body: JPEG binary data
```

## Metrics API

### 1. Prometheus Metrics
**Endpoint**: `GET /metrics`  
**Content-Type**: `text/plain`  
**Purpose**: Prometheus-compatible system metrics

**Request**:
```http
GET /metrics
```

**Response**:
```
# HELP mibeecam_system_uptime_seconds System uptime in seconds
# TYPE mibeecam_system_uptime_seconds counter
mibeecam_system_uptime_seconds 12345

# HELP mibeecam_wifi_signal_strength WiFi signal strength in dBm
# TYPE mibeecam_wifi_signal_strength gauge
mibeecam_wifi_signal_strength -45

# HELP mibeecam_camera_frames_total Total camera frames captured
# TYPE mibeecam_camera_frames_total counter
mibeecam_camera_frames_total 5678

# HELP mibeecam_motion_events_total Total motion detection events
# TYPE mibeecam_motion_events_total counter
mibeecam_motion_events_total 123

# HELP mibeecam_upload_attempts_total Total upload attempts
# TYPE mibeecam_upload_attempts_total counter
mibeecam_upload_attempts_total 456

# HELP mibeecam_free_heap_bytes Free heap memory in bytes
# TYPE mibeecam_free_heap_bytes gauge
mibeecam_free_heap_bytes 81920

# HELP mibeecam_memory_usage_percent Memory usage percentage
# TYPE mibeecam_memory_usage_percent gauge
mibeecam_memory_usage_percent 60.5
```

## WebSocket API (Future Enhancement)

### Planning Notes:
- **WebSocket Endpoint**: `/ws` (planned)
- **Purpose**: Real-time bidirectional communication
- **Features**: 
  - Live camera feed streaming
  - Real-time system status updates
  - Configuration push notifications
  - Motion event notifications

## Error Handling

### Standard Error Response Format
```json
{
    "success": false,
    "error": "error_message",
    "details": "detailed_error_description",
    "timestamp": "2024-01-15T10:30:00Z"
}
```

### HTTP Status Codes

| Status Code | Description | Usage |
|-------------|-------------|-------|
| 200 | OK | Successful request |
| 202 | Accepted | Reboot command accepted |
| 400 | Bad Request | Invalid request format |
| 401 | Unauthorized | Authentication required (future) |
| 404 | Not Found | Resource not found |
| 500 | Internal Server Error | Server error |
| 503 | Service Unavailable | Service temporarily unavailable |

### Common Error Messages

| Error Code | Message | Description |
|------------|---------|-------------|
| config_invalid | "Invalid configuration parameter" | Validation error |
| network_error | "Network connection failed" | WiFi/network issue |
| camera_error | "Camera initialization failed" | Camera hardware issue |
| storage_error | "Storage system error" | SPIFFS/NVS issue |
| memory_error | "Insufficient memory" | Memory allocation failure |
| upload_error | "Image upload failed" | Server communication failure |

## Rate Limiting

### API Rate Limits
- **Configuration Updates**: 1 request per minute
- **Capture Requests**: 5 requests per minute
- **Status Requests**: No limit
- **Stream Requests**: Max 2 concurrent connections

### Implementation Notes
- Rate limiting prevents abuse
- Burst requests allowed within limits
- Connection tracking for streaming clients

## Authentication and Security

### Current Security Model
- **Network Access**: Local WiFi network only
- **No Authentication**: Currently open for local access
- **HTTPS**: Not implemented (local network assumption)

### Planned Security Features
- **Basic Authentication**: Username/password protection
- **HTTPS Support**: Secure communication
- **API Token**: Access token authentication
- **IP Whitelisting**: Restrict access to specific IPs

## Client Integration Examples

### JavaScript Web Interface
```javascript
// Get system status
fetch('/api/status')
    .then(response => response.json())
    .then(data => console.log('System status:', data));

// Update configuration
fetch('/api/config', {
    method: 'POST',
    headers: {
        'Content-Type': 'application/json',
    },
    body: JSON.stringify({
        motion_threshold: 10,
        device_name: 'MyCamera'
    })
})
.then(response => response.json())
.then(data => console.log('Update result:', data));
```

### Python Client Example
```python
import requests

# Get configuration
response = requests.get('http://192.168.1.100/api/config')
config = response.json()
print(f"Device name: {config['device_name']}")

# Update configuration
update_data = {
    "motion_threshold": 15,
    "motion_cooldown": 20
}
response = requests.post(
    'http://192.168.1.100/api/config',
    json=update_data
)
print(f"Update success: {response.json()}")
```

### cURL Examples
```bash
# Get system status
curl http://192.168.1.100/api/status

# Update configuration
curl -X POST http://192.168.1.100/api/config \
     -H "Content-Type: application/json" \
     -d '{"motion_threshold": 8, "device_name": "NewCamera"}'

# Capture image
curl -o capture.jpg http://192.168.1.100/capture

# Get metrics
curl http://192.168.1.100/metrics
```

## WebSocket Integration (Planned)

### JavaScript WebSocket Example
```javascript
const ws = new WebSocket('ws://192.168.1.100/ws');

ws.onmessage = function(event) {
    const data = JSON.parse(event.data);
    if (data.type === 'motion') {
        console.log('Motion detected!');
        // Update UI
    } else if (data.type === 'status') {
        console.log('System status:', data.status);
        // Update system status
    }
};

// Subscribe to motion events
ws.send(JSON.stringify({
    action: 'subscribe',
    event: 'motion'
}));
```

## Performance Considerations

### Optimization Tips
- **Client-Side Caching**: Cache web interface files
- **Connection Pooling**: Reuse HTTP connections
- **Streaming Optimization**: Proper MJPEG handling
- **Error Handling**: Implement retry logic for critical operations

### Best Practices
- **Error Recovery**: Implement retry logic for failed operations
- **Connection Management**: Properly close streaming connections
- **Memory Management**: Monitor heap usage during operations
- **Network Resilience**: Handle WiFi disconnections gracefully

## Troubleshooting API Issues

### Common API Problems
1. **404 Error**: Check endpoint path and ensure services are running
2. **Connection Failed**: Verify WiFi network and IP address
3. **500 Error**: Check server logs for detailed error information
4. **Slow Response**: Check network conditions and device load

### Debugging Commands
```bash
# Check web server status
idf.py monitor

# Check WiFi connection
idf.py monitor

# Check camera status
curl http://192.168.1.100/api/status
```

## Version Information

### API Versioning
- **Current Version**: 1.0
- **Compatibility**: Backward compatible with existing clients
- **Future Changes**: Major version changes will be documented

### Changelog
- **v1.0**: Initial API implementation with core endpoints
- **Future**: WebSocket support, authentication, additional metrics

## Support

For API-related issues and feature requests:
- Review this documentation for usage examples
- Check the troubleshooting section for common issues
- Refer to the main project documentation for system configuration
- Consult the [troubleshooting guide](troubleshooting.md) for system-level issues