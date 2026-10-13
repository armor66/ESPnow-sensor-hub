#include "modem.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_modem_api.h"

#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_sleep.h"

// события модема
#define MODEM_CONNECTED_BIT BIT0

// Тайминги питания/включения
#define MODEM_POWER_DRAIN_MS   2000   // снять DC-DC и дать стечь ёмкостям
#define MODEM_VBAT_SETTLE_MS   500    // стабилизация 3.8В после включения DC-DC
#define MODEM_PWRKEY_ON_MS     100   // импульс PWRKEY для включения (typ~50mS)
#define MODEM_PWRKEY_OFF_MS    3000   // удержание PWRKEY для выключения (min~2.5с)
#define MODEM_OFF_SETTLE_MS    2000   // пауза для выключения (typ~1.9с)
#define MODEM_OFF_ON_BUF_MS    2500   // пауза после выключения перед включением (min~2с)
#define MODEM_BOOT_MS          6000   // ожидание готовности UART модема

// Синхронизация AT
#define MODEM_SYNC_RETRIES_COUNT 6
#define MODEM_SYNC_RETRY_DELAY_MS 2000

// Фоновый надзор
#define MODEM_SUPERVISOR_PERIOD_MS 10000
#define MODEM_DEFAULT_TIMEOUT_MS   30000

static const char *TAG = "MODEM_DRIVER";

struct modem_ctx_t {
    esp_modem_dce_t *dce;
    esp_netif_t *netif;
    EventGroupHandle_t event_group;
    SemaphoreHandle_t lock;       // сериализует операции с DCE
    TaskHandle_t supervisor;      // фоновый надзор (NULL пока не запущен)
    int pin_pwrkey;
    int pin_dc_dc_en;
    int pin_status;
    int pin_usb_plug;
    uint32_t connect_timeout_ms;  // таймаут для переподключения в надзоре
};

// ---- IP-события -------------------------------------------------------------

static void on_ip_event(
    void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data
) {
    struct modem_ctx_t *ctx = (struct modem_ctx_t*) arg;

    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ESP_LOGI(TAG, "PPP соединение установлено!");
        xEventGroupSetBits(ctx->event_group, MODEM_CONNECTED_BIT);
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP соединение разорвано");
        xEventGroupClearBits(ctx->event_group, MODEM_CONNECTED_BIT);
    }
}

// ---- Управление питанием ----------------------------------------------------

// Включить модем коротким импульсом PWRKEY (DC-DC уже должен быть включён).
static void modem_power_on_pulse(modem_handle_t ctx) {
    gpio_set_level(ctx->pin_pwrkey, 1);   // PWRKEY -> земля
    vTaskDelay(pdMS_TO_TICKS(MODEM_PWRKEY_ON_MS));
    gpio_set_level(ctx->pin_pwrkey, 0);
}

// Выключить модем длинным удержанием PWRKEY (безопасное программное выключение).
static void modem_power_off_pulse(modem_handle_t ctx) {
    gpio_set_level(ctx->pin_pwrkey, 1);   // PWRKEY -> земля на 2.5с
    vTaskDelay(pdMS_TO_TICKS(MODEM_PWRKEY_OFF_MS));
    gpio_set_level(ctx->pin_pwrkey, 0);
}

// Холодный запуск: гарантированно снять питание, затем включить заново.
// Нужен на первом старте, чтобы модем не остался в data/CMUX от прошлого раза.
static void modem_cold_start(modem_handle_t ctx) {
    gpio_set_direction(ctx->pin_pwrkey, GPIO_MODE_OUTPUT);
    gpio_set_direction(ctx->pin_dc_dc_en, GPIO_MODE_OUTPUT);
    gpio_set_direction(ctx->pin_status, GPIO_MODE_INPUT);
    gpio_set_pull_mode(ctx->pin_status, GPIO_PULLUP_ONLY);
    gpio_set_direction(ctx->pin_usb_plug, GPIO_MODE_INPUT);
    gpio_set_pull_mode(ctx->pin_usb_plug, GPIO_PULLUP_ONLY);

    gpio_set_level(ctx->pin_pwrkey, 0);
    gpio_set_level(ctx->pin_dc_dc_en, 0);
    vTaskDelay(pdMS_TO_TICKS(MODEM_POWER_DRAIN_MS));

    gpio_set_level(ctx->pin_dc_dc_en, 1);
    vTaskDelay(pdMS_TO_TICKS(MODEM_VBAT_SETTLE_MS));

    modem_power_on_pulse(ctx);
    vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_MS));
}

