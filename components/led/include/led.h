#ifndef LED_H
#define LED_H

typedef struct led_ctx_t* led_handle_t;

// Состояния индикатора.
typedef enum {
    LED_STATE_OFF = 0,  // погашен
    LED_STATE_ON,       // горит постоянно
    LED_STATE_BLINK,    // мигает (например, во время инициализации)
} led_state_t;

// Конструктор индикатора. Настраивает GPIO и запускает фоновую задачу.
// Возвращает NULL при ошибке.
//led_handle_t led_init(int gpio_pin);
led_handle_t led_init(int alrm_pin, int mqtt_pin);

// Задать состояние индикатора. Потокобезопасно (атомарная запись состояния).
void led_set_state(led_handle_t led, led_state_t state);

// Деструктор: останавливает задачу и освобождает ресурсы.
void led_destroy(led_handle_t led);

#endif
