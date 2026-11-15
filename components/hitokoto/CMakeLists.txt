idf_component_register(
    SRCS "hitokoto.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client mbedtls esp-tls
)