static void enter_indefinite_deep_sleep(modem_handle_t ctx) {
    while (!gpio_get_level(ctx->pin_status)) {
        vTaskDelay(pdMS_TO_TICKS(500));     // 2.5 seconds usually
        ESP_LOGI(TAG, "Wait for modem STATUS outputs a LOW level (inverted)");
    }
    /* enter_indefinite_deep_sleep by disabling all wakeup sources */
    esp_err_t err = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "Entering indefinite deep sleep; external reset or power cycle is required");
        esp_deep_sleep_start();
    } else {
        ESP_LOGE(TAG, "Failed to disable deep-sleep wakeup sources: %s", esp_err_to_name(err));
    }
//    gpio_set_level(ctx->pin_dc_dc_en, 0);
}

esp_err_t modem_shutdown(modem_handle_t handle) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
/* Can power off module by AT command 'AT+CPOF' */
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    ESP_LOGW(TAG, "Выключение модема (PWRKEY off 3sec, settle ~2sec, power off)");
    modem_power_off_pulse(handle);
//    vTaskDelay(pdMS_TO_TICKS(MODEM_OFF_SETTLE_MS));   // monitor modem STATUS outputs instead

    enter_indefinite_deep_sleep(handle);
    return 0;
}

// ---- Подъём сети ------------------------------------------------------------

// Синхронизация с восстановлением: если модем не отвечает на AT, возможно он
// застрял в DATA/CMUX — пробуем вернуть его в командный режим и повторить.
static esp_err_t modem_sync_with_recovery(modem_handle_t handle) {
    for (int try = 1; try <= MODEM_SYNC_RETRIES_COUNT; try++) {
        if (esp_modem_sync(handle->dce) == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "AT не отвечает, попытка %d из %d (возврат в командный режим)",
                 try, MODEM_SYNC_RETRIES_COUNT);
        esp_modem_set_mode(handle->dce, ESP_MODEM_MODE_COMMAND);
        vTaskDelay(pdMS_TO_TICKS(MODEM_SYNC_RETRY_DELAY_MS));
    }
    return ESP_FAIL;
}

