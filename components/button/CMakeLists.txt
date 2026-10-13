idf_component_register(SRCS "src/button.c"
                      INCLUDE_DIRS "include"
                      REQUIRES espnow
                      PRIV_REQUIRES esp_driver_gpio esp_common)
