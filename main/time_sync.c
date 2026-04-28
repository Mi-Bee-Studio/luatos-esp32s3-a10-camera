#include "time_sync.h"
#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <esp_log.h>

static const char *TAG = "time_sync";

esp_err_t time_sync_init(const char *timezone) {
    if (!timezone) {
        ESP_LOGE(TAG, "Timezone cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Set timezone
    setenv("TZ", timezone, 1);
    tzset();

    // Configure SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP initialized, timezone: %s", timezone);
    return ESP_OK;
}

bool time_is_synced(void) {
    time_t now = time(NULL);
    return now > 1700000000; // After Jan 2024 threshold
}

void time_get_str(char *buf, size_t len) {
    if (!buf || len < 20) {
        return;
    }

    time_t now = time(NULL);
    struct tm *timeinfo = localtime(&now);
    
    if (timeinfo) {
        strftime(buf, len, "%Y-%m-%d %H:%M:%S", timeinfo);
    } else {
        strncpy(buf, "Invalid time", len);
    }
}

esp_err_t time_set_manual(int year, int month, int day, int hour, int min, int sec) {
    struct timeval tv = {0};
    struct tm tm = {0};
    
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    tm.tm_isdst = -1; // Let system determine DST
    
    if (mktime(&tm) == (time_t)-1) {
        ESP_LOGE(TAG, "Invalid time parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    tv.tv_sec = mktime(&tm);
    tv.tv_usec = 0;
    
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE(TAG, "Failed to set system time");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Manual time set: %04d-%02d-%02d %02d:%02d:%02d", 
             year, month, day, hour, min, sec);
    return ESP_OK;
}