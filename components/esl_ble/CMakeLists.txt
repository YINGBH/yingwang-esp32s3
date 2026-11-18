idf_component_register(
    SRCS "esl_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES bt esp_timer app_state calendar net_clock bsp_nologo esp_app_format tracker_scan
)
