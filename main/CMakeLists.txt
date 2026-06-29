idf_component_register(SRCS "onenet_mqtt.c" "wifi_sta.c" "audio_tone.c" "max98357.c" "main.c" "max30102.c" "algorithm.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi nvs_flash esp_event esp_netif mqtt
                             esp_driver_i2c esp_driver_i2s esp_driver_gpio)
