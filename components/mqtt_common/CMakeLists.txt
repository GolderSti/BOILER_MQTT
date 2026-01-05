idf_component_register(SRCS "mqtt_common.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_wifi wifi_common mqtt
                    )
