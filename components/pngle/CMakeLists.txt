# miniz.c 由 pngle.c 通过 miniz.h 包含进来，不能单独放 SRCS，否则重复定义符号
idf_component_register(SRCS "pngle.c" INCLUDE_DIRS "." REQUIRES heap)