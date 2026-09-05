/*
 * camera_driver.c - OV2640 camera driver for ESP32-S3-A10
 *
 * Implements camera initialization, frame capture, and resource management
 * for the 8225N (OV2640) camera module on the ESP32-S3-A10 board.
 * Uses the esp32-camera component for hardware abstraction.
 */

#include "camera_driver.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "camera_driver";

/* ── ESP32-S3-A10 (8225N module) pin mapping ── */
#define CAM_PIN_PWDN    (-1)
#define CAM_PIN_RESET   (-1)
#define CAM_PIN_XCLK    39
#define CAM_PIN_SIOD    21   /* I2C data  */
#define CAM_PIN_SIOC    46   /* I2C clock */
#define CAM_PIN_D0      34
#define CAM_PIN_D1      47
#define CAM_PIN_D2      48
#define CAM_PIN_D3      33
#define CAM_PIN_D4      35
#define CAM_PIN_D5      37
#define CAM_PIN_D6      38
#define CAM_PIN_D7      40
#define CAM_PIN_VSYNC    42
#define CAM_PIN_HREF     41
#define CAM_PIN_PCLK    36

/* ── Defaults ── */
#define DEFAULT_RESOLUTION   CAMERA_RES_VGA
#define DEFAULT_FPS          15
#define DEFAULT_JPEG_QUALITY 12
#define DEFAULT_FB_COUNT     1

/* ── Module state ── */
static bool s_camera_initialized = false;
static camera_resolution_t s_current_resolution = CAMERA_RES_VGA;
static SemaphoreHandle_t s_camera_mutex = NULL;
static const char *s_cap_source = "board";   /* 最近一次 effective 计算的钳制层 */

/* ── Helpers ── */

/* 家族统一刻度（契约 v1.3 §5）：camera_resolution_t 的值即 framesize_t 枚举 */
static framesize_t resolution_to_framesize(camera_resolution_t res)
{
    if ((int)res >= FRAMESIZE_VGA && (int)res <= FRAMESIZE_UXGA) {
        return (framesize_t)res;
    }
    return FRAMESIZE_VGA;
}

static const char* resolution_to_string(camera_resolution_t res)
{
    switch (res) {
        case CAMERA_RES_VGA:  return "VGA";
        case CAMERA_RES_SVGA: return "SVGA";
        case CAMERA_RES_XGA:  return "XGA";
        case CAMERA_RES_HD:   return "HD";
        case CAMERA_RES_SXGA: return "SXGA";
        case CAMERA_RES_UXGA: return "UXGA";
        default:              return "Unknown";
    }
}

/* ── 三层分辨率上限（sensor ∩ board ∩ memory，PIT-021 附录）─────────
 * memory 层（本板是 DRAM 板，这层是关键防线）：esp32-camera 的 JPEG fb
 * 按 宽*高/5 分配（cam_hal 同式），DRAM-only + fb_count=1 时预算 =
 * fb_size，换档后须仍留 floor 给 WiFi/httpd/流任务。floor 校准依据
 * （2026-09-03 实测）：VGA 单流稳态空闲 40-47K（流活跃可低至 21K）、
 * health 告警线 15K；SVGA（fb 96K vs VGA 61K）即触发 PIT-012 堆枯竭
 * 螺旋。稳态下本层独立复算出与板级常数一致的 VGA 上限。 */
#define CAMERA_RES_MEM_FLOOR (32 * 1024)

/* framesize → 尺寸表，下标 0 对应 FRAMESIZE_VGA（钉死 esp32-camera 2.1.x
 * 枚举序；组件枚举漂移时由 _Static_assert 在构建期暴露） */
static const struct { uint16_t w, h; } s_fs_dims[] = {
    { 640,  480},  { 800,  600},  {1024,  768},  {1280,  720},  {1280, 1024},
    {1600, 1200},  {1920, 1080},  { 720, 1280},  { 864, 1536},  {2048, 1536},
    {2560, 1440},  {2560, 1600},  {1080, 1920},  {2560, 1920},  {2592, 1944},
};
_Static_assert(FRAMESIZE_VGA == 10, "s_fs_dims pinned to esp32-camera 2.1.x enum");

static size_t fb_bytes_for_res(camera_resolution_t res)
{
    int idx = (int)res - (int)FRAMESIZE_VGA;
    if (idx < 0 || (size_t)idx >= sizeof(s_fs_dims) / sizeof(s_fs_dims[0])) {
        return 0;
    }
    return (size_t)s_fs_dims[idx].w * s_fs_dims[idx].h / 5;
}

static bool fb_budget_ok(camera_resolution_t res)
{
    size_t need = fb_bytes_for_res(res) * DEFAULT_FB_COUNT;
    size_t cur_fb = s_camera_initialized
        ? fb_bytes_for_res(s_current_resolution) * DEFAULT_FB_COUNT : 0;
    /* fb_location=CAMERA_FB_IN_DRAM → fb 来自内部 DMA 域 */
    size_t avail = heap_caps_get_free_size(MALLOC_CAP_DMA) + cur_fb;
    return need != 0 && avail >= need + CAMERA_RES_MEM_FLOOR;
}

