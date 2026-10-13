#ifndef ESPNOW_H
#define ESPNOW_H

// Define the heartbeat callback type
typedef void (*espnow_heartbeat_cb_t)(void *ctx);

void wifi_init(void);
esp_err_t espnow_queue_init(void);
void espnow_init(void);

// Function to register the heartbeat callback
void espnow_register_heartbeat_cb(espnow_heartbeat_cb_t cb, void *ctx);

extern bool battery_low;
extern bool battery_critical;

#endif