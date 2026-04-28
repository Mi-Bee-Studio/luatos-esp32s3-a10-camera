#pragma once

#include "esp_err.h"

float get_chip_temp(void);
esp_err_t health_monitor_init(void);
esp_err_t health_monitor_deinit(void);