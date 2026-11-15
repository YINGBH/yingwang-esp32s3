idf_component_register(
    SRCS "logring.c" "flog.c"
    INCLUDE_DIRS "include"
    REQUIRES log esp_common esp_partition
)
