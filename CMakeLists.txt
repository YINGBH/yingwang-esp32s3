cmake_minimum_required(VERSION 3.16)

# 目标芯片必须在这里写死。
# 不写的话，sdkconfig 一删，idf.py 会回落到默认的 esp32 ——
# 而这块板子的 UP/DOWN 键在 GPIO40/39，只有 S3 才有，编译会直接报错。
set(IDF_TARGET "esp32s3")

# 正式版版本号（显示在系统页、串口 GET、固件文件名里）
set(PROJECT_VER "2.12.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(yingwang)