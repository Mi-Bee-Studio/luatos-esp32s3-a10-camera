/**
 * @file at_command.h
 * @brief AT command interface for MiBeeCam ESP32-S3-A10.
 *
 * Provides a serial AT command interface over UART0 for device
 * configuration, query, and control. Supports 18 standard AT commands
 * including WiFi management, config get/set, device info, and streaming status.
 *
 * Architecture:
 *   - A dedicated FreeRTOS task reads lines from stdin (UART0 via VFS)
 *   - Lines starting with "AT" are parsed and dispatched to command handlers
 *   - Responses are printed via printf to stdout
 *   - Log output (ESP_LOG*) is also on UART0; the AT task ignores non-AT lines
 *
 * @note This module does NOT authenticate or authorize — it operates on
 *       the physical serial port and assumes trusted access.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the AT command interface.
 *
 * Installs the UART0 driver for VFS-backed stdin/stdout (if not already
 * installed by the console subsystem), registers the VFS UART handler,
 * and creates the AT command parsing task.
 *
 * The AT command task:
 *   - Stack: 4096 bytes
 *   - Priority: 5
 *   - Reads one line at a time via fgets(stdin, ...)
 *   - Dispatches matching commands to their handlers
 *   - Ignores non-AT lines (log output, garbage)
 *
 * @return ESP_OK on success
 * @return ESP_ERR_NO_MEM if task creation fails
 */
esp_err_t at_command_init(void);

/**
 * @brief Deinitialize the AT command interface.
 *
 * Signals the AT command task to stop and clean up.
 * Currently a no-op stub — the AT task runs until the device reboots.
 *
 * @return ESP_OK always
 */
esp_err_t at_command_deinit(void);

#ifdef __cplusplus
}
#endif
