# AGENTS.md — ESP32-S3-A10 CAM

MiBeeCam firmware for the **LuatOS ESP32-S3-A10** board (OV2640 / 8225N module). C, ESP-IDF. Web UI + MJPEG streamer + motion detection + ONVIF Profile S, all in a single ESP32-S3 image.

> **Sister projects**: `ai-thinker-esp32-cam`, `seeed-esp32s3-cam`, `esp32s3-n16r8-cam` (same Mi-Bee-Studio family). This is the **budget single-OTA variant** — do NOT copy their PSRAM config, ESP-IDF version, partition tables, or pin maps. See "What makes this board different" below.

## AT command interface (family contract v1.0, 2026-09-04)

统一契约：`docs/at-command.md`（四仓 md5 一致，地位同 api-contract）。核心集：
`AT / AT+HELP / AT+GMR / AT+STATUS / AT+WIFI?|= / AT+IP? / AT+CAMRES?|= / AT+CAMQUAL?|= /
AT+REBOOT / AT+RESTORE`（+能力裁剪项）。红线：**任何读指令不回显密码**；CAMQUAL 边界
10-63（PIT-021）。本板串口 /dev/ttyACM1（CH343，开串口即复位）。CAMRES 仅 VGA（DRAM）、CAMQUAL 保存+重启；CWJAP/CWLAP 等为历史别名；AT+CFGGET 已对 *pass*/*secret* 键脱敏（2026-09-04 前可明文读 wifi_pass，曾用于一次性板间凭据迁移后即封）；实现于 main/at_command.c。

## What makes this board different (read first)

This project diverges from the other S3 repos in ways that will silently break a build or boot if ignored:

| Concern | This repo | The other S3 repos (`seeed`, `esp32s3-n16r8`) |
|---------|-----------|-----------------------------------------------|
| **ESP-IDF** | **v5.5.4** (pinned) | v6.0.1 |
| **PSRAM** | **DISABLED** (`# CONFIG_SPIRAM is not set`) | Octal PSRAM mandatory |
| **Frame buffers** | DRAM only, `fb_count=1` | PSRAM, `fb_count=2` |
| **Partitions** | Single `factory` slot (3.5 MB) + huge SPIFFS (~3.94 MB) | Dual OTA slots |
| **esp32-camera** | `^2.0.0` | `^2.1.6` |
| **CMake project name** | `esp32s3_a10_camera` | `mibee_cam` |

**Why PSRAM is disabled:** Octal PSRAM timing tuning failed on this board → boot loop. Camera runs from internal DRAM with `fb_count=1`. Re-enabling PSRAM without solving the timing will boot-loop the device. Do not "fix" it by copying the seeed/n16r8 PSRAM block.

**Why v5.5.4:** v6.0 has known PSRAM issues with this board (even though PSRAM is off here, the toolchain pin keeps CI reproducible and matches the working local config). README explicitly warns against v6.0.

## Toolchain

```bash
# Activate ESP-IDF v5.5.4 — NOT v6.0.1
source ~/.espressif/v5.5.4/esp-idf/export.sh
idf.py --version   # → ESP-IDF v5.5.4
```

The v6.0.1 environment used by the sister repos will **not** reproduce this build's behavior.

## Build / flash / SPIFFS

> **Flashing policy note (root AGENTS.md, 2026-09-04)**: the family rule is
> Web-OTA-first; this repo is the **permanent exception** — single factory partition,
> no OTA endpoint by design. USB below is the only delivery path here.

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

**SPIFFS is NOT auto-packaged.** Unlike the sister repos (which use `spiffs_create_partition_image(... FLASH_IN_PROJECT)`), the web UI image must be generated and flashed manually:

```bash
# Generate SPIFFS image (size MUST match partitions.csv spiffs entry: 0x3CE000)
python $IDF_PATH/components/spiffs/spiffsgen.py 0x3CE000 main/web_ui build/spiffs.bin
# Flash to SPIFFS partition at 0x392000
python -m esptool --chip esp32s3 -p /dev/ttyUSB0 write_flash 0x392000 build/spiffs.bin
```

- **Serial port**: `/dev/ttyUSB0` (not `ttyACM0` like XIAO/GOOUUU boards). 115200 baud.
- **`sdkconfig` (generated) and `managed_components/` are gitignored.** After editing `sdkconfig.defaults`, delete `sdkconfig` or it silently overrides defaults.
- Clean rebuild: `idf.py fullclean && idf.py set-target esp32s3 && idf.py build`.

## Structure

18 modules (flat `1:1 .c/.h`) + vendored cJSON in `main/`:

```
main/
├── main.c              # 15-step boot sequence (camera BEFORE WiFi — I2C conflict)
├── camera_driver.*     # OV2640 (8225N), JPEG, resolution control
├── wifi_manager.*      # STA/AP, backup-SSID auto-fallback, AMPDU disabled
├── config_manager.*    # 契约 v1.0 逐键 NVS（mibee_cfg + schema_ver），legacy blob 一次性迁移
├── mjpeg_streamer.*    # Port 81 independent TCP server, multipart/x-mixed-replace
├── web_server.*        # Port 80 HTTP, REST API, SPIFFS static, WebSocket (/ws)
├── motion_detect.*     # Frame-difference + auto JPEG upload
├── onvif_service.* + onvif_discovery.*  # ONVIF Profile S (WS-Discovery + SOAP)
├── frame_broadcaster.* # DRAM frame cache, reference counting (fb_count=1 → single consumer pattern)
├── event_bus.*         # In-memory pub/sub, 9 event types
├── at_command.*        # UART0, 20 AT commands
├── health_monitor.*    # Prometheus /metrics, 60s interval
├── time_sync.*         # NTP via pool.ntp.org
├── status_led.*        # GPIO 10
├── webhook.*           # HTTP event forwarding
└── web_ui/             # index.html / preview.html / config.html (SPIFFS)
```

## Boot order (non-obvious)

**Camera initializes BEFORE WiFi.** The I2C/SCCB bus conflicts if WiFi grabs it first. Sequence:
NVS → Config load → LED → SPIFFS → **Camera** → WiFi → Health → Mode select → (STA: connect→streamer→web→NTP→motion→button→AT) or (AP: web only).

This is the **opposite** of `ai-thinker-esp32-cam` (which defers camera until after WiFi STA connect, for a DMA freeze workaround). Different boards, different bugs — don't unify.

## Config flags (compile-time, Kconfig)

Feature toggles live in `main/Kconfig.projbuild` and are mirrored in `sdkconfig.defaults`. All default ON:
`MIBEECAM_ENABLE_WEBHOOK`, `MIBEECAM_ENABLE_ONVIF`, `MIBEECAM_ENABLE_WS`, `MIBEECAM_ENABLE_FRAME_BROADCASTER`, `MIBEECAM_ENABLE_BACKUP_SSID`, `MIBEECAM_ENABLE_MDNS`, `MIBEECAM_ENABLE_WIFI_SCAN`.

To disable a feature: set `=n` in `sdkconfig.defaults`, delete `sdkconfig`, rebuild.

## WiFi quirks (from sdkconfig.defaults)

- **WPA3 fully disabled** (SAE auth issues on this board): WPA3_SAE, SAE_PK, SAE_H2E, SOFTAP_SAE, WPA3_OWE all off.
- **AMPDU TX/RX 重新启用（2026-09-03 晚，已上板复验）**：原先 =n 是绕驱动 stall 的权宜，
  但实测 HT40/-70dBm 场景下链路崩塌（ping 1-2.4s、HTTP 8s+）。seeed 现行配置就是
  AMPDU ON。重开后 `<ba-add>` 会话建立、ping 317ms→5ms、落点 MiBeeAP2 ch2 BW20 -59dBm。
  若串口再见 stall 可回退（defaults 里有注释）。
- **STA 强制 HT20**（`wifi_start_sta` 里 `esp_wifi_set_bandwidth`）：本板曾与 HT40 AP
  （ch11）谈到 40MHz，弱信号下 PER 恶化；HT20 灵敏度好 ~3dB（ai-thinker 同款）。
- Default AP on first boot: SSID `MiBeeCam`, password `12345678`, config at `http://192.168.4.1`.
- **status 新字段（2026-09-03 晚）**：`current_ssid`（实连 SSID）、`wifi_net`
  （primary|secondary，映射自 active_ssid_index）——SPA 顶栏/WiFi 页当前连接行依赖它们。
- **health 探测防误杀（PIT-002）**：WiFi 未连接时探测失败不计数（此前 4/4 无条件重启，
  掉线 120s 会被翻译成重启）。

## Frame-buffer constraint (fb_count=1)

With PSRAM off and only one DRAM framebuffer, motion detection and streaming contend for the same frame. Code uses a **sample-and-release pattern** in `frame_broadcaster` + pause-during-stream in motion detect to avoid contention. Raising `fb_count` requires PSRAM, which is disabled — don't try.

## Factory reset

Hold **BOOT** (GPIO 0) for 5 s → clears NVS, reboots into AP mode. (On `ai-thinker-esp32-cam`, GPIO0 is camera XCLK and unusable — different board, different choice.)

## Conventions

- `snake_case` functions/vars, `s_` static prefix, `ESP_LOGx(TAG, ...)`, `esp_err_t` returns.
- `sdkconfig.defaults` is the single source of truth for board config; pin numbers live in `camera_driver.c` via `CAMERA_MODEL_Air_ESP32S3` (the `esp32-camera` Air ESP32-S3 pin def — **not** the LuatOS documentation pinout).
- Config persisted as per-key NVS in namespace `mibee_cfg` with `schema_ver=1`（契约 v1.0,
  `docs/config-contract.md`）；legacy blob（`mibee_cfg/config` 或前代 `device_cfg/config`，
  v1/v2/v3）由 `config_init` 一次性迁移（`config_bak` 备份留存）。字段名 = 契约 JSON 名
  （`cam_framesize`=framesize_t 刻度、`cam_quality`、`cam_fps`、`onvif_enable`、motion
  超集 `motion_sensitivity` 等）。

> **2026-09-04 摄像头配置边界补全（已烧录验证）**：实测 VGA q10/q8/q6 全健康
> （q6 帧也才 22.6KB，但帧大小随场景复杂度浮动），家族统一 `CAMERA_QUALITY_MIN=10`
> （esp32-camera JPEG fb 按 w*h/5=61KB 分配，q<10 复杂场景超预算截帧，PIT-021）。
> 补了两个漏洞：**POST 画质此前完全无校验**（uint8 裸截断，q0/q300 全收）→ 现 10-63
> 越界 400；**/api/config 的摄像头键此前仍走热重配**（camera_deinit+init 并发竞态，
> 即 2026-09-03 实测致设备静默死亡的同款路径，/api/camera 已修而此处漏网）→ 现与
> /api/camera 对齐：校验+保存+应答 `rebooting:true`+1s 重启；同值 POST 不重启。
> NVS 加载钳制旧值（q<10→10，分辨率非 VGA→VGA）。`GET /api/camera` 新增
> `quality_min/quality_max`，SPA 滑杆据此钳制。
>
> **2026-09-04 分辨率三层上限（家族统一，已烧录验证）**：上限从 web_server 的
> 静态 VGA 表改为 `camera_get_effective_max_res() = min(sensor, board, memory)`
> （camera_driver.c/h，细节见 PITFALLS PIT-021 附录）。board 层常数
> `CAMERA_RES_BOARD_MAX=VGA`（PIT-012 实测：SVGA 触发堆枯竭螺旋，"能出图"≠
> "能稳定带流"）；memory 层为内部 DRAM fb 预算校验（floor=32K 按 PIT-012
> 实测校准，稳态独立复算出同样的 VGA；未来若出现内存宽裕板型此层自动放宽，
> 但放宽 board 层前必须按家族流程重测）；sensor 层查组件能力表（OV2640→UXGA，
> 换传感器自适应）。`GET /api/camera` 下发 `res_cap_source`（本板=board）；
> POST /api/camera、/api/config、AT+CAMRES 校验从 `!=0` 硬编码改为 effective。
> 另：仓内 esp32-camera 实际版本是 `~2.1.7`（下方差异表 "^2.0.0" 为旧信息）。