/** sensor 层：查 esp32-camera 组件自带能力表（单一事实源，勿手抄 PID 表）。
 *  未初始化/未知 PID → 回退板级常数（不放宽）。统一刻度下值域同 framesize_t。 */
static camera_resolution_t sensor_max_resolution(void)
{
    if (s_camera_initialized) {
        sensor_t *s = esp_camera_sensor_get();
        camera_sensor_info_t *info = s ? esp_camera_sensor_get_info(&s->id) : NULL;
        if (info && (int)info->max_size >= (int)FRAMESIZE_VGA) {
            int res = (int)info->max_size;
            if (res > (int)CAMERA_RES_UXGA) {
                res = (int)CAMERA_RES_UXGA;   /* 本板枚举天花板 */
            }
            return (camera_resolution_t)res;
        }
        ESP_LOGW(TAG, "Unknown sensor — sensor layer falls back to board max");
    }
    return CAMERA_RES_BOARD_MAX;
}

camera_resolution_t camera_get_effective_max_res(void)
{
    camera_resolution_t sensor_cap = sensor_max_resolution();
    camera_resolution_t board_cap  = CAMERA_RES_BOARD_MAX;
    camera_resolution_t cap = (sensor_cap < board_cap) ? sensor_cap : board_cap;
    while (cap > CAMERA_RES_VGA && !fb_budget_ok(cap)) {
        cap = (camera_resolution_t)((int)cap - 1);
    }
    if (cap == sensor_cap) {
        s_cap_source = "sensor";
    } else if (cap == board_cap) {
        s_cap_source = "board";
    } else {
        s_cap_source = "memory";
    }
    return cap;
}

const char *camera_res_cap_source(void)
{
    return s_cap_source;
}


/* ── Public API ── */

esp_err_t camera_init(camera_resolution_t resolution, uint8_t fps, uint8_t jpeg_quality)
{
    if (s_camera_initialized) {
        ESP_LOGW(TAG, "Camera already initialized, deinitializing first");
        esp_err_t ret = camera_deinit();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to deinitialize existing camera: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    /* Validate parameters（家族刻度 10-15；越界回退 VGA）*/
    if (resolution < CAMERA_RES_VGA || resolution > CAMERA_RES_UXGA) {
        ESP_LOGW(TAG, "Invalid resolution %d, defaulting to VGA", resolution);
        resolution = DEFAULT_RESOLUTION;
    }
    if (fps == 0) {
        ESP_LOGW(TAG, "Invalid fps 0, defaulting to %d", DEFAULT_FPS);
        fps = DEFAULT_FPS;
    }
    if (jpeg_quality < CAMERA_QUALITY_MIN || jpeg_quality > CAMERA_QUALITY_MAX) {
        ESP_LOGW(TAG, "JPEG quality %d out of range [%d-%d], clamping",
                 jpeg_quality, CAMERA_QUALITY_MIN, CAMERA_QUALITY_MAX);
        jpeg_quality = (jpeg_quality < CAMERA_QUALITY_MIN) ? CAMERA_QUALITY_MIN : CAMERA_QUALITY_MAX;
    }

    /* XCLK 频率：契约核心字段 xclk_freq_mhz（{10,16,20}，本板值 20）。
     * 旧实现按 fps 启发式取 10/20MHz，契约化后改为读配置（运行时可调）。 */
    uint32_t xclk_freq_hz = (uint32_t)config_get()->xclk_freq_mhz * 1000000;
    if (xclk_freq_hz != 10000000 && xclk_freq_hz != 16000000 && xclk_freq_hz != 20000000) {
        xclk_freq_hz = 20000000;   /* 越界安全默认 */
    }

    camera_config_t config = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d0       = CAM_PIN_D0,
        .pin_d1       = CAM_PIN_D1,
        .pin_d2       = CAM_PIN_D2,
        .pin_d3       = CAM_PIN_D3,
        .pin_d4       = CAM_PIN_D4,
        .pin_d5       = CAM_PIN_D5,
        .pin_d6       = CAM_PIN_D6,
        .pin_d7       = CAM_PIN_D7,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,

        .xclk_freq_hz = xclk_freq_hz,

        /* Frame buffer: use PSRAM if available, otherwise DRAM */
        .fb_location  = CAMERA_FB_IN_DRAM,

        /* JPEG output format */
        .pixel_format = PIXFORMAT_JPEG,

        .frame_size   = resolution_to_framesize(resolution),
        .jpeg_quality = jpeg_quality,
        .fb_count     = DEFAULT_FB_COUNT,

        /* When no frame buffer is available, wait for the driver to return one */
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
    };

    ESP_LOGI(TAG, "Initializing camera: %s, %d fps, quality %d",
             resolution_to_string(resolution), fps, jpeg_quality);

    /* Pre-scan I2C bus to verify camera hardware */
    {
        i2c_master_bus_handle_t bus_handle = NULL;
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = CAM_PIN_SIOD,
            .scl_io_num = CAM_PIN_SIOC,
            .clk_source = I2C_CLK_SRC_DEFAULT,
        };
        esp_err_t bus_ret = i2c_new_master_bus(&bus_config, &bus_handle);
        if (bus_ret == ESP_OK) {
            ESP_LOGI(TAG, "I2C bus scan:");
            for (int addr = 0x08; addr < 0x78; addr++) {
                if (i2c_master_probe(bus_handle, addr, 50) == ESP_OK) {
                    ESP_LOGI(TAG, "  Found I2C device at 0x%02x", addr);
                }
            }
            i2c_del_master_bus(bus_handle);
        } else {
            ESP_LOGE(TAG, "Failed to init I2C bus for scan: %s", esp_err_to_name(bus_ret));
        }
    }

    esp_err_t ret = esp_camera_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s (0x%x)", esp_err_to_name(ret), ret);
        return ret;
    }

    /* Retrieve sensor info */
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        /* 三层上限统一钳制（sensor ∩ board ∩ memory）。本板分辨率变更
         * 走保存+重启（2026-09-03 热重配竞态封禁），此处钳的是 NVS 残留；
         * OV2640 运行时 set_framesize 本身有效。 */
        if (resolution > camera_get_effective_max_res()) {
            ESP_LOGW(TAG, "Requested res %s exceeds effective max %s (source: %s), clamping",
                     resolution_to_string(resolution),
                     resolution_to_string(camera_get_effective_max_res()),
                     camera_res_cap_source());
            resolution = camera_get_effective_max_res();
            if (sensor->set_framesize) {
                sensor->set_framesize(sensor, resolution_to_framesize(resolution));
            }
        }
        ESP_LOGI(TAG, "Camera: %s @ %s", camera_get_sensor_name(),
                 resolution_to_string(resolution));

        /* Configure sensor for desired frame rate */
        if (sensor->set_pixformat) {
            sensor->set_pixformat(sensor, PIXFORMAT_JPEG);
        }

        /* 契约核心字段 cam_vflip / cam_hmirror（默认 0 = 传感器复位默认，
         * 行为不变；变更走保存+重启，在 init 时应用）*/
        if (sensor->set_vflip) {
            sensor->set_vflip(sensor, config_get()->cam_vflip);
        }
        if (sensor->set_hmirror) {
            sensor->set_hmirror(sensor, config_get()->cam_hmirror);
        }
    } else {
        ESP_LOGW(TAG, "Camera sensor info unavailable after init");
    }

    s_camera_initialized = true;
    s_camera_mutex = xSemaphoreCreateMutex();
    s_current_resolution = resolution;

    ESP_LOGI(TAG, "Camera initialized successfully");
    return ESP_OK;
}