// Подъём сети. Предполагает, что вызывающий держит ctx->lock.
static esp_err_t start_network_locked(modem_handle_t handle, uint32_t timeout_ms) {
    // выровнять состояние esp_modem с реальным (модем после рестарта в command)
    esp_modem_set_mode(handle->dce, ESP_MODEM_MODE_COMMAND);

    if (modem_sync_with_recovery(handle) != ESP_OK) {
        ESP_LOGE(TAG, "Модем не отвечает на AT-команды!");
        return ESP_FAIL;
    }

    int net_state = -1;
    esp_err_t err = esp_modem_get_network_registration_state(handle->dce, &net_state);

    if (err == ESP_OK) {
        switch (net_state) {
            case 0:
                ESP_LOGI("MODEM", "Не зарегистрирован, поиск сети не ведется.");
                break;
            case 1:
                ESP_LOGI("MODEM", "Успешно зарегистрирован! Домашняя сеть. (OK)");
                break;
            case 2:
                ESP_LOGI("MODEM", "Не зарегистрирован, но активно ищет сеть...");
                break;
            case 3:
                ESP_LOGE("MODEM", "В регистрации отказано оператором (проверьте SIM/баланс).");
                break;
            case 4:
                ESP_LOGW("MODEM", "Неизвестный статус.");
                break;
            case 5:
                ESP_LOGI("MODEM", "Успешно зарегистрирован! Роуминг. (OK)");
                break;
            default:
                ESP_LOGW("MODEM", "Получен недокументированный код статуса: %d", net_state);
                break;
        }
    } else {
    ESP_LOGE("MODEM", "Не удалось запросить статус сети у модема.");
    }

    // Пробуем CMUX (AT + PPP одновременно). Если не вышло — откат на DATA/PPP.
//    char response_buffer[256] = {0};
    esp_modem_at_raw(handle->dce, "ATE0", NULL, "OK", "ERROR", 100);
//    err = esp_modem_at_raw(handle->dce, "AT+CMUX=0,0\r", response_buffer, "OK", "ERROR", 1000);
//  esp_err_t 
    err = esp_modem_set_mode(handle->dce, ESP_MODEM_MODE_CMUX);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CMUX не запустился (%s), откат на DATA/PPP", esp_err_to_name(err));
        esp_modem_set_mode(handle->dce, ESP_MODEM_MODE_COMMAND);
        vTaskDelay(pdMS_TO_TICKS(MODEM_SYNC_RETRY_DELAY_MS));
        if (modem_sync_with_recovery(handle) != ESP_OK) {
            ESP_LOGE(TAG, "Модем не отвечает после отката CMUX!");
            return ESP_FAIL;
        }
//@my
        // esp_err_t err = esp_modem_set_mode(handle->dce, ESP_MODEM_MODE_COMMAND);
        // if (err != ESP_OK) {
        //     ESP_LOGE(TAG, "Не удалось перейти в COMMAND mode: %s", esp_err_to_name(err));
        //     return err;
        // }
//my@
        err = esp_modem_set_mode(handle->dce, ESP_MODEM_MODE_DATA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Не удалось перейти в DATA: %s", esp_err_to_name(err));
            return err;
        }
    }

    EventBits_t bits = xEventGroupWaitBits(
        handle->event_group, MODEM_CONNECTED_BIT,
        pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)
    );
    return (bits & MODEM_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

// ---- Фоновый надзор ---------------------------------------------------------

static void modem_supervisor_task(void *arg) {
    modem_handle_t handle = (modem_handle_t) arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MODEM_SUPERVISOR_PERIOD_MS));

        if (modem_driver_is_connected(handle)) {
            continue;
        }

        ESP_LOGW(TAG, "Надзор: соединение потеряно, перезапуск модема");
        if (modem_driver_restart(handle) == ESP_OK) {
            ESP_LOGI(TAG, "Надзор: модем восстановлен");
        } else {
            ESP_LOGE(TAG, "Надзор: не удалось восстановить модем, повтор позже");
        }
    }
}

static void modem_start_supervisor(modem_handle_t handle) {
    if (handle->supervisor != NULL) {
        return; // уже запущен
    }
    if (xTaskCreate(modem_supervisor_task, "modem_super", 4096, handle, 4,
                    &handle->supervisor) != pdPASS) {
        ESP_LOGW(TAG, "Не удалось запустить надзор за модемом");
        handle->supervisor = NULL;
    }
}

// ---- Публичный API ----------------------------------------------------------

modem_handle_t modem_driver_init(const modem_config_t *config) {
    if (config == NULL) {
        return NULL;
    }

    struct modem_ctx_t *ctx = calloc(1, sizeof(struct modem_ctx_t));
    if (ctx == NULL) {
        return NULL;
    }

    ctx->pin_pwrkey = config->pin_pwrkey;
    ctx->pin_dc_dc_en = config->pin_dc_dc_en;
    ctx->pin_status = config->pin_status;
    ctx->pin_usb_plug = config->pin_usb_plug;
    ctx->connect_timeout_ms = MODEM_DEFAULT_TIMEOUT_MS;

    ctx->lock = xSemaphoreCreateMutex();
    ctx->event_group = xEventGroupCreate();
    if (ctx->lock == NULL || ctx->event_group == NULL) {
        ESP_LOGE(TAG, "Не удалось создать примитивы синхронизации");
        if (ctx->lock) vSemaphoreDelete(ctx->lock);
        if (ctx->event_group) vEventGroupDelete(ctx->event_group);
        free(ctx);
        return NULL;
    }

    // Холодный запуск модема до создания UART/DCE.
    modem_cold_start(ctx);

    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    ctx->netif = esp_netif_new(&netif_ppp_config);
    if (ctx->netif == NULL) {
        ESP_LOGE(TAG, "Не удалось создать PPP netif");
        vSemaphoreDelete(ctx->lock);
        vEventGroupDelete(ctx->event_group);
        free(ctx);
        return NULL;
    }

    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, ctx);

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.tx_io_num = config->pin_tx;
    dte_config.uart_config.rx_io_num = config->pin_rx;
    dte_config.uart_config.rts_io_num = config->pin_rts;
    dte_config.uart_config.cts_io_num = config->pin_cts;
    dte_config.uart_config.flow_control = UART_HW_FLOWCTRL_CTS_RTS; // <--- CRITICAL FOR A7670E
    // Без аппаратного контроля потока: при FLOW_CONTROL_HW UART не передаёт,
    // пока модем не выставит CTS (нужен AT+IFC=2,2) — дедлок ещё до первого AT.
