/*
 * frame_broadcaster.h - Single-consumer DRAM frame cache with reference counting
 *
 * ARCHITECTURE NOTE: This is the PSRAM-disabled, single-consumer DRAM version.
 * The API is designed to allow future PSRAM-enabled multi-consumer expansion
 * without changing caller code. Currently only caches the LATEST frame.
 *
 * Memory: Uses DRAM only (PSRAM is disabled on this board). MAX_FRAME_SIZE
 * is statically allocated to avoid runtime fragmentation.
 *
 * Concurrency: Publisher (camera capture) and consumer (motion/stream) run in
 * different tasks. Mutex protects metadata; the frame buffer itself is
 * protected by reference counting.
 */
#ifndef FRAME_BROADCASTER_H
#define FRAME_BROADCASTER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Frame reference — consumers receive this pointer and must release it
 */
typedef struct {
    uint8_t *buf;           /**< Pointer to JPEG data (valid until fbroadcast_release) */
    size_t len;             /**< Frame length in bytes */
    uint32_t ref_count;     /**< Reference count (internal) */
    uint64_t timestamp_us;  /**< Frame timestamp (microseconds since boot) */
} frame_ref_t;

/**
 * @brief Initialize the frame broadcaster
 * @return ESP_OK on success, ESP_ERR_NO_MEM if allocation fails
 */
esp_err_t fbroadcast_init(void);

/**
 * @brief Publish a new frame (producer side)
 *        If the previous frame is still held (ref_count > 0), the new frame is DROPPED.
 *        This never blocks the producer.
 * @param jpeg_buf  Pointer to JPEG data (will be COPIED to internal buffer)
 * @param len       Frame length in bytes (must be <= MAX_FRAME_SIZE)
 * @param ts        Timestamp in microseconds
 * @return ESP_OK on success, ESP_ERR_INVALID_SIZE if frame too large (dropped)
 */
esp_err_t fbroadcast_publish(const uint8_t *jpeg_buf, size_t len, uint64_t ts);

/**
 * @brief Acquire the latest frame (consumer side)
 *        Increases ref_count. Consumer must call fbroadcast_release() when done.
 * @param out Output pointer to frame_ref_t (valid until release)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no frame available yet
 */
esp_err_t fbroadcast_acquire_latest(frame_ref_t **out);

/**
 * @brief Release a previously acquired frame
 *        Decreases ref_count. When ref_count reaches 0, the buffer can be overwritten.
 * @param frame Pointer returned by fbroadcast_acquire_latest()
 */
void fbroadcast_release(frame_ref_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* FRAME_BROADCASTER_H */
