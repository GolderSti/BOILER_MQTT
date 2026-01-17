idf_component_register(
    SRCS "sntp_common.c"
    INCLUDE_DIRS .
    PRIV_REQUIRES esp_netif wifi_common
)