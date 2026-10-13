#include "led.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "mqtt_publisher.h"
#include "espnow.h"

#define LED_BLINK_PERIOD_MS 200
#define LED_IDLE_POLL_MS    100

static const char *TAG = "LED";

struct led_ctx_t {
    int alrm_pin;
    int mqtt_pin;
    volatile led_state_t state;
    TaskHandle_t task;
};

static void led_task(void *arg) {
    struct led_ctx_t *ctx = (struct led_ctx_t *) arg;
    bool alarm_level = false;
    bool blink_level = false;

    while(1) {

        if (battery_critical) {
            gpio_set_level(ctx->alrm_pin, 1);               //(GPIO_NUM_5, 1);
            vTaskDelay(pdMS_TO_TICKS(LED_IDLE_POLL_MS));
        } else if (battery_low) {
            alarm_level = !alarm_level;
            gpio_set_level(ctx->alrm_pin, alarm_level);     //(GPIO_NUM_5, alarm_level);
            vTaskDelay(pdMS_TO_TICKS(LED_BLINK_PERIOD_MS));
        } else {
            gpio_set_level(ctx->alrm_pin, 0);               //(GPIO_NUM_5, 0);
            vTaskDelay(pdMS_TO_TICKS(LED_IDLE_POLL_MS));
        }
        
        if (mqtt_publisher_is_connected()) {
            gpio_set_level(ctx->mqtt_pin, 1);               //(GPIO_NUM_18, 1);
            vTaskDelay(pdMS_TO_TICKS(LED_IDLE_POLL_MS));
        } else {
            blink_level = !blink_level;
            gpio_set_level(ctx->mqtt_pin, blink_level);     //(GPIO_NUM_18, blink_level);
            vTaskDelay(pdMS_TO_TICKS(LED_BLINK_PERIOD_MS));
        }
    
        // switch (ctx->state) {
        //     case LED_STATE_BLINK:
        //         blink_level = !blink_level;
        //         gpio_set_level(ctx->gpio_pin, blink_level);
        //         vTaskDelay(pdMS_TO_TICKS(LED_BLINK_PERIOD_MS));
        //         break;
        //     case LED_STATE_ON:
        //         gpio_set_level(ctx->gpio_pin, 1);
        //         vTaskDelay(pdMS_TO_TICKS(LED_IDLE_POLL_MS));
        //         break;
        //     case LED_STATE_OFF:
        //     default:
        //         gpio_set_level(ctx->gpio_pin, 0);
        //         vTaskDelay(pdMS_TO_TICKS(LED_IDLE_POLL_MS));
        //         break;
        // }
    }
}

led_handle_t led_init(int alrm_pin, int mqtt_pin) {
    struct led_ctx_t *ctx = calloc(1, sizeof(struct led_ctx_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Не удалось выделить память под индикатор");
        return NULL;
    }

    ctx->alrm_pin = alrm_pin;
    ctx->mqtt_pin = mqtt_pin;
    ctx->state = LED_STATE_OFF;

    gpio_reset_pin(alrm_pin);
    gpio_reset_pin(mqtt_pin);
    gpio_set_direction(alrm_pin, GPIO_MODE_OUTPUT);
    gpio_set_direction(mqtt_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(alrm_pin, 0);
    gpio_set_level(mqtt_pin, 0);

    if (xTaskCreate(led_task, "led_task", 2048, ctx, 5, &ctx->task) != pdPASS) {
        ESP_LOGE(TAG, "Не удалось создать задачу индикатора");
        free(ctx);
        return NULL;
    }

    ESP_LOGW(TAG, "Battery state on GPIO %d, MQTT state on GPIO %d", alrm_pin, mqtt_pin);
    return ctx;
}

void led_set_state(led_handle_t led, led_state_t state) {
    if (led == NULL) {
        return;
    }
    led->state = state;
}

void led_destroy(led_handle_t led) {
    if (led == NULL) {
        return;
    }
    if (led->task != NULL) {
        vTaskDelete(led->task);
    }
//    gpio_set_level(led->gpio_pin, 0);
    gpio_set_level(led->alrm_pin, 0);
    gpio_set_level(led->mqtt_pin, 0);
    free(led);
}
