idf_component_register(
    SRCS "netmon.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip esp_netif esp_timer
)