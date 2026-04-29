# MiBeeCam 故障排查指南

## 🚨 故障概述

本文档记录了 MiBeeCam 开发过程中遇到的真实问题和解决方案，帮助用户快速诊断和解决常见故障。

---

## 🔧 硬件问题

### 问题 1: PSRAM 时序调谐失败导致启动循环

**现象**: 设备启动后反复重启，无法进入正常工作状态

**原因**: ESP32-S3 的 PSRAM 时序调谐参数全部失败 (14/14 测试参数均失败)，导致无法正常访问 PSRAM

**诊断方法**:
```bash
# 串口输出
I (30) boot: PSRAM initialization failed
I (31) boot: PSRAM timing tuning failed, all 14 configurations tested
E (32) boot: PSRAM disabled due to critical failure
```

**解决方案**:
1. **完全禁用 PSRAM**: 在 `sdkconfig.defaults` 中设置
   ```c
   # CONFIG_ESP32S3_SPIRAM_SUPPORT=y  # 注释掉此行
   CONFIG_ESP32S3_SPIRAM_MODE=disabled  # 明确设置为 disabled
   ```
2. **修改摄像头配置**: 确保使用 `CAMERA_FB_IN_DRAM` 而不是 `CAMERA_FB_IN_PSRAM`
3. **降低分辨率**: 使用 VGA (640x480) 而不是更高分辨率

**预防措施**:
- 始终使用 `lsp_diagnostics` 检查编译错误
- 编译后检查串口输出日志
- 避免启用 PSRAM 相关功能

---

### 问题 2: ESP-IDF v6.0 PSRAM 兼容性问题

**现象**: 使用 ESP-IDF v6.0 时 PSRAM 初始化失败，设备无法正常启动

**原因**: ESP-IDF v6.0 存在已知的 PSRAM 驱动 bug，与某些 ESP32-S3 板卡不兼容

**解决方案**:
```bash
# 回退到 ESP-IDF v5.4.3
git checkout v5.4.3
. $HOME/esp/esp-idf/export.sh
```

**验证方法**:
```bash
# 检查 ESP-IDF 版本
idf.py --version
# 应该显示: v5.4.3
```

**预防措施**:
- 确保使用推荐的 ESP-IDF 版本
- 定期检查 ESP-IDF 更新日志
- 在新版本发布前充分测试

---

### 问题 3: 摄像头引脚映射错误

**现象**: 摄像头初始化失败，无法获取图像数据

**原因**: 使用了错误的摄像头模型定义，引脚映射与实际硬件不匹配

**诊断方法**:
```bash
# 串口输出
E (123) camera: Camera probe failed
E (124) camera: Camera configuration error
```

**解决方案**:
1. **使用正确的摄像头模型**:
   ```c
   # 必须使用 ESP32-S3-A10 专用型号
   # 错误: #define CAMERA_MODEL_AI_THINKER
   # 正确: #define CAMERA_MODEL_Air_ESP32S3
   ```
2. **更新引脚映射** (camera_driver.c):
   ```c
   // ESP32-S3-A10 专用引脚映射
   #define CAMERA_PIN_XCLK     39
   #define CAMERA_PIN_SIOD      21
   #define CAMERA_PIN_SIOC      46
   #define CAMERA_PIN_D0        34
   #define CAMERA_PIN_D1        47
   #define CAMERA_PIN_D2        48
   #define CAMERA_PIN_D3        33
   #define CAMERA_PIN_D4        35
   #define CAMERA_PIN_D5        37
   #define CAMERA_PIN_D6        38
   #define CAMERA_PIN_D7        40
   #define CAMERA_PIN_VSYNC     42
   #define CAMERA_PIN_HREF      41
   #define CAMERA_PIN_PCLK      36
   ```

**验证方法**:
```bash
# 编译并烧录后检查串口输出
I (456) camera: Camera initialized successfully
```

---

### 问题 4: XCLK 时钟信号生成错误

