#include "relay.h"

#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "RELAY_COMP";

struct relay_ctx_t {
    int gpio_pin;
    bool is_active;
};

relay_handle_t relay_init(int gpio_pin) {
    struct relay_ctx_t* ctx = calloc(1, sizeof(struct relay_ctx_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Не удалось выделить память под реле");
        return NULL;
    }

    ctx->gpio_pin = gpio_pin;
    ctx->is_active = false;

    gpio_reset_pin(gpio_pin);
    gpio_set_direction(gpio_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(ctx->gpio_pin, 0);

    ESP_LOGI(TAG, "Реле проинициализировано на GPIO %d", gpio_pin);

    return ctx;
}

void relay_set_state(relay_handle_t relay, bool state) {
    if (relay == NULL) {
        ESP_LOGE(TAG, "Реле не проинициализировано");
        return;
    }

    int level = state == true ? 1 : 0;
    gpio_set_level(relay->gpio_pin, level);
    relay->is_active = state;

    ESP_LOGD(TAG, "Relay state set %d on GPIO %d", 
        relay->is_active, relay->gpio_pin
    );
}

bool relay_get_state(relay_handle_t relay) {
    if (relay == NULL) {
        ESP_LOGE(TAG, "Реле не проинициализировано");
        return false;
    }
    return relay->is_active;
}

void relay_destroy(relay_handle_t relay) {
    if (relay == NULL) {
        return;
    }
    gpio_set_level(relay->gpio_pin, 0);
    free(relay);
}
