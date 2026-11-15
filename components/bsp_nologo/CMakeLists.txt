idf_component_register(
    SRCS "bsp_display.c" "bsp_power.c" "bsp_buttons.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_driver_spi esp_driver_gpio esp_driver_ledc
)