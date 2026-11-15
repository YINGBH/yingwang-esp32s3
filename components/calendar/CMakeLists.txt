idf_component_register(
    SRCS "calendar.c" "lunar.c" "calendar_remote.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json nvs_flash mbedtls net_clock
)
