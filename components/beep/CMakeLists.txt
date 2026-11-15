idf_component_register(
    SRCS "beep.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2s nvs_flash
)
