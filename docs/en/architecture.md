# Architecture

MiBeeCam firmware for the **LuatOS ESP32-S3-A10** board (OV2640 / 8225N module).
This is the budget variant of the MiBee Cam family: **no PSRAM, a single factory
partition (no OTA), ESP-IDF v5.5.4**. Those constraints shape everything below —
see "Board constraints" before proposing changes.

Sister repos: `ai-thinker-esp32-cam`, `esp32s3-n16r8-cam`, `seeed-esp32s3-cam`.

## Module Map

| Module | Files | Purpose | Dependencies |
|--------|-------|---------|--------------|
| main | main.c | App entry, 15-step boot sequence, AP/STA mode select | All modules |
| config_manager | config_manager.c/h | Per-key NVS config (`mibee_cfg` + `schema_ver`), one-shot legacy blob migration, SD provisioning override | nvs_flash |
| camera_driver | camera_driver.c/h | OV2640 init from config, resolution/quality bounds (`camera_get_effective_max_res()` = min(sensor, board, memory), board cap = VGA), reboot-to-apply for camera keys | esp_camera, config_manager |
| frame_broadcaster | frame_broadcaster.c/h | DRAM frame cache with reference counting; sample-and-release pattern (fb_count=1 → effectively single consumer per frame) | esp_camera |
| mjpeg_streamer | mjpeg_streamer.c/h | Independent TCP server on port 81, multipart/x-mixed-replace; MAX_STREAM_CLIENTS=1 with LRU kick + hammering backoff guard | frame_broadcaster |
| web_server | web_server.c/h | Port 80: REST API (see `s_uris[]` route table at top of file), SPIFFS static serving, WebSocket `/ws` | esp_http_server, config_manager, event_bus |
| ws clients | (inside web_server.c) | WebSocket push: motion / WiFi / health / upload events via event_bus subscriptions | event_bus |
| motion_detect | motion_detect.c/h | Frame-difference motion detection, auto JPEG upload on trigger | frame_broadcaster, config_manager |
| onvif_service + onvif_discovery | onvif_service.c/h, onvif_discovery.c/h | ONVIF Profile S: WS-Discovery + SOAP (registers SOAP handlers into the port-80 httpd) | mdns, web_server |
| wifi_manager | wifi_manager.c/h | STA/AP, dual-network boot-time RSSI pick + failover, backup SSID, HT20 forced, AMPDU off (board-specific stability) | esp_wifi |
| wifi_channel_health | wifi_channel_health.c/h | Channel busy-score / BSS / RSSI snapshot for `/api/status` | esp_wifi |
| event_bus | event_bus.c/h | In-memory pub/sub, 16 subscriptions, 9+ event types | — |
| health_monitor | health_monitor.c/h | Prometheus `/metrics`, heap watchdog with probe-failure classification (network-down never counted as hang) | wifi_manager |
| time_sync | time_sync.c/h | SNTP via pool.ntp.org | esp_netif |
| webhook | webhook.c/h | HTTP event forwarding (conditional on config) | event_bus |
| at_command + at_port | at_command.c/h, at_port.c/h | Family AT console on UART0 (`docs/at-command.md`) + board port (CH343 `/dev/ttyACM1`) | config_manager |
| status_led | status_led.c/h | GPIO 10 status LED states | — |
| device_id | device_id.c/h | Stable device identity (MAC-derived) | esp_efuse |
| csi_motion | csi_motion.cpp/h (optional) | ESPectre WiFi CSI sensing, compile-time gated (`CONFIG_MIBEE_CSI_MOTION`, default off; sensing and streaming are mutually exclusive on this PSRAM-less board) | components/espectre |
| cJSON | cJSON.c/h | Vendored JSON (do not edit) | — |

## Boot Sequence

From `main.c` `app_main()`:

1. **NVS init** (erase + retry on corruption)
2. **Config load** (per-key NVS, legacy migration)
3. **Status LED init**, then **event_bus init** (before any publisher/subscriber)
4. **SPIFFS mount** (web UI)
5. **Camera init — BEFORE WiFi** (SCCB/I2C bus conflict if WiFi grabs it first; opposite order from ai-thinker, which defers camera for a different DMA bug — do not unify)
6. **Frame broadcaster init**
7. **WiFi init** (netif + event loop); optional CSI sensing (gated, off by default)
8. **Health monitor init**
9. **Mode select**: stored WiFi credentials?
   - **STA**: dual-network boot pick → connect. MJPEG streamer init here; the
     remaining services (time sync, web server, motion detect, webhook, ONVIF)
     start from the `wifi_state_cb` on `WIFI_STATE_STA_CONNECTED` — MJPEG server
     start, motion detect (gated by `motion_enabled` and a 30KB heap floor),
     webhook (if configured), ONVIF discovery + SOAP (if `onvif_enable`).
   - **AP** (first boot / no credentials): open `MiBeeCam` AP
     (`http://192.168.4.1`), web server + MJPEG + ONVIF only.
10. **BOOT button factory-reset monitor** (hold GPIO0 5s → erase NVS → AP mode)
11. **AT command interface** on UART0

## Data Flow

```
OV2640 ──esp_camera fb──▶ frame_broadcaster (DRAM cache, ref-counted)
                              │ sample-and-release (fb_count=1)
                              ├─▶ mjpeg_streamer (:81, single client)
                              ├─▶ motion_detect ──event_bus──▶ ws push / webhook / upload
                              └─▶ web_server /api/capture (single JPEG)

event_bus: MOTION_DETECTED/END, WIFI_STATE_CHANGED, WIFI_SWITCHED_SSID,
           STREAM_CLIENT_CONNECTED/DISCONNECTED, HEALTH_WARNING,
           UPLOAD_SUCCESS/FAILED   → ws_server push, webhook
```

## Board Constraints (why things look unusual)

- **No PSRAM** (`CONFIG_SPIRAM` unset — Octal timing unsolved on this board,
  enabling it boot-loops). All frame buffers in internal DRAM, `fb_count=1`:
  streaming, motion detection and capture contend for one frame → the
  broadcaster's sample-and-release pattern and the single-stream cap
  (`MAX_STREAM_CLIENTS=1`) are load-bearing, not cosmetic.
- **Resolution cap = VGA** (board layer of the three-layer cap; SVGA+ spirals
  into heap exhaustion, see family PIT-021). Camera key changes apply via
  save + reboot, not hot reinit (single-FB reinit race bricks the board).
- **Single factory partition** — no OTA endpoint by design; delivery is USB
  (`/dev/ttyACM1`, CH343: opening the port resets the board).
- **ESP-IDF v5.5.4 pinned**; **WPA3 and AMPDU disabled** (board-specific
  stability verdicts — do not re-enable without re-validating).
- **lwIP budget**: `LWIP_MAX_SOCKETS=16`, `TCP_MSL=15000` — family EMFILE
  recipe; the httpd `max_open_sockets=10` leaves room for :81 and the
  webhook/uploader.

## Where to Look

- HTTP endpoints: `s_uris[]` route table at the top of `main/web_server.c`
- API behavior contracts (family-wide): `docs/api-contract.md`,
  `docs/config-contract.md`, `docs/at-command.md`
- Web UI: SPIFFS-hosted family SPA (`main/web_ui/`, 4 files)
- Build/flash: `README.md`