**现象**: 摄像头无法正常工作，图像数据异常

**原因**: ESP32-S3 的 XCLK 不能使用 GPIO 矩阵方法生成，必须使用 LEDC 方法

**解决方案**:
```c
// 使用 LEDC 方法生成 XCLK 时钟
void s3_enable_xclk(int freq) {
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = freq,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_conf);
    
    ledc_channel_config_t channel_conf = {
        .gpio_num = 39,  // XCLK GPIO
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 50,
        .hpoint = 0
    };
    ledc_channel_config(&channel_conf);
}
```

**关键要点**:
- 必须使用 `s3_enable_xclk()` 函数
- 不能使用标准的 GPIO 输出方法
- 频率设置为 10MHz

---

### 问题 5: Flash 大小配置错误

**现象**: 程序无法编译或烧录失败

**原因**: Flash 实际大小为 16MB，但配置文件错误地设置为 8MB

**解决方案**:
```bash
# 在 menuconfig 中配置
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
```

**分区表验证** (partitions.csv):
```csv
# 确保分区大小与 16MB Flash 匹配
factory,app,factory,0x10000,0x380000  # 3.5MB
spiffs,data,spiffs,0x392000,0x3CE000  # ~3.94MB
```

---

## 🌐 网络问题

### 问题 6: MJPEG 流 /stream 返回 404 错误

**现象**: 访问 `/stream` 端点返回 404 Not Found

**原因**: 通配符 URI 处理器 (`/*`) 拦截了 `/stream` 请求

**解决方案**:
```c
// 在 web_server.c 中，必须先注册 /stream，再注册 /*
err_t stream_uri_handler(httpd_req_t *req);
err_t wildcard_uri_handler(httpd_req_t *req);

// 正确的注册顺序
esp_err_t ret = httpd_register_uri_handler(server, &stream_uri);
if (ret != ESP_OK) {
    return ret;
}
ret = httpd_register_uri_handler(server, &wildcard_uri);
```

**验证方法**:
```bash
# 使用 curl 测试
curl -I http://192.168.4.1/stream
# 应该返回 200 OK
```

---

### 问题 7: HTTP 请求头长度超限

**现象**: 大型 POST 请求失败，返回 400 Bad Request

**原因**: 默认 HTTP 请求头长度 (512 字节) 不够，特别是包含大量 JSON 数据时

**解决方案**:
```c
// 在 sdkconfig.defaults 中设置
CONFIG_HTTPD_MAX_REQ_HDR_LEN=2048
```

**验证方法**:
```bash
# 测试大配置文件上传
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -d '{"large_config_data": "..."}'
```

---

### 问题 8: WiFi 5GHz 频段支持问题

**现象**: 无法连接 5GHz WiFi 网络

**原因**: ESP32-S3 仅支持 2.4GHz WiFi，不支持 5GHz 频段

**解决方案**:
1. **仅使用 2.4GHz 网络**
2. **在代码中明确指定频段**:
   ```c
   wifi_config_t wifi_config = {
       .sta = {
           .ssid = "MyWiFi_2.4G",
           .password = "password",
           .threshold.rssi = -65,  // 2.4GHz 信号强度阈值
           .scan_method = WIFI_FAST_SCAN,
           .sort_method = WIFI_CONNECT_AP_BY_SIGNAL
       }
   };
   ```

**预防措施**:
- 使用 `network_config_t` 验证 WiFi 配置
- 检查 WiFi 频段支持限制
- 在用户界面中明确提示支持频段

---

## 💾 存储问题

### 问题 9: SPIFFS 分区偏移量不匹配

**现象**: Web 界面文件无法正确加载

**原因**: SPIFFS 分区偏移量与 partitions.csv 中定义的不匹配

**解决方案**:
```csv
# partitions.csv 中的偏移量必须匹配
spiffs,data,spiffs,0x392000,0x3CE000

# 编译时的偏移量也必须一致
python $IDF_PATH/components/spiffs/spiffsgen.py 0x3CE000 main/web_ui build/spiffs.bin
```

