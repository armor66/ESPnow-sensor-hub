#ifndef MQTT_H
#define MQTT_H

#include "esp_err.h"
#include "cJSON.h"

typedef struct mqtt_ctx_t* mqtt_handle_t;

// Конфигурация MQTT. Компонент не знает об app_config_t — слабая связанность.
typedef struct mqtt_config_t {
    const char *uri;
    const char *client_id;
    const char *username;
    const char *password;
    // const int   timeout_ms;
    // const int   reconnect_timeout_ms;
    const char *cmd_topic;
    const char *resp_topic;
} mqtt_config_t;

// Колбэк команды. user — контекст, переданный в mqtt_app_start (обычно контроллер).
// payload — распарсенный JSON (валиден только на время вызова).
typedef esp_err_t (*mqtt_command_cb_t)(void *user, const char *cmd, cJSON *payload);

// Запуск MQTT-клиента. Колбэк и контекст хранятся в самом объекте mqtt,
// глобальные переменные не используются. Возвращает NULL при ошибке.
mqtt_handle_t mqtt_app_start(const mqtt_config_t *config, mqtt_command_cb_t cb, void *user);

esp_err_t mqtt_app_publish(mqtt_handle_t handle, const char *topic, const char *data);

// Геттеры топиков. Возвращают NULL, если топик неизвестен.
const char* mqtt_get_resp_topic(mqtt_handle_t handle);

const char* mqtt_get_cmd_topic(mqtt_handle_t handle);

// Хелпер формирования JSON-ответа; результат освобождать через free().
char* form_json_response(const char *cmd, const char *result, const char *desc);

char* form_json_cmd(const char *cmd);

// Clears a retained message on a specific topic by publishing NULL with retain=1
esp_err_t mqtt_clear_retained(mqtt_handle_t handle, const char *topic);

#endif
