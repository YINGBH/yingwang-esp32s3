idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES hitokoto app_update net_wifi portal nvs_flash spi_flash bsp_nologo audio_in net_clock logring
             app_state config_store stats beep usb_console screen_bg calendar esl_ble captcha
)
