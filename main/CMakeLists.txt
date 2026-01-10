idf_component_register(SRCS "ota.c" "roaming_app_example.c"
                    PRIV_REQUIRES esp_wifi nvs_flash mbedtls
                    INCLUDE_DIRS "."
                    REQUIRES wifi_common mqtt_common hlk_ld2410c app_update esp_https_ota version)
