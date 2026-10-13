#include "mqtt.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MQTT_APP";

typedef struct mqtt_ctx_t {
    const char *uri;
    const char *client_id;
    const char *username;
    const char *password;
    const char *cmd_topic;
    const char *resp_topic;
    esp_mqtt_client_handle_t client;
    mqtt_command_cb_t cmd_cb;
    void *user;
} mqtt_ctx_t;

// Разбирает входящий payload и вызывает зарегистрированный колбэк команды.
// Контекст берётся из ctx (handler_args), без глобальных переменных.
static void handle_incoming_data(mqtt_ctx_t *ctx, const char *data, int data_len) {
    if (ctx->cmd_cb == NULL) {
        ESP_LOGW(TAG, "Команда получена, но колбэк не зарегистрирован");
        return;
    }

    // данные приходят без завершающего '\0'
    char *raw_json = malloc(data_len + 1);
    if (raw_json == NULL) {
        ESP_LOGE(TAG, "Нет памяти под входящее сообщение");
        return;
    }
    memcpy(raw_json, data, data_len);
    raw_json[data_len] = '\0';

    cJSON *root = cJSON_Parse(raw_json);
    free(raw_json);
    if (root == NULL) {
        ESP_LOGE(TAG, "Не удалось распарсить JSON!");
        return;
    }

    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd) || cmd->valuestring == NULL) {
        ESP_LOGE(TAG, "В JSON нет строкового поля \"cmd\"");
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "Получена команда: %s", cmd->valuestring);
    ctx->cmd_cb(ctx->user, cmd->valuestring, root);

    cJSON_Delete(root);
}

static void mqtt_event_handler(
    void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data
) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    mqtt_ctx_t *ctx = handler_args;
                                                                                        //event enum
    switch ((esp_mqtt_event_id_t) event_id) {
        case MQTT_EVENT_CONNECTED:                                                          // 1
            ESP_LOGW(TAG, "Успешно подключено к MQTT брокеру!");
            if (ctx && ctx->cmd_topic) {
                esp_mqtt_client_subscribe(client, ctx->cmd_topic, 1);
            }
            break;
        case MQTT_EVENT_DISCONNECTED:                                                       // 2
            ESP_LOGW(TAG, "Отключено от MQTT брокера. Автоматическое переподключение...");
            break;
        case MQTT_EVENT_SUBSCRIBED:                                                         // 3
            ESP_LOGW(TAG, "Успешно подписались на топик! msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:                                                               // 6
            ESP_LOGW(TAG, "Получены данные! topic=%.*s, data=%.*s",
                event->topic_len, event->topic,
                event->data_len, event->data
            );
            handle_incoming_data(ctx, event->data, event->data_len);
            break;
        case MQTT_EVENT_ERROR:                                                              // 0
            ESP_LOGE(TAG, "Произошла ошибка MQTT!");
            break;
        case MQTT_EVENT_PUBLISHED:                                                          // 5
            ESP_LOGW(TAG, "Сообщение опубликовано!");
            break;
        default:
            ESP_LOGW(TAG, "Необработанное событие EVENT=%d", event->event_id);  // 8 deleted from outbox by expiry timeout
            if (ctx && ctx->resp_topic) {                                       // 4 acknowledged an unsubscribe request
                // Publishing NULL with length 0 and retain=1 clears the retained message on the broker
                esp_mqtt_client_publish(client, ctx->resp_topic, NULL, 0, 0, 1);
            }
            break;
    }
}

esp_err_t mqtt_clear_retained(mqtt_handle_t handle, const char *topic) {
    if (handle == NULL || handle->client == NULL || topic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    // NULL data, len = 0, qos = 0, retain = 1
    int msg_id = esp_mqtt_client_publish(handle->client, topic, NULL, 0, 0, 1);
    if (msg_id != -1) {
        ESP_LOGI(TAG, "Topic %s (msg_id=%d) cleared", topic, msg_id);
        return ESP_OK;
    }
    return ESP_FAIL;
}

mqtt_handle_t mqtt_app_start(const mqtt_config_t *config, mqtt_command_cb_t cb, void *user) {
    if (config == NULL) {
        ESP_LOGE(TAG, "Пустой конфиг!");
        return NULL;
    }

    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = config->uri,
        .credentials.client_id = config->client_id,
        .credentials.username = config->username,
        .credentials.authentication.password = config->password,
        // .network.timeout_ms = config->timeout_ms,
        // .network.reconnect_timeout_ms = config->reconnect_timeout_ms,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Не удалось инициализировать MQTT клиент!");
        return NULL;
    }

    mqtt_ctx_t *ctx = calloc(1, sizeof(mqtt_ctx_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Не удалось выделить память под MQTT объект");
        esp_mqtt_client_destroy(client);
        return NULL;
    }
    ctx->client = client;
    ctx->uri = config->uri;     //config->client_id; config->username; config->password;
    ctx->client_id = config->client_id;
    ctx->username = config->username;
    ctx->password = config->password;
    ctx->cmd_topic = config->cmd_topic;
    ctx->resp_topic = config->resp_topic;
    ctx->cmd_cb = cb;
    ctx->user = user;

    // контекст передаётся в обработчик через handler_args — без глобальных переменных
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        client, ESP_EVENT_ANY_ID, mqtt_event_handler, ctx
    ));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));

    return ctx;
}

esp_err_t mqtt_app_publish(mqtt_handle_t handle, const char *topic, const char *data) {
    if (handle == NULL || handle->client == NULL) {
        ESP_LOGE(TAG, "MQTT клиент не инициализирован!");
        return ESP_ERR_INVALID_ARG;
    }

    int msg_id = esp_mqtt_client_publish(handle->client, topic, data, 0, 1, 1);
    if (msg_id != -1) {
        ESP_LOGI(TAG, "Сообщение отправлено в топик %s (msg_id=%d)", topic, msg_id);
    } else {
        ESP_LOGE(TAG, "Ошибка отправки сообщения (нет соединения)");
        return ESP_FAIL;
    }
    return ESP_OK;
}

const char* mqtt_get_resp_topic(mqtt_handle_t handle) {
    if (handle == NULL) return NULL;
    return handle->resp_topic;
}
const char* mqtt_get_cmd_topic(mqtt_handle_t handle) {
    if (handle == NULL) return NULL;
    return handle->cmd_topic;
}

char* form_json_response(const char *cmd, const char *result, const char *desc) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    if (cmd == NULL) {
        cJSON_AddNullToObject(root, "cmd");
    } else {
        cJSON_AddStringToObject(root, "cmd", cmd);
    }

    if (result == NULL) {
        cJSON_AddNullToObject(root, "result");
    } else {
        cJSON_AddStringToObject(root, "result", result);
    }

    if (desc == NULL) {
        cJSON_AddNullToObject(root, "desc");
    } else {
        cJSON_AddStringToObject(root, "desc", desc);
    }

    char *json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_string;
}

char* form_json_cmd(const char *cmd) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    if (cmd == NULL) {
        cJSON_AddNullToObject(root, "cmd");
    } else {
        cJSON_AddStringToObject(root, "cmd", cmd);
    }

    char *json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_string;
}
