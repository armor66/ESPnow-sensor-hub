#include "battery_monitor.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "BATTERY";

#define BAT_ADC_UNIT        ADC_UNIT_1
#define BAT_ADC_CHANNEL     ADC_CHANNEL_7  // GPIO 35
#define BAT_ADC_ATTEN       ADC_ATTEN_DB_12
#define BAT_ADC_BITWIDTH    ADC_BITWIDTH_12
#define BATT_MIN_MV         3000        // Minimum voltage (3V)
#define BATT_MAX_MV         4100        // Maximum voltage (4.05V)

static adc_oneshot_unit_handle_t adc1_handle;

esp_err_t battery_monitor_init(void) {
    // ADC initialization
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = BAT_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));
    
    // Channel configuration
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = BAT_ADC_BITWIDTH,
        .atten = BAT_ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, BAT_ADC_CHANNEL, &config));
    
    ESP_LOGI(TAG, "Battery monitor initialized");
    return ESP_OK;
}

esp_err_t battery_monitor_read(battery_info_t *info) {
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // SAFETY CHECK: Prevent crash if init() was not called
    if (adc1_handle == NULL) {
        ESP_LOGE(TAG, "did battery_monitor_init() crashed?");
        return ESP_ERR_INVALID_STATE;
    }

    // Read ADC value
    int adc_raw;
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, BAT_ADC_CHANNEL, &adc_raw));
    
    // Simple conversion without calibration (approximate)
    // For ADC_ATTEN_DB_11 maximum value is approximately 3.3V
    uint32_t voltage_mv = (adc_raw * 330) / 395;      // 3300 / 4095
    
    // Multiply by 2 if using a 1:1 voltage divider
    voltage_mv *= 2;
    
    // Calculate battery percentage
    int percent = (voltage_mv - BATT_MIN_MV) / ((BATT_MAX_MV - BATT_MIN_MV) / 100);

    if (percent > 100) percent = 100;
    if (voltage_mv < BATT_MIN_MV) percent = 0;
    
    // Fill the structure
    info->voltage_mv = voltage_mv;
    info->level = percent;
    
    ESP_LOGI(TAG, "Battery: %lu mV, Level: %d%%", voltage_mv, percent);
    return ESP_OK;
}

// bool if_battery_low(battery_info_t *info) {
//     return (info->level < 18)? true: false;
// }
// bool if_battery_empty(battery_info_t *info) {
//     return (info->level == 0)? true: false;
// }