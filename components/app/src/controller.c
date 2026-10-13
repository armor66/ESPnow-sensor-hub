#include "controller.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REBOOT_FLUSH_DELAY_MS 500

static const char *TAG = "CONTROLLER";

struct app_controller_t {
    modem_handle_t modem;
    relay_handle_t relay;
    mqtt_handle_t mqtt;
};

static void clear_response_topic(app_controller_t *self) {
    if (self == NULL || self->mqtt == NULL) return;
    
    const char *resp_topic = mqtt_get_resp_topic(self->mqtt);
    if (resp_topic != NULL) {
        mqtt_clear_retained(self->mqtt, resp_topic);
    }
}
// ---- Публикация ответа ------------------------------------------------------
static esp_err_t publish_response(app_controller_t *self, const char *cmd,
                                    const char *result, const char *desc) {
    const char *resp_topic = mqtt_get_resp_topic(self->mqtt);
    if (resp_topic == NULL) {
        ESP_LOGW(TAG, "Топик ответов не задан, ответ не опубликован");
        return ESP_ERR_INVALID_STATE;
    }

    char *resp = form_json_response(cmd, result, desc);
    if (resp == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t rc = mqtt_app_publish(self->mqtt, resp_topic, resp);
    free(resp);

    return rc;
}
// ---- Публикация команды ------------------------------------------------------
static esp_err_t publish_command(app_controller_t *self, const char *cmd) {

    const char *cmd_topic = mqtt_get_cmd_topic(self->mqtt);
    if (cmd_topic == NULL) {
        ESP_LOGW(TAG, "Топик команд не задан, не опубликован");
        return ESP_ERR_INVALID_STATE;
    }

    char *command = form_json_cmd(cmd);
    if (command == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t cc = mqtt_app_publish(self->mqtt, cmd_topic, command);
    free(command);
    return cc;
}

// ---- Обработчики отдельных команд ------------------------------------------
// Каждый обработчик имеет единую сигнатуру — это позволяет хранить их в таблице.

typedef esp_err_t (*command_fn_t)(app_controller_t *self, const char *cmd, cJSON *payload);

static esp_err_t cmd_reboot(app_controller_t *self, const char *cmd, cJSON *payload) {
    publish_response(self, cmd, "ok", "restarting");
    // даём времени сообщению уйти в сеть до перезагрузки
    vTaskDelay(pdMS_TO_TICKS(REBOOT_FLUSH_DELAY_MS));
    esp_restart();
    return ESP_OK; // не достигается, чтобы компилятор не газовал
}

static esp_err_t cmd_shutdown(app_controller_t *self, const char *cmd, cJSON *payload) {
    publish_response(self, cmd, "ok", "shutdown");
    // даём времени сообщению уйти в сеть до выключения
    vTaskDelay(pdMS_TO_TICKS(REBOOT_FLUSH_DELAY_MS));
    modem_shutdown(self->modem);
    return ESP_OK; // не достигается, чтобы компилятор не газовал
}

static esp_err_t cmd_relay_on(app_controller_t *self, const char *cmd, cJSON *payload) {
    relay_set_state(self->relay, true);
    return publish_response(self, cmd, "ok", "relay turned on");
}

static esp_err_t cmd_relay_off(app_controller_t *self, const char *cmd, cJSON *payload) {
    relay_set_state(self->relay, false);
    return publish_response(self, cmd, "ok", "relay turned off");
}

static esp_err_t cmd_status(app_controller_t *self, const char *cmd, cJSON *payload) {
    bool state = relay_get_state(self->relay);
    int rssi = 0;

    if (modem_driver_get_signal_quality(self->modem, &rssi) != ESP_OK) {
        char desc[64];
        snprintf(desc, sizeof(desc), "relay:%s", state ? "on" : "off");
        return publish_response(self, cmd, "partial", desc);
    }

    char desc[128];
    snprintf(desc, sizeof(desc), "relay:%s,rssi:%d", state ? "on" : "off", rssi);
    return publish_response(self, cmd, "ok", desc);
}

// ---- Таблица команд ---------------------------------------------------------
// Добавить новую команду = добавить одну строку. Ядро менять не нужно.

static const struct {
    const char *name;
    command_fn_t fn;
} COMMANDS[] = {
    { "reboot",    cmd_reboot    },
    { "shutdown",  cmd_shutdown  },
    { "relay_on",  cmd_relay_on  },
    { "relay_off", cmd_relay_off },
    { "status",    cmd_status    },
};

// ---- Публичный API ----------------------------------------------------------

app_controller_t* controller_create(modem_handle_t modem, relay_handle_t relay) {
    if (modem == NULL || relay == NULL) {
        ESP_LOGE(TAG, "controller_create: отсутствует зависимость");
        return NULL;
    }

    app_controller_t *self = calloc(1, sizeof(app_controller_t));
    if (self == NULL) {
        return NULL;
    }
    self->modem = modem;
    self->relay = relay;
    self->mqtt = NULL;
    return self;
}

void controller_set_mqtt(app_controller_t *self, mqtt_handle_t mqtt) {
    if (self == NULL) {
        return;
    }
    self->mqtt = mqtt;
}

esp_err_t controller_handle_command(void *user, const char *cmd, cJSON *payload) {
    app_controller_t *self = (app_controller_t *) user;
    if (self == NULL || cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Обработка команды: %s", cmd);

    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (strcmp(cmd, COMMANDS[i].name) == 0) {
            return COMMANDS[i].fn(self, cmd, payload);
        }
    }

    return publish_response(self, cmd, "error", "unknown command");
}
// ---- Публичный API ----------------------------------------------------------
void controller_toggle_relay(app_controller_t *self) {
    if (self == NULL) {
        return;
    }
    bool new_state = !relay_get_state(self->relay);
    relay_set_state(self->relay, new_state);
    publish_response(self, "toggle", "ok", new_state ? "relay turned on" : "relay turned off");
}

void controller_toggle_command(app_controller_t *self) {
    if (self == NULL) {
        return;
    }
    bool new_state = !relay_get_state(self->relay);

    publish_command(self, new_state? "relay_on": "relay_off");
}

void controller_shutdown_command(app_controller_t *self) {
    if (self == NULL) {
        return;
    }
    publish_response(self, "cmd", "ok", "shutdown");
    // даём время сообщению уйти в сеть до выключения
    vTaskDelay(pdMS_TO_TICKS(REBOOT_FLUSH_DELAY_MS));
    vTaskDelay(pdMS_TO_TICKS(REBOOT_FLUSH_DELAY_MS));
    clear_response_topic(self);
    vTaskDelay(pdMS_TO_TICKS(REBOOT_FLUSH_DELAY_MS));
    modem_shutdown(self->modem);
}

void controller_publish_lowbattery(app_controller_t *self) {
    if (self == NULL) {
        return;
    }
    bool new_state = !relay_get_state(self->relay);
    relay_set_state(self->relay, new_state);
    publish_response(self, "toggle", "alarm", "low battery");
}

void controller_publish_battery_ok(app_controller_t *self) {
    if (self == NULL) {
        return;
    }
    relay_set_state(self->relay, true);
    publish_response(self, "toggle", "ok", "battery ok");
    vTaskDelay(pdMS_TO_TICKS(REBOOT_FLUSH_DELAY_MS));
    vTaskDelay(pdMS_TO_TICKS(REBOOT_FLUSH_DELAY_MS));
    clear_response_topic(self);
}


