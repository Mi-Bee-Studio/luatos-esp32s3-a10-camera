# AGENTS.md — ESP32-S3-A10 CAM

MiBeeCam firmware for the **LuatOS ESP32-S3-A10** board (OV2640 / 8225N module). C, ESP-IDF. Web UI + MJPEG streamer + motion detection + ONVIF Profile S, all in a single ESP32-S3 image.

> **Sister projects**: `ai-thinker-esp32-cam`, `seeed-esp32s3-cam`, `esp32s3-n16r8-cam` (same Mi-Bee-Studio family). This is the **budget single-OTA variant** — do NOT copy their PSRAM config, ESP-IDF version, partition tables, or pin maps. See "What makes this board different" below.

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
├── config_manager.*    # NVS-backed, v1→v2 auto-migration
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
- **AMPDU TX/RX disabled** (`CONFIG_ESP_WIFI_AMPDU_*_ENABLED=n`) — mirrors the working `seeed-esp32s3-cam` config.
- Default AP on first boot: SSID `MiBeeCam`, password `12345678`, config at `http://192.168.4.1`.

## Frame-buffer constraint (fb_count=1)

With PSRAM off and only one DRAM framebuffer, motion detection and streaming contend for the same frame. Code uses a **sample-and-release pattern** in `frame_broadcaster` + pause-during-stream in motion detect to avoid contention. Raising `fb_count` requires PSRAM, which is disabled — don't try.

## Factory reset

Hold **BOOT** (GPIO 0) for 5 s → clears NVS, reboots into AP mode. (On `ai-thinker-esp32-cam`, GPIO0 is camera XCLK and unusable — different board, different choice.)

## Conventions

- `snake_case` functions/vars, `s_` static prefix, `ESP_LOGx(TAG, ...)`, `esp_err_t` returns.
- `sdkconfig.defaults` is the single source of truth for board config; pin numbers live in `camera_driver.c` via `CAMERA_MODEL_Air_ESP32S3` (the `esp32-camera` Air ESP32-S3 pin def — **not** the LuatOS documentation pinout).
- Config persisted in NVS namespace `device_cfg`, struct versioned with v1→v2 migration in `config_manager.c`.

## REST API Endpoints

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
