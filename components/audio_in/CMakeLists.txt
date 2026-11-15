idf_component_register(
    SRCS "audio_level.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2s bsp_nologo
    PRIV_REQUIRES esp_timer
)