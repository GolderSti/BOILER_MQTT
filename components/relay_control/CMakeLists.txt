idf_component_register(
    SRCS "relay_control.c" "relay_io.c"
    REQUIRES sun_time mqtt_common
    INCLUDE_DIRS .
    PRIV_REQUIRES driver nvs_flash
)
