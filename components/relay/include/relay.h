#ifndef RELAY_H
#define RELAY_H

#include "stdbool.h"

typedef struct relay_ctx_t* relay_handle_t;

/*
    relay_init - конструктор реле. Инициализирует на каком пине работает реле.
    Возвращает объект реле.
*/
relay_handle_t relay_init(int gpio_pin);

// Принимает объект реле и передаёт соостояние. true - вкл, false - выкл
void relay_set_state(relay_handle_t relay, bool state);
bool relay_get_state(relay_handle_t relay);

// Деструктор реле.
void relay_destroy(relay_handle_t relay);

#endif