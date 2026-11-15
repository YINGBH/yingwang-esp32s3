idf_component_register(
    SRCS "captcha_infer.c" "captcha_loader.c"
    INCLUDE_DIRS "."
    REQUIRES esp_partition esp_timer
)

# 模型是 3.1MB 的 float32 常量数组，编译慢但只占 Flash
# 推理是纯计算热点：单独开 -O3（全局是 -Og 调试优化，慢很多）
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -funroll-loops -ffast-math -Wno-unused-variable -Wno-unused-but-set-variable)