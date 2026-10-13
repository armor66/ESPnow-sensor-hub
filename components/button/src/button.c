#include "button.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "espnow.h"

// Кнопка замыкает GPIO на землю, вход с подтяжкой к питанию => нажата = 0.
#define BUTTON_PRESSED_LEVEL 0

#define BUTTON_DEBOUNCE_MS 50   // окно, за которое дребезг успокаивается
#define BUTTON_POLL_MS     10   // период опроса при ожидании отпускания

static const char *TAG = "BUTTON";

struct button_ctx_t {
    int gpio_pin;
    button_press_cb_t cb;
    void *user;
    TaskHandle_t task;
};

// ISR делает абсолютный минимум: будит задачу. Никаких задержек, логов и
// пользовательских колбэков в прерывании быть не должно.
static void IRAM_ATTR button_isr_handler(void *arg) {
    struct button_ctx_t *ctx = (struct button_ctx_t *) arg;
    BaseType_t hp_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(ctx->task, &hp_task_woken);
    portYIELD_FROM_ISR(hp_task_woken);
}

static void button_task(void *arg) {
    struct button_ctx_t *ctx = (struct button_ctx_t *) arg;

    for (;;) {
        // Спим без нагрузки на CPU, пока ISR не разбудит первым фронтом.
        // pdTRUE = сбросить счётчик уведомлений в 0 при выходе: сколько бы
        // фронтов дребезга ISR ни прислал, мы выходим ровно один раз.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Фронт пришёл, но это может быть дребезг. Ждём, пока линия устаканится.
        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));

        // Подтверждаем, что кнопка реально нажата, а не словили короткий всплеск.
        if (gpio_get_level(ctx->gpio_pin) != BUTTON_PRESSED_LEVEL) {
            ulTaskNotifyTake(pdTRUE, 0);   // выкинуть мусор, накопленный за дребезг
            continue;
        }

        // Ровно одно нажатие — дёргаем колбэк.
        if (ctx->cb != NULL) {
            ctx->cb(ctx->user);
        }

        // Ждём отпускания: иначе пока кнопку держат, мы бы сработали повторно.
        // Это же проглатывает весь дребезг нажатия и удержания.
        while (gpio_get_level(ctx->gpio_pin) == BUTTON_PRESSED_LEVEL) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));  // дебаунс отпускания

        // Сбросить уведомления, накопившиеся за время дребезга отпускания.
        ulTaskNotifyTake(pdTRUE, 0);
    }
}

button_handle_t button_init(int gpio_pin, button_press_cb_t cb, void *user) {
    struct button_ctx_t *ctx = calloc(1, sizeof(struct button_ctx_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Не удалось выделить память под кнопку");
        return NULL;
    }

    ctx->gpio_pin = gpio_pin;
    ctx->cb = cb;
    ctx->user = user;

    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << gpio_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,   // фронт нажатия (высокий -> низкий)
    };
    gpio_config(&io_conf);

    // Задачу создаём ДО навешивания ISR: к моменту первого прерывания
    // ctx->task уже должен быть валиден.
    if (xTaskCreate(button_task, "button_task", 4096, ctx, 6, &ctx->task) != pdPASS) {
        ESP_LOGE(TAG, "Не удалось создать задачу кнопки");
        free(ctx);
        return NULL;
    }

    // Сервис прерываний GPIO может быть уже установлен другим компонентом.
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Не удалось установить ISR service: %s", esp_err_to_name(err));
        vTaskDelete(ctx->task);
        free(ctx);
        return NULL;
    }

    gpio_isr_handler_add(gpio_pin, button_isr_handler, ctx);

    ESP_LOGI(TAG, "Кнопка инициализирована на GPIO %d", gpio_pin);
    return ctx;
}

void button_destroy(button_handle_t button) {
    if (button == NULL) {
        return;
    }
    gpio_isr_handler_remove(button->gpio_pin);
    if (button->task != NULL) {
        vTaskDelete(button->task);
    }
    free(button);
}
