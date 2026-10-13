#include "app.h"
#include "esp_log.h"
#include "esp_log_level.h"

// Управляющие выводы   LilyGo-ESP32-WROWER
#define MODEM_PWR_KEY_PIN  4    // inv Q2 output    //14   // PWRKEY (через инвертор Q2)
#define MODEM_RESET_PIN     -1  //5 (RV1 deleted)   // inv Q1 output, LOW (через инвертор Q1)
#define MODEM_TX           26   // RS0108Y          //9 ESP TX -> модем RXD (через TXS0104E)
#define MODEM_RX           27   // RS0108Y          //10 ESP RX <- модем TXD (через TXS0104E)
#define MODEM_CTS          21   //11
#define MODEM_RTS          22   //12
#define MODEM_DTR           25  // RS0108Y output, LOW
#define MODEM_RI            33  // RS0108Y input
#define MODEM_STATUS        0   // RV4->LED2 Input, active low via invertor (at modem active high(1.8V))
#define VBUS_USB_PLUG       23  // active 0, 1 if usb not plugged

#define PIN_DCDC_EN        12   // D21 in parallel D21-VBUS -> BAT-Q8, V3V-Q11 -> VDD3V3 //13 EN преобразователя MP2307 (питание модема)
#define MQTT_LED_PIN       18   // индикаторный светодиод VD9
#define RELAY_PIN          19               //40   // реле K1 (через VT3)

// GPIO кнопки. Пока не разведён в прошивке: -1 отключает компонент кнопки.
#define BUTTON_PIN          32              //8
#define BUTTERY_ADC         35  //VV_BAT->100k-ADC-100k-GND
#define BUTTERY_ALRM        5   // low battery (3300mV) alarm
#define SOLAR_ADC           36  //SOL_IN->100k-ADC-100k-GND

/*
поддерживает топики wildcard и слеш в самом начале топика

mqtt [srv2.clusterfly.ru]
SSL [выкл]  tcp:9991
SSL [вкл]   ssl:9992
SSL [выкл]  ws:9993
SSL [вкл]   wss:9994
user	user_xxxxxxxx
password    xxxxxxxxxxxxx
*/

/*SD card: 2, 13, 14, 15*/
/*Free PINs: 39(ADC), 34(GPIO 34–39 input only pins)*/
void app_main(void) {
    esp_log_level_set("esp-tls", ESP_LOG_NONE);
    esp_log_level_set("transport_base", ESP_LOG_NONE);
//    esp_log_level_set("mqtt_client", ESP_LOG_NONE);

    const app_config_t config = {
        .modem_config = {
            .pin_rx = MODEM_RX,
            .pin_tx = MODEM_TX,
            .pin_cts = MODEM_CTS,
            .pin_rts = MODEM_RTS,
            .pin_pwrkey = MODEM_PWR_KEY_PIN,
            .pin_dc_dc_en = PIN_DCDC_EN,
            .pin_status = MODEM_STATUS,
            .pin_usb_plug = VBUS_USB_PLUG,
            .apn = "internet.mts.ru"
        },
        .mqtt_config = {
//          .uri = "mqtt://broker.hivemq.com:1883",
            .uri = "mqtt://srv2.clusterfly.ru:9991",
            .client_id = "esp32-master-gateway",
            .username = "user_xxxxxxxx",
            .password = "xxxxxxxxxxxxx",
            // .timeout_ms = 5000,
            // .reconnect_timeout_ms = 5000,
            .cmd_topic = "user_xxxxxxxx/plug/command",
            .resp_topic = "user_xxxxxxxx/plug/state"
        },
        .relay_pin = RELAY_PIN,
        .mqtt_led_pin = MQTT_LED_PIN,
        .button_pin = BUTTON_PIN,
        .bat_alrm_pin = BUTTERY_ALRM
    };

    app_start(&config);
}
