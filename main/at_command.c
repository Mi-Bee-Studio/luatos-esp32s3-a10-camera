/**
 * @file at_command.c
 * @brief AT command interface implementation for MiBeeCam ESP32-S3-A10.
 *
 * Implements 18 AT commands for serial-port device control:
 *   AT, AT+RST, AT+GMR, AT+RESTORE, AT+HEAP, AT+UPTIME, AT+TEMP,
 *   AT+CWJAP?, AT+CWJAP=, AT+CWQAP, AT+CIFSR, AT+CWLAP,
 *   AT+NAME?, AT+NAME=, AT+CFGGET=, AT+CFGSET=, AT+SAVE, AT+STREAM?
 *
 * Architecture:
 *   - UART0 driver installed (or reused from console) for stdin/stdout
 *   - Dedicated FreeRTOS task reads lines and dispatches to handlers
 *   - Command table: static array of {cmd_string, handler_fn} entries
 *   - Response helpers: at_ok(), at_error(), at_error_msg(), at_send()
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_chip_info.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "driver/uart_vfs.h"

#include "config_manager.h"
#include "wifi_manager.h"
#include "health_monitor.h"
#include "mjpeg_streamer.h"
#include "motion_detect.h"
#include "at_command.h"

/* ---------------------------------------------------------------------------
 * Module constants
 * -------------------------------------------------------------------------*/
static const char *TAG = "at_cmd";

#define AT_LINE_MAX        256
#define AT_CMD_BUF_MAX     64
#define AT_FIELD_MAX       64
#define AT_VALUE_MAX       128

/* WiFi scan max APs */
#define AT_SCAN_MAX_APS    15

/* ---------------------------------------------------------------------------
 * Module-global state
 * -------------------------------------------------------------------------*/
static volatile bool s_running = false;

/* ---------------------------------------------------------------------------
 * Response helpers
 * -------------------------------------------------------------------------*/

/** Print \r\nOK\r\n */
static void at_ok(void)
{
    printf("\r\nOK\r\n");
}

/** Print \r\nERROR\r\n */
static void at_error(void)
{
    printf("\r\nERROR\r\n");
}

/** Print \r\nERROR: <msg>\r\n */
static void at_error_msg(const char *msg)
{
    printf("\r\nERROR: %s\r\n", msg ? msg : "unknown");
}

/**
 * @brief Print a formatted response line.
 * Output: \r\n<fmt...>\r\n
 */
static void at_send(const char *fmt, ...)
{
    va_list args;
    printf("\r\n");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\r\n");
}

/* ---------------------------------------------------------------------------
 * Command handlers — one per AT command
 * -------------------------------------------------------------------------*/

/**
 * Handler for: AT  (test command, no params)
 */
static void handle_at(const char *params)
{
    (void)params;
    at_ok();
}

/**
 * Handler for: AT+RST  (reboot device)
 */
