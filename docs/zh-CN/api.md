# MiBeeCam API 接口文档

## 📡 概述

MiBeeCam 提供 10 个 HTTP API 接口，支持设备配置、状态查询、视频流、图像捕获等功能。所有接口都使用 JSON 格式进行数据交换。

## 🌐 基本信息

- **基础 URL**: `http://<device_ip>:80`
- **支持方法**: GET, POST
- **内容类型**: `application/json`, `image/jpeg`, `multipart/x-mixed-replace`
- **字符编码**: UTF-8
- **CORS**: 已启用支持

## 🔌 API 端点总览

| 端点 | 方法 | 描述 | 返回格式 |
|------|------|------|----------|
| `/` | GET | Web 主界面 | HTML |
| `/preview.html` | GET | 实时预览页面 | HTML |
| `/config.html` | GET | 配置管理页面 | HTML |
| `/stream` | GET | MJPEG 视频流 | multipart/x-mixed-replace |
| `/capture` | GET | 单帧捕获 + 上传 | JPEG + 响应 |
| `/api/status` | GET | 系统状态 | JSON |
| `/api/config` | GET | 当前配置 | JSON |
| `/api/config` | POST | 更新配置 | JSON |
| `/api/reboot` | POST | 重启设备 | JSON |
| `/metrics` | GET | Prometheus 指标 | Prometheus 格式 |

---

## 📄 详细 API 文档

### 1. Web 主界面

**端点**: `GET /`

**描述**: 返回设备的 Web 管理界面

**响应**:
```http
HTTP/1.1 200 OK
Content-Type: text/html; charset=utf-8
Content-Length: [length]

[HTML 内容]
```

**HTML 内容**: SPIFFS 中的 `index.html` 文件

---

### 2. 实时预览页面

**端点**: `GET /preview.html`

**描述**: 提供实时视频流预览界面

**响应**:
```http
HTTP/1.1 200 OK
Content-Type: text/html; charset=utf-8
Content-Length: [length]

[HTML 内容]
```

**功能特性**:
- MJPEG 实时流显示
- 设备状态监控
- 支持多客户端连接（最多 2 个）

---

### 3. 配置管理页面

**端点**: `GET /config.html`

**描述**: 设备配置管理界面

**响应**:
```http
HTTP/1.1 200 OK
Content-Type: text/html; charset=utf-8
Content-Length: [length]

[HTML 内容]
```

**配置项**:
- 设备名称
- WiFi SSID 和密码
- 服务器 URL
- 运动检测参数
- JPEG 质量
- 时区设置

---

### 4. MJPEG 视频流

**端点**: `GET /stream`

**描述**: 实时 MJPEG 视频流传输

**响应格式**: `multipart/x-mixed-replace`

**响应头**:
```http
HTTP/1.1 200 OK
Content-Type: multipart/x-mixed-replace; boundary=boundarystring
Connection: close
```

**数据格式**:
```
--boundarystring
Content-Type: image/jpeg

[JPEG 图像数据]
--boundarystring
Content-Type: image/jpeg

[JPEG 图像数据]
--boundarystring--
```

**技术特性**:
- 分块传输，每帧独立
- 最大并发连接数: 2
- 默认分辨率: VGA (640x480)
- 默认帧率: 15 FPS
- 分块大小: 4KB

**错误处理**:
- 连接数超限: 503 Service Unavailable
- 系统错误: 500 Internal Server Error

---

### 5. 单帧捕获 + 上传

**端点**: `GET /capture`

**描述**: 捕获单帧图像并上传到配置的服务器

**请求参数**:
| 参数 | 类型 | 必需 | 描述 |
|------|------|------|------|
| `quality` | integer | 否 | JPEG 质量 (1-63, 默认 12) |
| `resolution` | string | 否 | 分辨率 (vga, svga, xga, uxga, 默认 vga) |

**响应**:
```http
HTTP/1.1 200 OK
Content-Type: image/jpeg
X-Device-ID: MiBeeCam

[JPEG 图像数据]
```