## REST API Endpoints

> **2026-09-02 契约 v1.0 统一化**（权威规范：`docs/api-contract.md`，下表已部分过时）：
> 新增 `GET /api/auth`、`POST /api/time`；移除 `/api/led`、`/api/ai/status` 桩端点（能力位 false 不注册）；
> status 字段对齐契约（`cam_framesize`(str)→`resolution`、`heap_free/heap_min`→`free_heap/min_heap`，
> 新增 `device_name`/`firmware_version`/`stream_clients`/`stream_clients_max`）；
> config 键改名 `wifi_ssid_2/wifi_pass_2/onvif_enable`；WS 事件改契约名
> （`motion_started/motion_cleared`）且格式补 `data` 字段；`/api/camera` 返回 `supported_resolutions`(0-3)。
> **重大修复**：通配符匹配下原 `GET /*` 先注册导致 `/api/camera`、`/ws` 运行时 404 —— 已重排
> 注册顺序（精确端点 → /ws → 静态兜底），勿回退。
> Web UI 已换统一 SPA **v3 "Honey"** 四文件（与 n16r8/seeed md5 一致）；**v1.1 起 SPIFFS 已改为
> 构建期自动打包**（根 CMakeLists `spiffs_create_partition_image`），手动 spiffsgen 流程作废，
> `idf.py flash` 即含 UI。
>
> **契约 v1.1（2026-09-02）**：公开默认密码统一为 `mibeecam2026`（Kconfig 默认值，可入文档；本地部署可在 gitignored sdkconfig 用 `CONFIG_MIBEE_CAM_DEFAULT_WEB_PASSWORD` 覆盖；空密码加载自动迁移）、拒绝 <6 位密码；
> api_version=1.1。
> 改 UI 后必须重新生成并烧写 spiffs 分区。
>
> **2026-09-03 上板部署会话（5 项修复 + 2 个教训）**：
> 1. **event_bus 订阅表 8→16**：v1.1 的 9 个 WS 事件订阅 + webhook 订阅 > 8，启动即
>    `subscription table full`，部分 WS 事件静默丢失。改 `MAX_SUBSCRIPTIONS`。
> 2. **health 日志 uptime 用 esp_timer**：原 `time(NULL)` 在 SNTP 同步后打印 epoch 大数
>    （`Uptime: 1788399188`）。`/api/status` 与 `/metrics` 一直是对的（esp_timer）。
> 3. **MJPEG 堆水位准入门**：本板无 PSRAM，双客户端拉流实测 min_heap 压到 **108~168B**，
>    取帧分配失败→流任务自断（用户侧死帧）。`MJPEG_2ND_CLIENT_HEAP_FLOOR=30KB`：单客户端
>    稳态 free≈25K，故第 2 路实际恒 503、SPA 退避重试——**本板事实单流**（与 ai-thinker 同档）。
>    首路永不限制。门槛在 LRU 踢除之前判断。修复后 150s 长稳 min_heap 稳定 4.3~5.4KB。
> 4. **EMFILE 复发（同 seeed 病根）**：被门槛拒绝的浏览器每 30s 重连，503+close 留 TIME_WAIT，
>    lwIP 10 插座 + MSL 60s → 表满，httpd 无法 accept（`error in accept (23)`，API 间歇无响应）。
>    `LWIP_MAX_SOCKETS=16` + `LWIP_TCP_MSL=15000`。**改 sdkconfig.defaults 后必须删 sdkconfig 重配**。
> 5. **health 警告阈值 30KB→15KB**：单流稳态 free≈25K，30KB 阈值每 30s 误报+WS 刷屏。
> 6. SPA `.video-stage`：`max-height:58vh` 会把 4:3 容器钳成 ~2:1（4:3 传感器画面左右黑边），
>    改 `width: min(100%, calc(58vh * 4/3))`——**已三仓同步**，勿回退。
> 教训：测试时若发现"神秘 MJPEG 客户端"占着槽位——是浏览器 SPA 的 img 自愈重连（被踢 ~7s
> 即回，重启后也会抢回槽位），不是泄漏；用户开着页面时设备永远有 1 路流在跑。
>
> **2026-09-03 下午浏览器全面测试追加（4 项，均已烧录验证）**：
> 1. **CH343 open-reset 陷阱（重要）**：在宿主机上**任何一次 open(/dev/ttyACM1) 都会复位本板**
>    （CH343 驱动在 open 时断言复位线，`rts=False,dtr=False` 构造参数拦不住，实测 uptime 52→16）。
>    ai-thinker 的 CH340 同样中招。seeed 的 Espressif 原生 USB-JTAG（ttyACM0）无此问题。
>    观察本板串口必须走**常驻采集器**（tools/overnight_log.py，端口已改 ACM1），
>    绝不要临时开串口——那会把"自发性重启"和"自己造成的重启"搅在一起（今日教训）。
> 2. **POST /api/camera 热重配致命竞态**：原实现裸调 camera_deinit+init，MJPEG/motion/广播
>    并发取帧 → 设备级静默死亡（今日 3 次实录：写入后 ~10s USB 消失）。已改为
>    **保存+应答+1s 后重启应用**（`{"status":"saved (rebooting to apply)","rebooting":true}`）。
>    姐妹板（PSRAM fb_count=2）不受此限仍热重配——这是本板单板差异。
> 3. **硬单流定论**：30K/34K 堆门槛都会被开机初期高堆瞬时值（37K+）绕过，双流实测
>    min_heap 108~1276B + 摄像头互斥锁获取失败 + httpd 卡死。`MAX_STREAM_CLIENTS=1`
>    （新观众 LRU 踢旧观众，SPA ~7s 自愈重连，与 ai-thinker 同模型），status
>    `stream_clients_max` 同步=1。堆门槛 34K 保留作纵深防御。单流稳态 free 21~42K。
> 4. 共享 UI 修复随本轮上线：canvas 跨源污染（img.crossOrigin='anonymous'，服务端已有 ACAO）、
>    静态资源 no-cache、api() 401 重试旧 headers、video-stage 动态宽高比。三仓 md5 一致。