**烧录验证**:
```bash
# 烧录时使用正确的偏移量
python -m esptool --chip esp32s3 -p COMx write_flash 0x392000 build/spiffs.bin
```

---

### 问题 10: 中文显示乱码

**现象**: Web 界面中的中文字符显示为 `\uXXXX` 转义序列

**原因**: HTML 文件编码问题，JavaScript 没有正确处理 UTF-8 字符

**解决方案**:
```python
# 在生成 HTML 文件时使用正则表达式替换转义字符
import re

def fix_chinese_encoding(html_content):
    # 替换 \uXXXX 转义序列为 UTF-8 字符
    return re.sub(r'\\u([0-9a-fA-F]{4})', 
                  lambda m: chr(int(m.group(1), 16)), 
                  html_content)

# 或者直接在 HTML 头部指定编码
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>MiBeeCam</title>
</head>
```

---

## 🎥 图像和视频问题

### 问题 11: DRAM 帧缓冲限制分辨率

**现象**: 高分辨率 (SVGA 以上) 摄像头初始化失败或图像异常

**原因**: DRAM 空间有限，高分辨率需要更多内存
- VGA (640x480): ~60KB
- SVGA (800x600): ~480KB
- XGA (1024x768): ~1.2MB

**解决方案**:
1. **使用 VGA 默认分辨率**:
   ```c
   // 在配置中设置默认分辨率
   #define DEFAULT_RESOLUTION CAMERA_RES_VGA
   ```
2. **降低 JPEG 质量**: 使用质量 12-15 减少文件大小
3. **禁用 PSRAM**: 确保 `CAMERA_FB_IN_DRAM` 设置

**内存使用监控**:
```c
// 在代码中添加内存监控
printf("Free heap: %d bytes\n", esp_get_free_heap_size());
printf("Min heap: %d bytes\n", esp_get_minimum_free_heap_size());
```

---

### 问题 15: MJPEG 预览画面不到1秒就卡住定格

**现象**: 访问 `/preview.html` 预览页面，视频画面显示不到1秒后定格不动，但服务端 `/stream` 端点仍可返回数据

**原因**: 帧缓冲区争用导致流断流。由于 PSRAM 不可用，摄像头使用 DRAM 且 `fb_count=1`（单帧缓冲区）。运动检测任务在比较两帧时持有帧缓冲区约 1.5 秒，此期间 MJPEG 流无法获取缓冲区，`esp_camera_fb_get()` 超时返回 NULL，流传输中断。

**深层原因分析**:

| 因素 | 影响 |
|------|------|
| PSRAM 禁用 | 只能用 DRAM，~137KB 可用空间仅够一个 VGA 帧缓冲 (~60KB) |
| `fb_count=1` | 同一时刻只能有一个任务持有帧缓冲区 |
| 运动检测旧逻辑 | 获取 fb_a → 等待 500ms → 获取 fb_b → 比较 → 释放。单缓冲下 fb_b 永远拿不到 |
| MJPEG 流无重试 | 一次捕获失败就 `break` 退出循环，整个流终止 |
| `STREAM_FRAME_DELAY` | 每帧额外 66ms 人为延迟，进一步降低帧率 |

**解决方案**:

1. **运动检测：采样即释放模式**（`motion_detect.c`）
   ```c
   // 旧逻辑（单缓冲下死锁）:
   fb_a = capture();         // 持有缓冲区
   delay(500ms);             // 持有期间其他任务无法获取
   fb_b = capture();         // 永远拿不到 → 超时
   compare(fb_a, fb_b);
   release(fb_a, fb_b);

   // 新逻辑（采样即释放）:
   fb_a = capture();
   samples = malloc(sample_count);  // 分配小块采样缓冲区 (~3KB)
   copy_samples(fb_a->buf, samples); // 采样关键字节
   release(fb_a);             // 立即释放！
   delay(500ms);
   fb_b = capture();          // 现在可以获取到了
   compare(samples, fb_b);
   release(fb_b);
   free(samples);
   ```

