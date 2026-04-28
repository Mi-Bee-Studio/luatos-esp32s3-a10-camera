#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t time_sync_init(const char *timezone);
bool time_is_synced(void);
void time_get_str(char *buf, size_t len);
esp_err_t time_set_manual(int year, int month, int day, int hour, int min, int sec);