**成功响应** (JSON):
```json
{
  "status": "success",
  "message": "Image captured and uploaded",
  "device_id": "MiBeeCam",
  "timestamp": "2026-04-29T10:30:00Z",
  "file_size": 45236,
  "upload_url": "http://server.com/upload"
}
```

**错误响应**:
```json
{
  "status": "error",
  "code": 400,
  "message": "Invalid resolution parameter"
}
```

---

### 6. 系统状态查询

**端点**: `GET /api/status`

**描述**: 获取设备当前系统状态

**响应**:
```json
{
  "status": "ok",
  "device": {
    "name": "MiBeeCam",
    "id": "device-001",
    "uptime": 12345,
    "version": "1.0.0"
  },
  "system": {
    "cpu_usage": 15.2,
    "memory_usage": 45,
    "free_heap": 81920,
    "min_heap": 40960
  },
  "network": {
    "mode": "STA",
    "ssid": "MyWiFi",
    "ip": "192.168.1.100",
    "rssi": -65,
    "status": "connected"
  },
  "camera": {
    "active": true,
    "resolution": "VGA",
    "fps": 15,
    "quality": 12,
    "buffer_count": 1
  },
  "motion": {
    "enabled": true,
    "threshold": 5,
    "cooldown": 10,
    "last_detection": null
  },
  "led": {
    "status": "LED_RUNNING",
    "mode": "blinking"
  },
  "timestamp": "2026-04-29T10:30:00Z"
}
```

**状态码**:
- `200 OK`: 请求成功
- `500 Internal Server Error`: 系统错误

---

### 7. 获取配置

**端点**: `GET /api/config`

**描述**: 获取当前设备配置

**响应**:
```json
{
  "config": {
    "version": 2,
    "magic": 4334148824,
    "device_name": "MiBeeCam",
    "server_url": "http://server.com/api",
    "wifi": {
      "ssid": "MyWiFi",
      "password": "password123"
    },
    "motion": {
      "threshold": 5,
      "cooldown": 10
    },
    "camera": {
      "resolution": "VGA",
      "fps": 15,
      "quality": 12
    },
    "timezone": "CST-8"
  },
  "timestamp": "2026-04-29T10:30:00Z"
}
```

**配置字段说明**:
| 字段 | 类型 | 描述 |
|------|------|------|
| `version` | integer | 配置版本号 (当前: 2) |
| `magic` | integer | 魔数字 (0xA5B6C7D8) |
| `device_name` | string | 设备名称 |
| `server_url` | string | 图像上传服务器 URL |
| `wifi.ssid` | string | WiFi SSID |
| `wifi.password` | string | WiFi 密码 |
| `motion.threshold` | integer | 运动检测阈值 (1-100) |
| `motion.cooldown` | integer | 冷却时间 (秒) |
| `camera.resolution` | string | 摄像头分辨率 |
| `camera.fps` | integer | 帧率 (1-30) |
| `camera.quality` | integer | JPEG 质量 (1-63) |
| `timezone` | string | 时区设置 |

---

### 8. 更新配置

**端点**: `POST /api/config`

**描述**: 更新设备配置

**请求头**:
```
Content-Type: application/json
```

**请求体**:
```json
{
  "device_name": "MiBeeCam",
  "server_url": "http://new-server.com/api",
  "wifi": {
    "ssid": "NewWiFi",
    "password": "newpassword"
  },
  "motion": {
    "threshold": 8,
    "cooldown": 15
  },
  "camera": {
    "resolution": "SVGA",
    "fps": 20,
    "quality": 10
  },
  "timezone": "CST-8"
}
```

**响应**:
```json
{
  "status": "success",
  "message": "Configuration updated successfully",
  "config": {
    "old": { ... },
    "new": { ... }
  },
  "reboot_required": true,
  "timestamp": "2026-04-29T10:30:00Z"
}
```

**错误响应**:
```json
{
  "status": "error",
  "code": 400,
  "message": "Invalid configuration parameters",
  "details": {
    "field": "camera.quality",
    "error": "Quality must be between 1 and 63"
  }
}
```

