idf_component_register(
    SRCS "usb_console.c"
    INCLUDE_DIRS "include"
    REQUIRES hitokoto stats esp_driver_usb_serial_jtag esp_app_format json beep portal captcha esl_ble screen_bg app_state config_store net_wifi bsp_nologo logring audio_in
)
