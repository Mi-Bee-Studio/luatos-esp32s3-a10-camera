/*
 * camera_driver.h - OV2640 camera driver for ESP32-S3-A10
 *
 * Provides camera initialization, capture, and deinitialization
 * for the 8225N (OV2640) camera module on the ESP32-S3-A10 board.
 */

#ifndef CAMERA_DRIVER_H
#define CAMERA_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_camera.h"

/**
 * Supported camera resolutions
 */
typedef enum {
    CAMERA_RES_VGA = 0,     /**< 640x480 */
    CAMERA_RES_SVGA = 1,    /**< 800x600 */
    CAMERA_RES_XGA = 2,     /**< 1024x768 */
    CAMERA_RES_UXGA = 3,    /**< 1600x1200 */
    CAMERA_RES_MAX
} camera_resolution_t;

/* JPEG quality bounds (lower = better quality / larger frames). The driver
 * sizes JPEG frame buffers at width*height/5; quality < 10 regularly exceeds
 * that budget (VGA fb = 61KB) and produces truncated frames. With no PSRAM
 * (DRAM-only, fb_count=1) oversized frames also pressure the heap spiral
 * documented in AGENTS.md. Measured 2026-09-04: VGA q10 = ~13KB typical. */
#define CAMERA_QUALITY_MIN 10
#define CAMERA_QUALITY_MAX 63

/* ── 分辨率三层上限（2026-09-04 家族统一，PIT-021 附录）─────────────
 * effective = min(传感器上限, 板级实测上限, 运行时 fb 预算)：
 *  1. sensor  — esp32-camera 自动检测（camera_sensor_info_t.max_size，
 *               OV2640→UXGA），换传感器候选表自适应收缩；
 *  2. board   — 本板实测常数（唯一手工数字，禁止沿用姐妹板数值）：
 *               2026-09-03 实测 SVGA 使 cam_hal DMA 池 61KB→96KB，
 *               空闲堆 45K→16K，暗光大帧再挤压 → 每秒分配失败、httpd
 *               拒连、流 16s 断连螺旋（PIT-012）——"能出图"≠"能稳定
 *               带流"，本板（DRAM-only, fb_count=1）稳定档位只有 VGA；
 *  3. memory  — 运行时 fb 预算校验（宽*高/5*fb_count + floor ≤ 可用
 *               内部 DRAM），只能收紧；本板稳态下该层独立复算出同样
 *               的 VGA 上限——若未来出现内存更宽裕的板型（如启用
 *               PSRAM），此层自动放宽候选表，届时须按家族流程重新
 *               实测板级常数再放宽第 2 层。
 * GET /api/camera 下发 res_cap_source 报告被哪一层钳制（诊断用）。 */
#define CAMERA_RES_BOARD_MAX CAMERA_RES_VGA

/**
 * @brief 当前实际可用最大分辨率 min(sensor, board, memory)
 */
camera_resolution_t camera_get_effective_max_res(void);

/**
 * @brief 上限被哪一层钳制（sensor / board / memory），静态字符串
 */
const char *camera_res_cap_source(void);

/**
 * Initialize the OV2640 camera with the given parameters.
 *
 * @param resolution  Desired resolution (default: CAMERA_RES_VGA)
 * @param fps         Desired frame rate (default: 15)
 * @param jpeg_quality JPEG quality 0-63, lower = better (default: 12)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t camera_init(camera_resolution_t resolution, uint8_t fps, uint8_t jpeg_quality);

/**
 * Deinitialize the camera and release all resources.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t camera_deinit(void);

/**
 * Capture a single frame from the camera.
 *
 * @param fb  Output pointer to the frame buffer. Caller must call
 *            camera_return_fb() when done with the buffer.
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED if not initialized,
 *         or other error codes from the camera driver
 */
esp_err_t camera_capture(camera_fb_t **fb);

/**
 * Return a previously captured frame buffer to the driver for reuse.
 *
 * @param fb  Frame buffer to return
 * @return ESP_OK on success
 */
esp_err_t camera_return_fb(camera_fb_t *fb);

/**
 * Check if the camera has been initialized.
 *
 * @return true if initialized, false otherwise
 */
bool camera_is_initialized(void);

/**
 * Get the human-readable name of the camera sensor.
 *
 * @return Sensor name string (e.g. "OV2640"), or "Unknown" if not initialized
 */
const char* camera_get_sensor_name(void);

/**
 * Get the current resolution setting.
 *
 * @return Current resolution enum value
 */
camera_resolution_t camera_get_resolution(void);

#endif /* CAMERA_DRIVER_H */
