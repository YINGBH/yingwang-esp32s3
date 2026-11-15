idf_component_register(
    SRCS "portal_client.c" "pwd_enc.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client pngle captcha nvs_flash
    PRIV_REQUIRES mbedtls esp_timer
)