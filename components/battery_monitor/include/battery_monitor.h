#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Battery information structure
 */
typedef struct {
    uint32_t voltage_mv;    // Voltage in millivolts
    int level;              // Battery level in percentage (0-100%)
} battery_info_t;

/**
 * @brief Initialize battery monitoring module
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t battery_monitor_init(void);

/**
 * @brief Read current battery status
 * 
 * @param info Pointer to structure to store battery information
 * @return esp_err_t ESP_OK on success
 */
esp_err_t battery_monitor_read(battery_info_t *info);

// bool if_battery_low(battery_info_t *info);
// bool if_battery_empty(battery_info_t *info);

#ifdef __cplusplus
}
#endif 