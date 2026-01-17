idf_component_register(
    SRCS "cordic_helper.c" "sun_time_common.c" "time_main.c" "cordic.c" "sun_time.c"
    REQUIRES sntp_common
    INCLUDE_DIRS .
    PRIV_REQUIRES esp_timer
)
