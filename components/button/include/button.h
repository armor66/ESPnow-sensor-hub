#ifndef BUTTON_H
#define BUTTON_H

typedef struct button_ctx_t* button_handle_t;

// Колбэк нажатия. user — произвольный контекст, переданный в button_init.
typedef void (*button_press_cb_t)(void *user);

// Конструктор кнопки. Настраивает GPIO как вход с подтяжкой к питанию,
// вешает прерывание по спадающему фронту (кнопка замыкает на землю)
// и запускает задачу антидребезга. Возвращает NULL при ошибке.
button_handle_t button_init(int gpio_pin, button_press_cb_t cb, void *user);

void button_destroy(button_handle_t button);

#endif
