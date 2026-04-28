# MiBeeCam 硬件配置文档

## 📱 硬件概述

MiBeeCam 基于 ESP32-S3-A10 开发板构建，采用 OV2640 摄像头传感器，提供高性能的视频采集和智能监控功能。
<p align="center">
    <img src="../images/luatos-esp32s3-a10.png" alt="ESP32-S3-A10 开发板" width="480">
</p>

## 🔧 核心硬件规格

| 组件 | 规格 | 说明 |
|------|------|------|
| **处理器** | ESP32-S3 双核 | 240MHz 主频，WiFi + 蓝牙 5.0 |
| **Flash 存储** | 16MB | 程序存储和数据存储 |
| **PSRAM** | 8MB Octal | 物理存在但禁用（时序问题） |
| **摄像头** | OV2640 传感器 | 8225N 模块，支持多种分辨率 |
| **LED 指示灯** | GPIO 10 | 状态指示，50ms 闪烁模式 |
| **复位按钮** | GPIO 0 | 长按 5 秒执行工厂重置 |

## 🎥 摄像头配置

### 摄像头型号

```c
// 固定使用 ESP32-S3-A10 专用型号
#define CAMERA_MODEL_Air_ESP32S3
```

### 引脚映射 (camera_driver.c)

| 功能 | GPIO 引脚 | 说明 |
|------|-----------|------|
| **XCLK** | 39 | LEDC 方法生成时钟信号 |
| **SIOD** | 21 | I2C 数据线 |
| **SIOC** | 46 | I2C 时钟线 |
| **D0** | 34 | 数据位 0 |
| **D1** | 47 | 数据位 1 |
| **D2** | 48 | 数据位 2 |
| **D3** | 33 | 数据位 3 |
| **D4** | 35 | 数据位 4 |
| **D5** | 37 | 数据位 5 |
| **D6** | 38 | 数据位 6 |
| **D7** | 40 | 数据位 7 |
| **VSYNC** | 42 | 垂直同步信号 |
| **HREF** | 41 | 水平参考信号 |
| **PCLK** | 36 | 像素时钟信号 |
| **PWDN** | -1 | 电源控制（未使用） |
| **RESET** | -1 | 复位信号（未使用） |

### 重要技术细节

**XCLK 时钟信号**:
- 不能使用 GPIO 矩阵方法
- 必须使用 LEDC 方法：`s3_enable_xclk()`
- 频率：10MHz

**数据位排列**:
- 数据线按照 ESP32-S3-A10 特定的 GPIO 映射
- 与 LuatOS 文档中的引脚不同，必须使用此映射

## 💾 存储配置

### 分区表 (partitions.csv)

| 分区 | 类型 | 子类型 | 偏移量 | 大小 | 用途 |
|------|------|--------|--------|------|------|
| nvs | data | nvs | 0x9000 | 24KB | WiFi 配置等非易失性数据 |
| phy_init | data | phy | 0xf000 | 4KB | PHY 初始化数据 |
| factory | app | factory | 0x10000 | 3.5MB | 主程序固件 |
| otadata | data | ota | 0x390000 | 8KB | OTA 更新数据 |
| spiffs | data | spiffs | 0x392000 | ~3.94MB | Web 界面文件 |

### SPIFFS 文件系统

- **偏移量**: 0x392000
- **大小**: 0x3CE000 (~3.94MB)
- **源文件**: `main/web_ui/`
- **包含文件**:
  - `index.html` - 主界面
  - `preview.html` - 预览页面
  - `config.html` - 配置页面
- **最大文件数**: 5
- **自动格式化**: 挂载失败时自动格式化

### 分区生成和烧录

```bash
# 生成 SPIFFS 镜像
python $IDF_PATH/components/spiffs/spiffsgen.py 0x3CE000 main/web_ui build/spiffs.bin

# 烧录 SPIFFS 镜像
python -m esptool --chip esp32s3 -p COMx write_flash 0x392000 build/spiffs.bin
```

## 🌐 WiFi 配置

### 支持 2.4GHz 频段
- **不支持 5GHz**（硬件限制）
- **STA 模式**: 优先连接已配置的 WiFi 网络
- **AP 模式**: WiFi 配置失败时启动热点

### AP 模式默认配置

| 配置项 | 值 |
|--------|-----|
| SSID | "MiBeeCam" |
| 密码 | "12345678" |
| IP 地址 | 192.168.4.1 |

## 🔌 状态指示 LED

### LED 引脚配置
- **GPIO 10**: 状态指示灯
- **闪烁模式**: 50ms 定时器控制

### LED 状态含义

| 状态 | 说明 | 频率 |
|------|------|------|
| LED_STARTING | 系统启动 | 快速闪烁 |
| LED_WIFI_CONNECTING | WiFi 连接中 | 中速闪烁 |
| LED_RUNNING | 系统正常运行 | 缓慢闪烁 |
| LED_ERROR | 错误状态 | 常亮 |
| LED_AP_MODE | AP 热点激活 | 快速闪烁 |

## ⚡ 电源要求

- **输入电压**: 5V
- **电流需求**: 
  - 空闲: ~150mA
  - 摄像头工作: ~300mA
  - 最大峰值: ~500mA

## 📡 串口配置

| 参数 | 值 |
|------|-----|
| 波特率 | 115200 |
| 数据位 | 8 |
| 停止位 | 1 |
| 校验位 | None |
| 流控制 | None |

## 🛠️ 调试接口

### BOOT 按钮功能
- **短按**: 无特殊功能
- **长按 5 秒**: 执行工厂重置
- **GPIO 0**: 复位信号输入

### 工厂重置流程
1. 检测 BOOT 按钮按下
2. 等待 5 秒确认
3. 清除 NVS 配置数据
4. 重启设备
5. 恢复默认配置

## 🔧 开发调试

### 调试引脚可用
- GPIO 0: BOOT 按钮
- GPIO 10: LED 状态指示
- 串口: 调试信息输出

### 重要注意事项

1. **PSRAM 问题**: 8MB PSRAM 物理存在但禁用，所有数据存储在 DRAM
2. **摄像头引脚**: 必须使用 CAMERA_MODEL_Air_ESP32S3 的引脚映射
3. **Flash 大小**: 16MB 不是 8MB
4. **分辨率限制**: VGA (640x480) 为默认分辨率，受 DRAM 限制
5. **XCLK 时钟**: 必须使用 LEDC 方法，不能使用 GPIO 矩阵

---

*硬件配置文档 - 最后更新: 2026-04-29*