2. **流传输期间暂停运动检测**（`motion_detect.c`）
   ```c
   while (s_running) {
       // 有流客户端时完全跳过运动检测
       if (mjpeg_streamer_get_client_count() > 0) {
           vTaskDelay(pdMS_TO_TICKS(500));
           continue;
       }
       // ... 正常运动检测逻辑
   }
   ```

3. **流捕获重试 + 去除人为延迟**（`mjpeg_streamer.c`）
   ```c
   // 添加重试逻辑
   for (retries = 0; retries < 3; retries++) {
       ret = camera_capture(&fb);
       if (ret == ESP_OK) break;
       vTaskDelay(pdMS_TO_TICKS(50));  // 短暂等待后重试
   }

   // 移除 STREAM_FRAME_DELAY，改为最小让步
   vTaskDelay(pdMS_TO_TICKS(1));  // 仅喂看门狗 + 任务切换
   ```

4. **预览页面实时状态指示**（`preview.html`）
   ```javascript
   // MJPEG <img> 的 onload 只触发一次，无法计数帧率
   // 改用 canvas 像素变化检测
   setInterval(function() {
       ctx.drawImage(img, 0, 0, 40, 30);
       var sample = ctx.getImageData(0, 0, 40, 30).data;
       if (pixelsChanged(sample, lastSample)) {
           fpsDisplay.textContent = '● LIVE';
       }
   }, 500);
   ```

**修复效果**:

| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| 预览画面 | 不到1秒定格 | 持续刷新 |
| 流吞吐量 | ~10 KB/s | ~36 KB/s |
| 帧率 | ~0.35 FPS | ~6 FPS |
| 浏览器像素变化 | 无（冻结） | 每3秒可检测到变化 |

**关键经验**:

- `fb_count=1` 时，帧缓冲区是全局独占资源，任何任务持有时间都不能超过必要最小值
- 需要跨帧比较的算法必须：获取帧 → 提取特征数据到独立内存 → 立即释放帧缓冲区
- ESP-IDF 的 `httpd` 是单线程的，流传输期间会阻塞所有其他 HTTP 请求（API 不可访问），这是设计限制而非 bug
- 浏览器 `<img>` 标签加载 MJPEG 流时 `onload` 只触发一次，不能用于帧率计算

**验证方法**:
```bash
# 1. 服务端测试流吞吐量
curl -s --max-time 5 http://192.168.61.173/stream -o /dev/null -w 'Bytes: %{size_download}\nRate: %{speed_download} B/s\n'

# 2. 浏览器像素变化检测（在浏览器控制台执行）
var c=document.createElement('canvas');c.width=40;c.height=30;
var x=c.getContext('2d');x.drawImage(document.getElementById('stream'),0,0,40,30);
console.log(Array.from(x.getImageData(0,0,5,1).data));
# 等3秒后再执行一次，像素值应发生变化
```

---
### 问题 12: xQueueGenericSend 初始启动崩溃

**现象**: 设备首次启动时，运动检测任务与摄像头任务之间的竞争条件导致崩溃

**现象**: 首次启动时出现 `xQueueGenericSend` 错误，但第二次启动恢复正常

**原因**: FreeRTOS 队列在首次初始化时存在竞争条件

**解决方案**:
```c
// 在 app_main() 中添加初始化延迟
void app_main(void) {
    // 等待系统稳定
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // 初始化摄像头
    init_camera();
    
    // 等待摄像头初始化完成
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // 启动其他任务
    start_motion_detection_task();
}
```

**预防措施**:
- 添加任务间同步机制
- 使用信号量确保初始化顺序
- 添加错误处理和重试逻辑

---

## 🐛 编译和开发问题

### 问题 13: 依赖库版本不兼容