All business endpoints use the `/api/` prefix. Returns JSON envelope `{"ok":true,"data":...}` on success, `{"ok":false,"error":"..."}` on failure.

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| GET | `/api/status` | open | Device status (WiFi, camera, system) |
| GET | `/api/config` | open | Current configuration (passwords masked) |
| POST | `/api/config` | write | Partial config update; first-time password setup when `web_password` is empty |
| GET | `/api/capabilities` | open | Board capability flags (12 booleans) |
| GET | `/api/capture` | open | Single JPEG snapshot (`image/jpeg`, not JSON) |
| GET | `/api/wifi/scan` | open | WiFi AP scan (guarded by `CONFIG_MIBEECAM_ENABLE_WIFI_SCAN`) |
| POST | `/api/reset` | write | Factory reset config to defaults |
| POST | `/api/reboot` | write | Reboot device |
| OPTIONS | `/*` | — | CORS preflight (204 No Content) |

**Auth:** `X-Password` header for write operations. When `web_password` is empty (first boot), all writes return 401 `SET_PASSWORD_FIRST` except `POST /api/config` with a `web_password` field (first-time setup).

**MJPEG stream:** Separate TCP server on port `:81` (independent of main web server on port 80).

**Exempt paths:** `/metrics` (Prometheus), `/ws` (WebSocket), `/onvif/*` (SOAP) are not under `/api/`.

