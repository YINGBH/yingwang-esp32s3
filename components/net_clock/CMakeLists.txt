idf_component_register(
    SRCS "time_weather.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json mbedtls netmon nvs_flash
)
