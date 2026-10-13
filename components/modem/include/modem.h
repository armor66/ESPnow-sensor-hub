#ifndef MODEM_H
#define MODEM_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct modem_ctx_t* modem_handle_t;

typedef struct {
    int pin_rx;
    int pin_tx;
    int pin_cts;
    int pin_rts;
    int pin_pwrkey;     // PWRKEY через инвертор VT2: уровень 1 => PWRKEY к земле
    int pin_dc_dc_en;   // включение DC-DC питания модема (3.8В)
    int pin_status;     // pull-up, active low(inverted) when initialization is completed the status outputs a high level
    int pin_usb_plug;   // pull-up, active low, high if usb not plugged
    const char *apn;
} modem_config_t;

// Конструктор. Выполняет холодный запуск модема (питание + PWRKEY), поднимает
// UART и создаёт DCE. Сеть НЕ поднимает. Возвращает NULL при ошибке.
modem_handle_t modem_driver_init(const modem_config_t *config);

// Поднять сеть (sync + режим + ожидание PPP). По первому успеху запускает
// фоновый надзор, который сам переподнимает модем при обрыве.
esp_err_t modem_driver_start_network(modem_handle_t handle, uint32_t timeout_ms);

// Перезапуск модема без ребута МК: выключение длинным импульсом PWRKEY,
// затем включение. UART/DCE не пересоздаются.
esp_err_t modem_driver_restart(modem_handle_t handle);

// true, если PPP-соединение сейчас установлено.
bool modem_driver_is_connected(modem_handle_t handle);

esp_err_t modem_driver_get_signal_quality(modem_handle_t handle, int *rssi);

void modem_driver_destroy(modem_handle_t handle);

esp_err_t modem_shutdown(modem_handle_t handle);

#endif
