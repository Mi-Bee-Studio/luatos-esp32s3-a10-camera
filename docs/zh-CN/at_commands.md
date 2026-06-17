# AT 指令手册

## 概述

简短描述：通过 UART0（115200 波特率）的 AT 指令接口，20 个指令用于 WiFi 配置、系统信息、摄像头控制。

## 使用方法

- 通过 USB 串口连接（115200 8N1）
- 发送指令以 \r\n 结尾
- 响应使用标准 AT 格式：\r\nOK\r\n 或 \r\nERROR\r\n
- 日志输出可能与 AT 响应交错

## 指令列表

### 基础指令

#### AT
测试指令。
响应：\r\nOK\r\n

#### AT+RST
重启设备。
响应：\r\nOK\r\n（然后设备重启）

#### AT+GMR
获取固件版本和芯片信息。
响应：
\r\n
Firmware: MiBeeCam v0.2.0\r\n
Build: <日期> <时间>\r\n
IDF: <版本>\r\n
Chip: ESP32-S3 Rev <n>\r\n
\r\nOK\r\n

#### AT+RESTORE
恢复出厂设置并重启。
响应：\r\nOK\r\n（然后设备以默认配置重启）

### 系统指令

#### AT+HEAP
获取堆内存信息。
响应：
\r\n
Free: <字节数> bytes\r\n
Min: <字节数> bytes\r\n
Baseline: <字节数> bytes\r\n
\r\nOK\r\n

#### AT+UPTIME
获取运行时间。
响应：
\r\n
Uptime: <秒数> seconds\r\n
\r\nOK\r\n

#### AT+TEMP
获取芯片温度。
响应：
\r\n
Temp: <温度> C\r\n
\r\nOK\r\n

### WiFi 指令

#### AT+CWJAP?
查询当前 WiFi 状态。
响应：
\r\n
State: <AP|CONNECTING|CONNECTED|DISCONNECTED|FAILED>\r\n
SSID: <ssid>\r\n
IP: <ip>\r\n
\r\nOK\r\n

#### AT+CWJAP=<ssid>,<pwd>
设置 WiFi 凭证并连接。
参数：ssid（最多 32 字符），pwd（最多 64 字符）
引号可选：AT+CWJAP="MyNet","MyPass" 或 AT+CWJAP=MyNet,MyPass
响应：成功时 \r\nOK\r\n，失败时 \r\nERROR\r\n
注意：配置保存在内存中。使用 AT+SAVE 持久化。

#### AT+CWJAP2?
查询备用 WiFi（SSID2）配置。
响应：\r\n+CWJAP2:<ssid>\r\n\r\nOK\r\n
注意：出于安全考虑不显示密码。备用 SSID 为空时显示 "(not set)"。

#### AT+CWJAP2=<ssid>,<pwd>
设置备用 WiFi 凭证。主 WiFi 连接失败时自动回退使用。
参数：ssid（最多 32 字符），pwd（最多 64 字符）
响应：成功时 \r\nOK\r\n，失败时 \r\nERROR\r\n
注意：配置保存在内存中。使用 AT+SAVE 持久化。
#### AT+CWQAP
断开 WiFi 并切换到 AP 模式。
响应：\r\nOK\r\n

#### AT+CIFSR
获取 IP 地址。
响应：\r\n+CIFSR:<ip>\r\n\r\nOK\r\n

#### AT+CWLAP
扫描附近的 WiFi 网络（阻塞，约 1-2 秒）。
响应（每个网络）：
\r\n+CWLAP:<ssid>,<rssi>,<authmode>\r\n
...
\r\nOK\r\n

### 设备名称

#### AT+NAME?
获取设备名称。
响应：\r\n+NAME:<name>\r\n\r\nOK\r\n

#### AT+NAME=<name>
设置设备名称（最多 32 字符，内存中）。
响应：\r\nOK\r\n

### 配置指令

#### AT+CFGGET=<field>
获取配置字段值。
字段：wifi_ssid, wifi_pass, wifi_ssid2, wifi_pass2, device_name, server_url, timezone, web_password, mdns_hostname, webhook_url, resolution, fps, jpeg_quality, motion_threshold, motion_cooldown, onvif_enabled, ws_enabled
响应：\r\n+CFGGET:<field>=<value>\r\n\r\nOK\r\n

#### AT+CFGSET=<field>,<value>
设置配置字段值（内存中）。
字段与 CFGGET 相同。数值验证：resolution(0-3), fps(1-30), jpeg_quality(1-63), 等。
响应：\r\nOK\r\n

#### AT+SAVE
保存当前配置到 NVS（持久化）。
响应：成功时 \r\nOK\r\n，NVS 失败时 \r\nERROR\r\n

### 流状态

#### AT+STREAM?
获取流传输和运动检测状态。
响应：
\r\n
Stream clients: <客户端数>\r\n
Motion detect: <running|stopped>\r\n
\r\nOK\r\n

## 示例

### 无头配置 WiFi
```
AT+CWJAP=MyHomeWiFi,MyPassword
AT+SAVE
AT+RST
```

### 检查设备状态
```
AT+CWJAP?
AT+CIFSR
AT+HEAP
AT+TEMP
AT+STREAM?
```

### 更改摄像头质量
```
AT+CFGSET=jpeg_quality,10
AT+CFGSET=resolution,1
AT+SAVE
AT+RST
```