## Web UI

Single-page application served from SPIFFS. Four files:
- `index.html` — page structure
- `app.js` — logic and API calls
- `style.css` — light/dark theme styles
- `i18n.js` — zh/en bilingual translations (auto-detect, persisted in localStorage)

Controls are shown or hidden based on `GET /api/capabilities`. The SPA baseline originates from `esp32s3-n16r8-cam` and is ported to all boards.

## Capabilities

This board returns the following from `GET /api/capabilities`:

| Capability | Supported |
|------------|-----------|
| ai | ❌ |
| sd | ❌ |
| audio | ❌ |
| ota | ❌ |
| mic | ❌ |
| flash_led | ❌ |
| recording | ❌ |
| timelapse | ❌ |
| onvif | ✅ |
| rtsp | ❌ |
| websocket | ✅ |
| mdns | ✅ |

## CI / release

`.github/workflows/build.yml` — `espressif/idf:v5.5.4` container, builds on push/PR, releases on `v*` tags. Release assets: `bootloader.bin`, `partition-table.bin`, `firmware.bin` (the app, renamed from `esp32s3_a10_camera.bin`), `spiffs.bin`, `flash_all.sh`/`.bat`, checksums.

## Do NOT

- Enable PSRAM (CONFIG_SPIRAM) — boot loop; timing tuning unresolved.
- Switch to ESP-IDF v6.0.x — PSRAM/board issues per README.
- Copy pin tables / partition layouts / PSRAM config from the sister S3 repos.
- Raise `fb_count` above 1 without PSRAM.
- Re-enable WPA3/AMPDU without re-validating WiFi stability on this board.
- Commit `sdkconfig`, `managed_components/`, or `build/`.
- Assume the LuatOS docs pinout — use `CAMERA_MODEL_Air_ESP32S3`.

## 2026-09-04 上午：MJPEG 客户端任务泄漏修复（PIT-024）
- **症状链**：半开连接上 `send()` 永久阻塞 → 客户端任务/栈/槽位泄漏
  （17h 漏 24 个 ≈96KB，Min Heap 一度 100B）→ httpd 饿死 → 自愈误杀重启。
- **修复（已上板验证：堆 40-47KB 稳定 15min+，Min Heap 9108，低堆告警绝迹）**：
  流循环头部移植 seeed 的死客户端探测（`recv(MSG_DONTWAIT)` 探 FIN/RST 即退），
  accept 后 `SO_SNDTIMEO=10s` 兜底零窗口。
- 教训：单仓修的家族缺陷必须当天评估姐妹仓同函数形态——seeed 有探测本仓没有，正是坑因。
- 统一 logo favicon.svg（四仓同 md5）；web_ui 新增文件需 `idf.py reconfigure`（PIT-026）。