**配置验证规则**:
- 设备名称: 1-32 字符
- 服务器 URL: 有效的 HTTP/HTTPS URL
- WiFi SSID: 1-32 字符
- WiFi 密码: 8-64 字符
- 运动阈值: 1-100
- 冷却时间: 1-3600 秒
- 帧率: 1-30
- JPEG 质量: 1-63
- 时区: 有效时区字符串

---

### 9. 重启设备

**端点**: `POST /api/reboot`

**描述**: 重启设备

**请求**:
```http
POST /api/reboot
Content-Type: application/json
```

**请求体**:
```json
{
  "delay": 5,
  "reason": "configuration_update"
}
```

**响应**:
```json
{
  "status": "accepted",
  "message": "Device reboot initiated",
  "reboot_time": "2026-04-29T10:30:05Z",
  "reason": "configuration_update",
  "timestamp": "2026-04-29T10:30:00Z"
}
```

**状态码**:
- `202 Accepted`: 重启已启动
- `400 Bad Request`: 参数错误
- `500 Internal Server Error`: 系统错误

---

### 10. Prometheus 指标

**端点**: `GET /metrics`

**描述**: Prometheus 兼容的系统监控指标

**响应格式**: Prometheus 文本格式

**响应示例**:
```
# HELP device_uptime_seconds Device uptime in seconds
# TYPE device_uptime_seconds gauge
device_uptime_seconds 12345

# HELP cpu_usage_percent CPU usage percentage
# TYPE cpu_usage_percent gauge
cpu_usage_percent 15.2

# HELP memory_usage_bytes Memory usage in bytes
# TYPE memory_usage_bytes gauge
memory_usage_bytes 458752

# HELP wifi_rssi WiFi signal strength in dBm
# TYPE wifi_rssi gauge
wifi_rssi -65

# HELP camera_fps Camera frames per second
# TYPE camera_fps gauge
camera_fps 15

# HELP motion_total_count Total motion detection events
# TYPE motion_total_count counter
motion_total_count 42

# HELP motion_upload_errors_count Motion upload error count
# TYPE motion_upload_errors_count counter
motion_upload_errors_count 3

# HELP system_errors_total Total system errors
# TYPE system_errors_total counter
system_errors_total 1

# HELP timestamp_seconds Unix timestamp
# TYPE timestamp_seconds gauge
timestamp_seconds 1714360200
```

**指标说明**:
- **gauge**: 当前状态的测量值
- **counter**: 单调递增的计数器
- **定时更新**: 60 秒间隔

---

## 🚀 错误处理

### HTTP 状态码

| 状态码 | 含义 | 描述 |
|--------|------|------|
| 200 OK | 成功 | 请求成功处理 |
| 202 Accepted | 已接受 | 请求已接受但尚未完成 |
| 400 Bad Request | 客户端错误 | 请求参数无效 |
| 404 Not Found | 未找到 | 请求的资源不存在 |
| 500 Internal Server Error | 服务器错误 | 服务器内部错误 |
| 503 Service Unavailable | 服务不可用 | 服务暂时不可用 |

### 错误响应格式

```json
{
  "status": "error",
  "code": 400,
  "message": "Error description",
  "details": {
    "field": "invalid_field",
    "error": "Specific error message"
  },
  "timestamp": "2026-04-29T10:30:00Z"
}
```

### 常见错误

1. **配置错误**
   ```json
   {
     "status": "error",
     "code": 400,
     "message": "Invalid configuration",
     "details": {
       "wifi.ssid": "SSID cannot be empty"
     }
   }
   ```

2. **网络连接错误**
   ```json
   {
     "status": "error",
     "code": 500,
     "message": "Network connection failed",
     "details": {
       "error": "WiFi connection timeout"
     }
   }
   ```

3. **摄像头错误**
   ```json
   {
     "status": "error",
     "code": 500,
     "message": "Camera initialization failed",
     "details": {
       "error": "Camera configuration error"
     }
   }
   ```

---

## 🔐 安全考虑

### 认证
- 当前版本无需认证
- 建议在生产环境中添加基本认证

### 输入验证
- 所有输入参数都经过验证
- 字符串长度限制
- 数值范围检查

### CORS 支持
- 已启用跨域请求支持
- 适用于 Web 前端集成

---

*API 接口文档 - 最后更新: 2026-04-29*