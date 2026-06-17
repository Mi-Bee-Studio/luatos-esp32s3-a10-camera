#pragma once

#include "esp_err.h"
#include <stdbool.h>

float get_chip_temp(void);
esp_err_t health_monitor_init(void);
esp_err_t health_monitor_deinit(void);

/**
 * @brief Get heap baseline values recorded at initialization
 * @param free_heap_out Output: free heap at init (bytes)
 * @param min_heap_out Output: minimum free heap at init (bytes)
 */
void health_get_baselines(size_t *free_heap_out, size_t *min_heap_out);

/**
 * @brief Check current heap against warning threshold
 * @param free_heap_out Output: current free heap (bytes)
 * @param warning_out Output: true if free heap < 30KB threshold
 */
void health_check_threshold(size_t *free_heap_out, bool *warning_out);