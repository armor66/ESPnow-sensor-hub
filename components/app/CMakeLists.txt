idf_component_register(
    SRCS "src/app.c" "src/mqtt.c" "src/controller.c"
    INCLUDE_DIRS "include"
    REQUIRES modem relay led button espnow espressif__cjson battery_monitor
    PRIV_REQUIRES driver nvs_flash esp_netif esp_event espressif__mqtt
)
