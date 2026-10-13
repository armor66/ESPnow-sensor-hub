#include <string.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_mac.h"

#include "espnow.h"
#include "wifi_cred.h"
#include "gateway_packets.h"
#include "mqtt_publisher.h"
#include "battery_monitor.h"
#include "driver/gpio.h"

#define ESPNOW_CHANNEL 13
#define ESPNOW_RX_QUEUE_LENGTH 10
#define ESPNOW_RX_TASK_STACK_SIZE 4096
#define ESPNOW_RX_TASK_PRIORITY 5

static const char *TAG = "ESP32NOW_master";

static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

bool battery_low = true;
//do not shutdown before first measure
bool battery_critical = false;

typedef struct {
    uint8_t src_addr[ESP_NOW_ETH_ALEN];
    int len;
    uint8_t data[ESP_NOW_MAX_DATA_LEN];
} espnow_rx_item_t;

static QueueHandle_t espnow_rx_queue;

// Heartbeat callback variables
static espnow_heartbeat_cb_t s_heartbeat_cb = NULL;
static void *s_heartbeat_ctx = NULL;

void espnow_register_heartbeat_cb(espnow_heartbeat_cb_t cb, void *ctx) {
    s_heartbeat_cb = cb;
    s_heartbeat_ctx = ctx;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Connecting to Wi-Fi...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected from Wi-Fi. Reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void on_espnow_send(const esp_now_send_info_t *tx_info, esp_now_send_status_t status){
    // Check if send was successful.
    ESP_LOGI(TAG, "send to " MACSTR " status: %s",
        MAC2STR(tx_info->des_addr),
        status == ESP_NOW_SEND_SUCCESS ? "success" : "fail");
}
/*******************/
static void espnow_rx_task(void *arg)
{
    (void)arg;

    espnow_rx_item_t item;
    gateway_mqtt_message_t message;

    /* ---- periodic publish bookkeeping ---- */
    const TickType_t PERIOD = pdMS_TO_TICKS(60U * 1000U);  /* 1 min */
    TickType_t last_periodic = xTaskGetTickCount();

    while (true) {
        /* how long until the next 10-min deadline? */
        TickType_t elapsed  = xTaskGetTickCount() - last_periodic;
        TickType_t wait     = (elapsed >= PERIOD) ? 0 : (PERIOD - elapsed);

        if (xQueueReceive(espnow_rx_queue, &item, wait) == pdTRUE) {
            /* ---------- normal ESP-NOW path ---------- */
            ESP_LOGW(TAG, "ESP-NOW packet received from " MACSTR ", len=%d",
                     MAC2STR(item.src_addr), item.len);

            esp_err_t err = gateway_packets_build_mqtt_message(
                                "sht41", item.data, (size_t)item.len, &message);
            if (err == ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGW(TAG, "Unsupported ESP-NOW packet from " MACSTR ", len=%d",
                         MAC2STR(item.src_addr), item.len);
                continue;
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to build MQTT message: %s",
                         esp_err_to_name(err));
                continue;
            }

            err = mqtt_publisher_publish(message.topic, message.payload, MQTT_QOS);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Published ESP-NOW packet to %s", message.topic);
            } else {
                ESP_LOGW(TAG, "Failed to publish ESP-NOW packet to %s: %s",
                         message.topic, esp_err_to_name(err));
            }
        } else {
            /* ---------- 10-minute timeout → periodic publish ---------- */
            last_periodic = xTaskGetTickCount();

            // 1. Initialize the struct with your required values
            static sensor_packet_t host_packet = {
                .node_id = 99,
                .sequence = 0,      //battery_info.level,
                .temp_c = 22.2f,
                .rh = 33.3f,
                .battery_mv = 0,    //battery_info.voltage_mv,
                .status_flags = 39
            };

            // Add battery information to the message
            battery_info_t battery_info;
            if (battery_monitor_read(&battery_info) == ESP_OK) {
                host_packet.sequence = battery_info.level;
                host_packet.battery_mv = battery_info.voltage_mv;
                /* if usb(VBUS) plugged or not exclusively*/
                if ((battery_info.level == 0) && (gpio_get_level(GPIO_NUM_23))) {
                    battery_critical = true;
                    host_packet.status_flags = 11;
                } else {
                    battery_critical = false;
                    if (battery_info.level < 18) {
                        host_packet.status_flags = 15;
                        battery_low = true;
                    } else {
                        host_packet.status_flags = 7;
                        battery_low = false;
                    }
                }
            }
            
            // 2. Cast the struct address to a const uint8_t pointer
            // (This safely handles float/uint32 byte-packing and CPU endianness)
            const uint8_t *host_data = (const uint8_t *)&host_packet;

            gateway_mqtt_message_t message;

            // 3. Call the function, passing the packet_type string, the data pointer, 
            // and the size of the struct
            esp_err_t err = gateway_packets_build_mqtt_message(
                "sht41",      // const char *packet_type
                host_data,                 // const uint8_t *data
                sizeof(host_packet),       // size_t len
                &message                   // gateway_mqtt_message_t *message
            );
            if (err == ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGW(TAG, "Unsupported ESP-NOW packet from " MACSTR ", len=%d",
                         MAC2STR(item.src_addr), item.len);
                continue;
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to build MQTT message: %s",
                         esp_err_to_name(err));
                continue;
            }
    
            err = mqtt_publisher_publish(message.topic, message.payload, MQTT_QOS);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Published ESP-NOW packet to %s", message.topic);
            } else {
                ESP_LOGW(TAG, "Failed to publish ESP-NOW packet to %s: %s",
                         message.topic, esp_err_to_name(err));
            }

            ESP_LOGW(TAG, "Periodic heartbeat");// failed: %s",
            // Trigger the registered heartbeat callback
            if (s_heartbeat_cb) {
                s_heartbeat_cb(s_heartbeat_ctx);
            }
        }
    }
}
/*****************************/
static void on_espnow_recv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (recv_info == NULL || data == NULL) {
        ESP_LOGW(TAG, "Invalid ESP-NOW receive callback arguments");
        return;
    }

    if (len <= 0 || len > ESP_NOW_MAX_DATA_LEN) {
        ESP_LOGW(TAG, "Invalid ESP-NOW packet length from " MACSTR ": %d",
                 MAC2STR(recv_info->src_addr), len);
        return;
    }

    if (espnow_rx_queue == NULL) {
        ESP_LOGE(TAG, "ESP-NOW RX queue is not initialized");
        return;
    }

    espnow_rx_item_t item = {
        .len = len,
    };
    memcpy(item.src_addr, recv_info->src_addr, sizeof(item.src_addr));
    memcpy(item.data, data, (size_t)len);

    if (xQueueSend(espnow_rx_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "ESP-NOW RX queue overflow from " MACSTR ", len=%d",
                 MAC2STR(recv_info->src_addr), len);
    }
}