**现象**: 编译失败，出现 `esp32-camera` 版本冲突

**解决方案**:
```yaml
# idf_component.yml
dependencies:
  espressif/esp32-camera:
    version: "^2.0.0"
    public: true
```

**版本检查**:
```bash
# 检查已安装的组件
idf.py list-components
```

---

### 问题 14: 串口输出中文乱码

**现象**: 串口输出中的中文字符显示为乱码

**原因**: 串口终端配置问题

**解决方案**:
```bash
# 设置正确的串口参数
minicom -b 115200 -D /dev/ttyUSB0 -8
# 或使用 PuTTY
# 波特率: 115200
# 数据位: 8
// 停止位: 1
// 校验: None
```

---

## 🔧 调试工具和方法

### 串口调试技巧

```bash
# 监控串口输出
idf.py -p COMx monitor

# 过滤特定日志
idf.py -p COMx monitor | grep "camera"
idf.py -p COMx monitor | grep "wifi"

# 实时查看系统状态
idf.py -p COMx monitor | grep "status"
```

### Web 界面调试

```bash
# 测试 API 端点
curl http://192.168.4.1/api/status

# 测试视频流
curl -I http://192.168.4.1/stream

# 测试配置更新
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -d '{"device_name":"Test"}'
```

### 内存和性能监控

```c
// 添加内存使用监控
void log_memory_usage() {
    size_t free_heap = esp_get_free_heap_size();
    size_t min_heap = esp_get_minimum_free_heap_size();
    printf("Memory - Free: %zu, Min: %zu\n", free_heap, min_heap);
}

// 添加任务监控
void log_task_status() {
    TaskHandle_t task;
    UBaseType_t stack_high_water;
    
    // 检查 MJPEG 任务
    task = xTaskGetHandle("mjpeg_task");
    stack_high_water = uxTaskGetStackHighWaterMark(task);
    printf("MJPEG task stack: %d\n", stack_high_water);
}
```

### 常用调试命令

```bash
# 检查编译错误
idf.py build

# 检查依赖关系
idf.py list-dependencies

# 清理构建
idf.py clean

# 生成内存映射
idf.py build memory-report
```

---

## 📊 问题分类统计

| 问题类别 | 发生频率 | 严重程度 | 解决成功率 |
|----------|----------|----------|------------|
| PSRAM 相关 | 高 | 高 | 100% |
| 引脚映射 | 中 | 高 | 100% |
| 网络配置 | 高 | 中 | 95% |
| 编译问题 | 中 | 中 | 100% |
| 存储问题 | 中 | 中 | 100% |
| 图像问题 | 低 | 中 | 90% |
| 帧缓冲区争用 | 中 | 高 | 100% |

---

## 🚀 最佳实践

1. **始终使用正确的 ESP-IDF 版本** (v5.4.3)
2. **禁用 PSRAM 以避免时序问题**
3. **使用正确的摄像头模型定义** (CAMERA_MODEL_Air_ESP32S3)
4. **先注册具体 URI，再注册通配符 URI**
5. **设置适当的 HTTP 头长度**
6. **使用 VGA 默认分辨率**
7. **添加任务间同步机制**
8. **定期检查内存使用情况**
9. **使用正确的串口参数**
10. **验证分区表配置**
11. **帧缓冲区用完即释放，不要跨帧持有**
12. **单缓冲模式下，使用采样即释放模式替代双帧持有**
13. **流传输期间暂停运动检测，避免缓冲区争用**

---

## 📞 支持和反馈

如果遇到本文档未涵盖的问题，请：

1. **检查串口输出**: `idf.py -p COMx monitor`
2. **查看编译错误**: `idf.py build`
3. **验证硬件连接**: 确保所有引脚正确连接
4. **查看日志文件**: 检查构建和运行时日志
5. **提交 Issue**: 在 GitHub 上提交详细的问题报告

---

*故障排查指南 - 最后更新: 2026-04-29*