//my@    dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    dte_config.uart_config.rx_buffer_size = 16384;
    dte_config.uart_config.tx_buffer_size = 2048;
    dte_config.dte_buffer_size = 2048;
    dte_config.task_stack_size = 6144;

    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(config->apn);

    // SIMCOM A7670E (AT-совместим с SIM7600)
    ctx->dce = esp_modem_new_dev(
        ESP_MODEM_DCE_SIM7600, &dte_config, &dce_config, ctx->netif
    );
    if (ctx->dce == NULL) {
        ESP_LOGE(TAG, "Не удалось создать устройство модема");
        esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event);
        esp_netif_destroy(ctx->netif);
        vSemaphoreDelete(ctx->lock);
        vEventGroupDelete(ctx->event_group);
        free(ctx);
        return NULL;
    }

    return ctx;
}

esp_err_t modem_driver_start_network(modem_handle_t handle, uint32_t timeout_ms) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    handle->connect_timeout_ms = timeout_ms;

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    esp_err_t rc = start_network_locked(handle, timeout_ms);
    xSemaphoreGive(handle->lock);

    // Запускаем надзор в любом случае: даже если первое подключение не удалось,
    // он будет переподнимать модем в фоне — без ребута МК.
    modem_start_supervisor(handle);
    return rc;
}

esp_err_t modem_driver_restart(modem_handle_t handle) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    ESP_LOGI(TAG, "Перезапуск модема (PWRKEY off -> on)");
    modem_power_off_pulse(handle);
    vTaskDelay(pdMS_TO_TICKS(MODEM_OFF_SETTLE_MS));
    vTaskDelay(pdMS_TO_TICKS(MODEM_OFF_ON_BUF_MS));
    modem_power_on_pulse(handle);
    vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_MS));

    esp_err_t rc = start_network_locked(handle, handle->connect_timeout_ms);
    xSemaphoreGive(handle->lock);
    return rc;
}

bool modem_driver_is_connected(modem_handle_t handle) {
    if (handle == NULL) {
        return false;
    }
    return (xEventGroupGetBits(handle->event_group) & MODEM_CONNECTED_BIT) != 0;
}

esp_err_t modem_driver_get_signal_quality(modem_handle_t handle, int *rssi) {
    if (handle == NULL || rssi == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Не блокируемся, если идёт переподключение — вернём ошибку, статус будет
    // частичным.
    if (xSemaphoreTake(handle->lock, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    int ber;
    esp_err_t rc = esp_modem_get_signal_quality(handle->dce, rssi, &ber);
    xSemaphoreGive(handle->lock);
    return rc;
}

void modem_driver_destroy(modem_handle_t handle) {
    if (handle == NULL) {
        return;
    }
    if (handle->supervisor != NULL) {
        vTaskDelete(handle->supervisor);
    }
    esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event);
    esp_modem_destroy(handle->dce);
    esp_netif_destroy(handle->netif);
    vEventGroupDelete(handle->event_group);
    vSemaphoreDelete(handle->lock);
    free(handle);
}