esp_err_t espnow_queue_init(void)
{
    espnow_rx_queue = xQueueCreate(ESPNOW_RX_QUEUE_LENGTH, sizeof(espnow_rx_item_t));
    if (espnow_rx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create ESP-NOW RX queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_created = xTaskCreate(espnow_rx_task, "espnow_rx_task",
                                          ESPNOW_RX_TASK_STACK_SIZE, NULL,
                                          ESPNOW_RX_TASK_PRIORITY, NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ESP-NOW RX task");
        vQueueDelete(espnow_rx_queue);
        espnow_rx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void wifi_init(void){
    // ESP_ERROR_CHECK(esp_netif_init());
    // ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_event_group = xEventGroupCreate();
    assert(wifi_event_group != NULL);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    uint8_t mac[ESP_NOW_ETH_ALEN];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    ESP_LOGI(TAG, "STA MAC: " MACSTR, MAC2STR(mac));

    uint8_t primary_channel;
    wifi_second_chan_t second_channel;
    ESP_ERROR_CHECK(esp_wifi_get_channel(&primary_channel, &second_channel));
    ESP_LOGI(TAG, "Current Wi-Fi channel: %d", primary_channel);

    ESP_LOGI(TAG, "Wi-Fi connected");
}

void espnow_init(void){
    ESP_ERROR_CHECK(esp_now_init());
    
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_espnow_send));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_espnow_recv));

    esp_now_peer_info_t peer = {0};

    memcpy(peer.peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
    peer.channel = 0; // Use the current Wi-Fi channel because this gateway is connected to the router.
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    
    ESP_LOGI(TAG, "ESP-NOW init");
}


