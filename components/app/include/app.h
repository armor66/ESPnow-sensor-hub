#ifndef APP_H
#define APP_H

#include "relay.h"
#include "modem.h"
#include "mqtt.h"

typedef struct app_config_t {
    modem_config_t modem_config;
    mqtt_config_t mqtt_config;
    int relay_pin;
    int mqtt_led_pin;
    int button_pin;     // GPIO кнопки; < 0 — кнопка отключена
    int bat_alrm_pin;
} app_config_t;

void app_start(const app_config_t *config);

#endif
