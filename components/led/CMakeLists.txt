idf_component_register(SRCS "src/led.c"
                      INCLUDE_DIRS "include"
                      REQUIRES espnow
                      PRIV_REQUIRES esp_driver_gpio)
