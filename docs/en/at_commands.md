# AT Command Reference

## Overview

Brief description: AT command interface over UART0 (115200 baud), 20 commands for WiFi config, system info, camera control.

## Usage

- Connect via USB serial (115200 8N1)
- Send commands terminated with \r\n
- Responses use standard AT format: \r\nOK\r\n or \r\nERROR\r\n
- Log output may interleave with AT responses

## Command List

### Basic Commands

#### AT
Test command.
Response: \r\nOK\r\n

#### AT+RST
Reboot device.
Response: \r\nOK\r\n (then device reboots)

#### AT+GMR
Get firmware version and chip info.
Response:
\r\n
Firmware: MiBeeCam v0.2.0\r\n
Build: <date> <time>\r\n
IDF: <version>\r\n
Chip: ESP32-S3 Rev <n>\r\n
\r\nOK\r\n

#### AT+RESTORE
Factory reset + reboot.
Response: \r\nOK\r\n (then device reboots with default config)

### System Commands

#### AT+HEAP
Get heap memory info.
Response:
\r\n
Free: <bytes> bytes\r\n
Min: <bytes> bytes\r\n
Baseline: <bytes> bytes\r\n
\r\nOK\r\n

#### AT+UPTIME
Get uptime.
Response:
\r\n
Uptime: <seconds> seconds\r\n
\r\nOK\r\n

#### AT+TEMP
Get chip temperature.
Response:
\r\n
Temp: <temp> C\r\n
\r\nOK\r\n

### WiFi Commands

#### AT+CWJAP?
Query current WiFi status.
Response:
\r\n
State: <AP|CONNECTING|CONNECTED|DISCONNECTED|FAILED>\r\n
SSID: <ssid>\r\n
IP: <ip>\r\n
\r\nOK\r\n

#### AT+CWJAP=<ssid>,<pwd>
Set WiFi credentials and connect.
Parameters: ssid (max 32 chars), pwd (max 64 chars)
Quotes optional: AT+CWJAP="MyNet","MyPass" or AT+CWJAP=MyNet,MyPass
Response: \r\nOK\r\n on success, \r\nERROR\r\n on failure
Note: Config saved in-memory. Use AT+SAVE to persist.

#### AT+CWJAP2?
Query backup WiFi (SSID2) configuration.
Response:
\r\n+CWJAP2:<ssid>\r\n\r\nOK\r\n
Note: Password not shown for security. Shows "(not set)" if backup SSID is empty.

#### AT+CWJAP2=<ssid>,<pwd>
Set backup WiFi credentials. Used for auto-fallback when primary WiFi fails.
Parameters: ssid (max 32 chars), pwd (max 64 chars)
Response: \r\nOK\r\n on success, \r\nERROR\r\n on failure
Note: Config saved in-memory. Use AT+SAVE to persist.
#### AT+CWQAP
Disconnect WiFi and switch to AP mode.
Response: \r\nOK\r\n

#### AT+CIFSR
Get IP address.
Response: \r\n+CIFSR:<ip>\r\n\r\nOK\r\n

#### AT+CWLAP
Scan nearby WiFi networks (blocking, ~1-2s).
Response (per network):
\r\n+CWLAP:<ssid>,<rssi>,<authmode>\r\n
...
\r\nOK\r\n

### Device Name

#### AT+NAME?
Get device name.
Response: \r\n+NAME:<name>\r\n\r\nOK\r\n

#### AT+NAME=<name>
Set device name (max 32 chars, in-memory).
Response: \r\nOK\r\n

### Config Commands

#### AT+CFGGET=<field>
Get config field value.
Fields: wifi_ssid, wifi_pass, wifi_ssid2, wifi_pass2, device_name, server_url, timezone, web_password, mdns_hostname, webhook_url, resolution, fps, jpeg_quality, motion_threshold, motion_cooldown, onvif_enabled, ws_enabled
Response: \r\n+CFGGET:<field>=<value>\r\n\r\nOK\r\n

#### AT+CFGSET=<field>,<value>
Set config field value (in-memory).
Same fields as CFGGET. Numeric validation: resolution(0-3), fps(1-30), jpeg_quality(1-63), etc.
Response: \r\nOK\r\n

#### AT+SAVE
Save current config to NVS (persistent).
Response: \r\nOK\r\n on success, \r\nERROR\r\n on NVS failure

### Stream Status

#### AT+STREAM?
Get streaming and motion detection status.
Response:
\r\n
Stream clients: <count>\r\n
Motion detect: <running|stopped>\r\n
\r\nOK\r\n

## Examples

### Configure WiFi Headlessly
```
AT+CWJAP=MyHomeWiFi,MyPassword
AT+SAVE
AT+RST
```

### Check Device Status
```
AT+CWJAP?
AT+CIFSR
AT+HEAP
AT+TEMP
AT+STREAM?
```

### Change Camera Quality
```
AT+CFGSET=jpeg_quality,10
AT+CFGSET=resolution,1
AT+SAVE
AT+RST
```