idf_component_register(
    SRCS "src/espnow.c" "src/gateway_packets.c" "src/mqtt_publisher.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common freertos
    PRIV_REQUIRES esp_event esp_wifi espressif__mqtt esp_driver_gpio battery_monitor
)