esp_err_t camera_deinit(void)
{
    if (!s_camera_initialized) {
        ESP_LOGW(TAG, "Camera not initialized, nothing to deinitialize");
        return ESP_OK;
    }

    esp_err_t ret = esp_camera_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera deinit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_camera_initialized = false;
    if (s_camera_mutex) {
        vSemaphoreDelete(s_camera_mutex);
        s_camera_mutex = NULL;
    }
    ESP_LOGI(TAG, "Camera deinitialized");
    return ESP_OK;
}

esp_err_t camera_capture(camera_fb_t **fb)
{
    if (!s_camera_initialized) {
        ESP_LOGE(TAG, "Camera not initialized");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (fb == NULL) {
        ESP_LOGE(TAG, "Null frame buffer pointer");
        return ESP_ERR_INVALID_ARG;
    }

    /* Acquire camera mutex before accessing framebuffer */
    if (s_camera_mutex == NULL || xSemaphoreTake(s_camera_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire camera mutex");
        return ESP_FAIL;
    }

    *fb = esp_camera_fb_get();
    if (*fb == NULL) {
        xSemaphoreGive(s_camera_mutex);
        ESP_LOGE(TAG, "Failed to capture frame");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t camera_return_fb(camera_fb_t *fb)
{
    if (fb == NULL) {
        ESP_LOGW(TAG, "Attempted to return NULL frame buffer");
        return ESP_ERR_INVALID_ARG;
    }

    esp_camera_fb_return(fb);
    if (s_camera_mutex) {
        xSemaphoreGive(s_camera_mutex);
    }
    return ESP_OK;
}

bool camera_is_initialized(void)
{
    return s_camera_initialized;
}

const char* camera_get_sensor_name(void)
{
    if (!s_camera_initialized) {
        return "Unknown";
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        return "Unknown";
    }

    /* 2026-09-04：改查组件能力表取名（换接其他传感器也能正确上报） */
    camera_sensor_info_t *info = esp_camera_sensor_get_info(&sensor->id);
    return info ? info->name : "Unknown";
}

camera_resolution_t camera_get_resolution(void)
{
    return s_current_resolution;
}