static void handle_rst(const char *params)
{
    (void)params;
    ESP_LOGI(TAG, "AT+RST: rebooting");
    at_ok();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

/**
 * Handler for: AT+GMR  (firmware version info)
 */
static void handle_gmr(const char *params)
{
    (void)params;
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    const char *chip_model_str;
    switch (chip_info.model) {
    case CHIP_ESP32:    chip_model_str = "ESP32";    break;
    case CHIP_ESP32S2:  chip_model_str = "ESP32-S2";  break;
    case CHIP_ESP32S3:  chip_model_str = "ESP32-S3";  break;
    case CHIP_ESP32C3:  chip_model_str = "ESP32-C3";  break;
    case CHIP_ESP32H2:  chip_model_str = "ESP32-H2";  break;
    case CHIP_ESP32C2:  chip_model_str = "ESP32-C2";  break;
    case CHIP_ESP32C6:  chip_model_str = "ESP32-C6";  break;
    case CHIP_ESP32P4:  chip_model_str = "ESP32-P4";  break;
    default:            chip_model_str = "Unknown";   break;
    }

    printf("\r\n");
    printf("Firmware: MiBeeCam v0.2.0\r\n");
    printf("Build: %s %s\r\n", __DATE__, __TIME__);
    printf("IDF: %s\r\n", esp_get_idf_version());
    printf("Chip: %s Rev %d\r\n", chip_model_str, chip_info.revision);
    at_ok();
}

/**
 * Handler for: AT+RESTORE  (factory reset + reboot)
 */
static void handle_restore(const char *params)
{
    (void)params;
    ESP_LOGW(TAG, "AT+RESTORE: factory reset");
    config_reset();
    at_ok();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/**
 * Handler for: AT+HEAP  (heap usage info)
 */
static void handle_heap(const char *params)
{
    (void)params;
    size_t current_free, baseline_free, baseline_min;
    bool warning;

    health_check_threshold(&current_free, &warning);
    health_get_baselines(&baseline_free, &baseline_min);
    size_t min_ever = esp_get_minimum_free_heap_size();

    printf("\r\n");
    printf("Free: %u bytes\r\n", (unsigned)current_free);
    printf("Min: %u bytes\r\n", (unsigned)min_ever);
    printf("Baseline: %u bytes\r\n", (unsigned)baseline_free);
    at_ok();
}

/**
 * Handler for: AT+UPTIME  (uptime in seconds)
 */
static void handle_uptime(const char *params)
{
    (void)params;
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    printf("\r\n");
    printf("Uptime: %u seconds\r\n", (unsigned)uptime_s);
    at_ok();
}

/**
 * Handler for: AT+TEMP  (chip temperature)
 */
static void handle_temp(const char *params)
{
    (void)params;
    float temp = get_chip_temp();
    int int_part = (int)temp;
    int dec_part = (int)((temp - int_part) * 100.0f);
    if (dec_part < 0) dec_part = -dec_part;

    printf("\r\n");
    printf("Temp: %d.%02d C\r\n", (int)temp, dec_part);
    at_ok();
}

/* ---------------------------------------------------------------------------
 * WiFi state to string mapping
 * -------------------------------------------------------------------------*/
static const char *wifi_state_str(wifi_state_t state)
{
    switch (state) {
    case WIFI_STATE_AP:               return "AP";
    case WIFI_STATE_STA_CONNECTING:   return "CONNECTING";
    case WIFI_STATE_STA_CONNECTED:    return "CONNECTED";
    case WIFI_STATE_STA_DISCONNECTED: return "DISCONNECTED";
    case WIFI_STATE_STA_FAILED:       return "FAILED";
    default:                          return "UNKNOWN";
    }
}

/**
 * Handler for: AT+CWJAP?  (query current WiFi connection)
 */
static void handle_cwjap_query(const char *params)
{
    (void)params;
    const cam_config_t *cfg = config_get();

    printf("\r\n");
    printf("State: %s\r\n", wifi_state_str(wifi_get_state()));
    printf("SSID: %s\r\n", cfg->wifi_ssid[0] ? cfg->wifi_ssid : "(none)");
    printf("IP: %s\r\n", wifi_get_ip_str());
    at_ok();
}

/**
 * @brief Strip enclosing double quotes from a string in-place.
 *        Modifies the string if quoted; otherwise returns it unchanged.
 */
static char *strip_quotes(char *s)
{
    if (!s) return s;
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        s[len - 1] = '\0';
        return s + 1;
    }
    return s;
}

/**
 * Handler for: AT+CWJAP=<ssid>,<pwd>  (set WiFi and connect)
 *
 * Parse format: ssid,pwd  or  "ssid","pwd"
 * Splits on first comma (ssid may contain commas if quoted?  No — standard
 * AT format splits on first comma as field delimiter).
 */
static void handle_cwjap_set(const char *params)
{
    if (!params || params[0] == '\0') {
        at_error();
        return;
    }

    /* Copy params to a mutable buffer */
    char buf[AT_LINE_MAX];
    strncpy(buf, params, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Split on first comma: ssid,pwd */
    char *comma = strchr(buf, ',');
    if (!comma) {
        at_error_msg("missing password");
        return;
    }
    *comma = '\0';
    char *ssid_str = buf;
    char *pwd_str  = comma + 1;

    /* Strip quotes */
    ssid_str = strip_quotes(ssid_str);
    pwd_str  = strip_quotes(pwd_str);

    /* Validate lengths */
    if (strlen(ssid_str) == 0) {
        at_error_msg("SSID empty");
        return;
    }
    if (strlen(ssid_str) > 32) {
        at_error_msg("SSID too long (max 32)");
        return;
    }
    if (strlen(pwd_str) > 64) {
        at_error_msg("password too long (max 64)");
        return;
    }

    /* Copy config from current, overwrite wifi fields */
    cam_config_t cfg;
    config_get_copy(&cfg);
    strncpy(cfg.wifi_ssid, ssid_str, sizeof(cfg.wifi_ssid) - 1);
    cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0';
    strncpy(cfg.wifi_pass, pwd_str, sizeof(cfg.wifi_pass) - 1);
    cfg.wifi_pass[sizeof(cfg.wifi_pass) - 1] = '\0';

    esp_err_t ret = config_set(&cfg);
    if (ret != ESP_OK) {
        at_error();
        return;
    }

    /* Start STA mode */
    /* Stop WiFi retry timer before connecting (prevent state machine desync) */
    wifi_stop_retry();
    ret = wifi_start_sta(ssid_str, pwd_str);
    if (ret == ESP_OK) {
        at_ok();
    } else {
        at_error();
    }
}

/**
 * Handler for: AT+CWJAP2?  (query backup WiFi configuration)
 */
static void handle_cwjap2_query(const char *params)
{
    (void)params;
    const cam_config_t *cfg = config_get();
    printf("\r\n");
    printf("Backup SSID: %s\r\n", cfg->wifi_ssid2[0] ? cfg->wifi_ssid2 : "(none)");
    printf("Active SSID: %s\r\n", cfg->wifi_ssid);
    printf("Backup status: %s\r\n", cfg->wifi_ssid2[0] ? "configured" : "not set");
    at_ok();
}

/**
 * Handler for: AT+CWJAP2=<ssid>,<pwd>  (set backup WiFi credentials)
 */
static void handle_cwjap2_set(const char *params)
{
    if (!params || params[0] == '\0') {
        at_error();
        return;
    }

    /* Copy params to a mutable buffer */
    char buf[AT_LINE_MAX];
    strncpy(buf, params, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Split on first comma: ssid,pwd */
    char *comma = strchr(buf, ',');
    if (!comma) {
        at_error_msg("missing password");
        return;
    }
    *comma = '\0';
    char *ssid_str = buf;
    char *pwd_str  = comma + 1;

    /* Strip quotes */
    ssid_str = strip_quotes(ssid_str);
    pwd_str  = strip_quotes(pwd_str);

    /* Validate lengths */
    if (strlen(ssid_str) == 0) {
        at_error_msg("SSID empty");
        return;
    }
    if (strlen(ssid_str) > 32) {
        at_error_msg("SSID too long (max 32)");
        return;
    }
    if (strlen(pwd_str) > 64) {
        at_error_msg("password too long (max 64)");
        return;
    }

    /* Copy config from current, overwrite backup wifi fields */
    cam_config_t cfg;
    config_get_copy(&cfg);
    strncpy(cfg.wifi_ssid2, ssid_str, sizeof(cfg.wifi_ssid2) - 1);
    cfg.wifi_ssid2[sizeof(cfg.wifi_ssid2) - 1] = '\0';
    strncpy(cfg.wifi_pass2, pwd_str, sizeof(cfg.wifi_pass2) - 1);
    cfg.wifi_pass2[sizeof(cfg.wifi_pass2) - 1] = '\0';

    esp_err_t ret = config_set(&cfg);
    if (ret == ESP_OK) {
        at_ok();
    } else {
        at_error();
    }
}

/**
 * Handler for: AT+CWQAP  (disconnect STA, switch to AP)
 */
static void handle_cwqap(const char *params)
{
    (void)params;
    esp_err_t ret = wifi_start_ap();
    if (ret == ESP_OK) {
        at_ok();
    } else {
        at_error();
    }
}

/**
 * Handler for: AT+CIFSR  (get IP address)
 */
static void handle_cifsr(const char *params)
{
    (void)params;
    at_send("+CIFSR:%s", wifi_get_ip_str());
    at_ok();
}

/**
 * Handler for: AT+CWLAP  (scan WiFi networks)
 *
 * Blocking call ~1-2 seconds. Only available when
 * CONFIG_MIBEECAM_ENABLE_WIFI_SCAN is enabled.
 */
static void handle_cwlap(const char *params)
{
    (void)params;
#ifdef CONFIG_MIBEECAM_ENABLE_WIFI_SCAN
    wifi_ap_record_t results[AT_SCAN_MAX_APS];
    uint16_t found = 0;

    esp_err_t ret = wifi_scan(results, AT_SCAN_MAX_APS, &found);
    if (ret != ESP_OK) {
        at_error();
        return;
    }

    for (uint16_t i = 0; i < found; i++) {
        at_send("+CWLAP:%s,%d,%d",
                results[i].ssid,
                results[i].rssi,
                results[i].authmode);
    }
    at_ok();
#else
    at_error_msg("WiFi scan not enabled");
#endif
}

/**
 * Handler for: AT+NAME?  (get device name)
 */
static void handle_name_query(const char *params)
{
    (void)params;
    const cam_config_t *cfg = config_get();
    at_send("+NAME:%s", cfg->device_name);
    at_ok();
}

/**
 * Handler for: AT+NAME=<name>  (set device name)
 */
static void handle_name_set(const char *params)
{
    if (!params || params[0] == '\0') {
        at_error_msg("name empty");
        return;
    }

    char buf[AT_LINE_MAX];
    strncpy(buf, params, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *name = strip_quotes(buf);

    if (strlen(name) > 32) {
        at_error_msg("name too long (max 32)");
        return;
    }
    if (strlen(name) == 0) {
        at_error_msg("name empty");
        return;
    }

    cam_config_t cfg;
    config_get_copy(&cfg);
    strncpy(cfg.device_name, name, sizeof(cfg.device_name) - 1);
    cfg.device_name[sizeof(cfg.device_name) - 1] = '\0';

    esp_err_t ret = config_set(&cfg);
    if (ret == ESP_OK) {
        at_ok();
    } else {
        at_error();
    }
}

/* ---------------------------------------------------------------------------
 * Config field metadata table (for CFGGET / CFGSET)
 * -------------------------------------------------------------------------*/

/** Field types for config get/set */
typedef enum {
    FIELD_TYPE_STRING,
    FIELD_TYPE_U8,
} field_type_t;

typedef struct {
    const char *name;
    field_type_t type;
    size_t offset;
    size_t max_len;       /* for strings: max length including null */
    uint8_t min_val;      /* for U8: minimum */
    uint8_t max_val;      /* for U8: maximum */
} config_field_t;

#define STRING_FIELD(field) \
    { #field, FIELD_TYPE_STRING, offsetof(cam_config_t, field), sizeof(((cam_config_t*)0)->field), 0, 0 }

#define U8_FIELD(field, minv, maxv) \
    { #field, FIELD_TYPE_U8, offsetof(cam_config_t, field), 0, minv, maxv }

static const config_field_t s_config_fields[] = {
    STRING_FIELD(wifi_ssid),
    STRING_FIELD(wifi_pass),
    STRING_FIELD(wifi_ssid2),
    STRING_FIELD(wifi_pass2),
    STRING_FIELD(device_name),
    STRING_FIELD(server_url),
    STRING_FIELD(timezone),
    STRING_FIELD(web_password),
    STRING_FIELD(mdns_hostname),
    STRING_FIELD(webhook_url),
    U8_FIELD(resolution,        0, 3),
    U8_FIELD(fps,               1, 30),
    U8_FIELD(jpeg_quality,      1, 63),
    U8_FIELD(motion_threshold,  1, 255),
    U8_FIELD(motion_cooldown,   1, 255),
    U8_FIELD(onvif_enabled,     0, 1),
    U8_FIELD(ws_enabled,        0, 1),
};
static const int s_config_fields_count = sizeof(s_config_fields) / sizeof(s_config_fields[0]);

/** Find a config field descriptor by name, or return NULL. */
static const config_field_t *find_config_field(const char *name)
{
    for (int i = 0; i < s_config_fields_count; i++) {
        if (strcmp(s_config_fields[i].name, name) == 0) {
            return &s_config_fields[i];
        }
    }
    return NULL;
}

/**
 * Get a pointer to a config field value given its descriptor.
 */
static const void *config_field_ptr(const cam_config_t *cfg, const config_field_t *f)
{
    return (const uint8_t *)cfg + f->offset;
}

/**
 * Set a config field value from a string.
 * Returns ESP_OK on success, ESP_ERR_INVALID_ARG on bad value.
 */
static esp_err_t config_field_set_from_str(cam_config_t *cfg, const config_field_t *f, const char *value)
{
    if (!cfg || !f || !value) return ESP_ERR_INVALID_ARG;

    if (f->type == FIELD_TYPE_STRING) {
        size_t vlen = strlen(value);
        if (vlen >= f->max_len) {
            return ESP_ERR_INVALID_ARG;
        }
        char *dest = (char *)((uint8_t *)cfg + f->offset);
        strncpy(dest, value, f->max_len - 1);
        dest[f->max_len - 1] = '\0';
        return ESP_OK;
    }

    if (f->type == FIELD_TYPE_U8) {
        long val = strtol(value, NULL, 10);
        if (val < (long)f->min_val || val > (long)f->max_val) {
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t *dest = (uint8_t *)((uint8_t *)cfg + f->offset);
        *dest = (uint8_t)val;
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

/**
 * Handler for: AT+CFGGET=<field>  (get config field value)
 */
static void handle_cfgget(const char *params)
{
    if (!params || params[0] == '\0' || strcmp(params, "?") == 0) {
        at_error_msg("missing field name");
        return;
    }

    const config_field_t *f = find_config_field(params);
    if (!f) {
        at_error_msg("unknown field");
        return;
    }

    const cam_config_t *cfg = config_get();

    if (f->type == FIELD_TYPE_STRING) {
        const char *s = (const char *)config_field_ptr(cfg, f);
        at_send("+CFGGET:%s=%s", f->name, s);
    } else if (f->type == FIELD_TYPE_U8) {
        uint8_t v = *(const uint8_t *)config_field_ptr(cfg, f);
        at_send("+CFGGET:%s=%u", f->name, (unsigned)v);
    }
    at_ok();
}

/**
 * Parse a "field,value" pair from a string.
 * Splits on first comma. Trims whitespace from field name.
 */
static esp_err_t parse_field_value(const char *input, char *field, size_t field_sz,
                                    char *value, size_t value_sz)
{
    if (!input || !field || !value) return ESP_ERR_INVALID_ARG;

    /* Copy to mutable buffer */
    char buf[AT_LINE_MAX];
    strncpy(buf, input, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Split on first comma */
    char *comma = strchr(buf, ',');
    if (!comma) {
        return ESP_ERR_INVALID_ARG;
    }
    *comma = '\0';
    char *f = buf;
    char *v = comma + 1;

    /* Trim leading/trailing whitespace from field name */
    while (*f == ' ' || *f == '\t') f++;
    char *end = f + strlen(f) - 1;
    while (end > f && (*end == ' ' || *end == '\t')) *end-- = '\0';

    /* Strip quotes from value */
    v = strip_quotes(v);

    strncpy(field, f, field_sz - 1);
    field[field_sz - 1] = '\0';
    strncpy(value, v, value_sz - 1);
    value[value_sz - 1] = '\0';

    if (strlen(field) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/**
 * Handler for: AT+CFGSET=<field>,<value>  (set config field)
 *
 * In-memory only until AT+SAVE is issued.
 */
static void handle_cfgset(const char *params)
{
    if (!params || params[0] == '\0') {
        at_error_msg("missing field and value");
        return;
    }

    char field[AT_FIELD_MAX];
    char value[AT_VALUE_MAX];

    if (parse_field_value(params, field, sizeof(field), value, sizeof(value)) != ESP_OK) {
        at_error_msg("invalid format: expected field,value");
        return;
    }

    const config_field_t *f = find_config_field(field);
    if (!f) {
        at_error_msg("unknown field");
        return;
    }

    cam_config_t cfg;
    config_get_copy(&cfg);
    esp_err_t ret = config_field_set_from_str(&cfg, f, value);
    if (ret != ESP_OK) {
        at_error_msg("invalid value for field");
        return;
    }

    ret = config_set(&cfg);
    if (ret == ESP_OK) {
        at_ok();
    } else {
        at_error();
    }
}

/**
 * Handler for: AT+SAVE  (save config to NVS)
 */
static void handle_save(const char *params)
{
    (void)params;
    esp_err_t ret = config_save(config_get());
    if (ret == ESP_OK) {
        at_ok();
    } else {
        at_error();
    }
}

/**
 * Handler for: AT+STREAM?  (query stream + motion status)
 */
static void handle_stream_query(const char *params)
{
    (void)params;
    int clients = mjpeg_streamer_get_client_count();
    bool motion_running = motion_detect_is_running();

    printf("\r\n");
    printf("Stream clients: %d\r\n", clients);
    printf("Motion detect: %s\r\n", motion_running ? "running" : "stopped");
    at_ok();
}

/* ---------------------------------------------------------------------------
 * Command registry
 * -------------------------------------------------------------------------*/

typedef struct {
    const char *cmd;   /**< Command suffix (e.g., "+RST", "+CWJAP") */
    void (*handler)(const char *params);
} at_cmd_entry_t;

static const at_cmd_entry_t s_cmds[] = {
    /* Basic commands */
    { "",                 handle_at          },   /* AT (no suffix) */

    /* System commands */
    { "+RST",             handle_rst         },
    { "+GMR",             handle_gmr         },
    { "+RESTORE",         handle_restore     },
    { "+HEAP",            handle_heap        },
    { "+UPTIME",          handle_uptime      },
    { "+TEMP",            handle_temp        },

    /* WiFi commands */
    { "+CWJAP?",          handle_cwjap_query },
    { "+CWJAP=",          handle_cwjap_set   },
    { "+CWJAP2?",         handle_cwjap2_query },
    { "+CWJAP2=",         handle_cwjap2_set   },
    { "+CWQAP",           handle_cwqap       },
    { "+CIFSR",           handle_cifsr       },
    { "+CWLAP",           handle_cwlap       },

    /* Device name commands */
    { "+NAME?",           handle_name_query  },
    { "+NAME=",           handle_name_set    },

    /* Config commands */
    { "+CFGGET=",         handle_cfgget      },
    { "+CFGSET=",         handle_cfgset      },
    { "+SAVE",            handle_save        },

    /* Stream commands */
    { "+STREAM?",         handle_stream_query },
};
static const int s_cmds_count = sizeof(s_cmds) / sizeof(s_cmds[0]);

/* ---------------------------------------------------------------------------
 * Command dispatch
 * -------------------------------------------------------------------------*/

/**
 * @brief Match a command string against the command table and dispatch.
 *
 * @param cmd_suffix  The part after "AT" (e.g., "+RST", "+CWJAP=", "+CWJAP?")
 * @param params      The parameter string (after "="), or "?" for queries, or "" for no params
 */
static void dispatch_command(const char *cmd_suffix, const char *params)
{
    for (int i = 0; i < s_cmds_count; i++) {
        if (strcmp(cmd_suffix, s_cmds[i].cmd) == 0) {
            s_cmds[i].handler(params);
            return;
        }
    }

    /* No match: if it looks like an extended command (+XXX), report error */
    at_error();
}

/* ---------------------------------------------------------------------------
 * AT command parser task
 * -------------------------------------------------------------------------*/

/**
 * @brief Dedicated task that reads lines from stdin and dispatches AT commands.
 *
 * Lines that do not start with "AT" (case-insensitive for the "AT" prefix)
 * are silently ignored (they could be log output mixed into the stream).
 */
static void at_command_task(void *arg)
{
    (void)arg;
    char line[AT_LINE_MAX];

    ESP_LOGI(TAG, "AT command task started");

    while (s_running) {
        /* Read one line via stdin (connected to UART0 via VFS) */
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Strip trailing \r and \n */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
            line[--len] = '\0';
        }

        /* Skip empty lines */
        if (len == 0) {
            continue;
        }

        /* Check for "AT" prefix (case-insensitive for "AT" only) */
        if (len >= 2 &&
            (line[0] == 'A' || line[0] == 'a') &&
            (line[1] == 'T' || line[1] == 't')) {

            const char *suffix = line + 2;   /* Everything after "AT" */

            /* If exactly "AT" with no suffix */
            if (suffix[0] == '\0') {
                dispatch_command("", "");
                continue;
            }

            /* For commands with "=" or "?": split suffix from params */
            /* Scan for the first = or ? */
            const char *eq_or_qmark = NULL;
            int idx = 0;
            while (suffix[idx] != '\0') {
                if (suffix[idx] == '=' || suffix[idx] == '?') {
                    eq_or_qmark = suffix + idx;
                    break;
                }
                idx++;
            }

            if (eq_or_qmark) {
                /* Build command suffix including the = or ? */
                char cmd_buf[AT_CMD_BUF_MAX];
                int cmd_len = (int)(eq_or_qmark - suffix) + 1; /* include = or ? */
                if (cmd_len >= (int)sizeof(cmd_buf)) {
                    at_error();
                    continue;
                }
                strncpy(cmd_buf, suffix, cmd_len);
                cmd_buf[cmd_len] = '\0';

                const char *params = eq_or_qmark + 1;

                /* For query commands (?), params is "?" part (already handled via cmd_buf) */
                if (*eq_or_qmark == '?') {
                    /* For query commands, pass "?" as params so handlers can distinguish */
                    dispatch_command(cmd_buf, "?");
                } else {
                    /* For set commands (=), pass everything after = */
                    dispatch_command(cmd_buf, params);
                }
            } else {
                /* No = or ? — this is a command with no params */
                char cmd_buf[AT_CMD_BUF_MAX];
                strncpy(cmd_buf, suffix, sizeof(cmd_buf) - 1);
                cmd_buf[sizeof(cmd_buf) - 1] = '\0';
                dispatch_command(cmd_buf, "");
            }
        }
        /* Lines not starting with "AT" are silently ignored (log output, etc.) */
    }

    ESP_LOGI(TAG, "AT command task stopped");
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 * UART / VFS initialization
 * -------------------------------------------------------------------------*/

/**
 * @brief Initialize UART0 for AT command I/O.
 *
 * Attempts to install the UART0 driver and connect it to the VFS layer
 * (stdin/stdout). If the driver is already installed by the console
 * subsystem, we just use the existing stdin/stdout connection.
 */
static void uart_vfs_init(void)
{
    /* Configure UART0 */
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(UART_NUM_0, 512, 512, 20, NULL, 0);
    if (ret == ESP_OK) {
        /* Fresh install — configure and connect to VFS */
        uart_param_config(UART_NUM_0, &uart_config);
        uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        uart_vfs_dev_use_driver(UART_NUM_0);
        ESP_LOGI(TAG, "UART0 driver installed for AT commands");
    } else {
        /* Driver already installed by console subsystem — use existing */
        ESP_LOGW(TAG, "UART0 already in use (%s), using existing console", esp_err_to_name(ret));
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

esp_err_t at_command_init(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "AT command interface already initialized");
        return ESP_OK;
    }

    /* Initialize UART/VFS for stdin/stdout access */
    uart_vfs_init();

    /* Start the AT command parsing task */
    s_running = true;
    BaseType_t ret = xTaskCreate(at_command_task, "at_cmd", 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "Failed to create AT command task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "AT command interface initialized");
    return ESP_OK;
}

esp_err_t at_command_deinit(void)
{
    s_running = false;
    ESP_LOGI(TAG, "AT command interface deinitialized");
    return ESP_OK;
}
