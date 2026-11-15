#ifndef CAPTCHA_LOADER_H
#define CAPTCHA_LOADER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 网络形状（推理代码用） */
#define CM_IN_H   25
#define CM_IN_W   62
#define CM_C1     16
#define CM_C2     32
#define CM_FC     64
#define CM_NCLS   9
#define CM_NDIGIT 4
#define CM_PH1    12
#define CM_PW1    31
#define CM_PH2    6
#define CM_PW2    15
#define CM_FLAT   2880

/* 从 storage 分区映射模型；只用一次，重复调用直接返回上次结果 */
esp_err_t captcha_model_load(void);

/* 模型信息（给界面显示用） */
typedef struct {
    bool     ready;      /* 模型加载成功 */
    uint32_t params;     /* 参数量（int16 个数） */
    uint32_t bytes;      /* 模型数据字节数（含头部） */
    uint32_t crc;        /* 头部+张量表的 CRC32，可当版本指纹 */
    uint8_t  tensors;    /* 张量个数 */
} captcha_model_info_t;

void captcha_model_get_info(captcha_model_info_t *out);

/** 模型字节数（0 = 没有/没加载） */
size_t captcha_model_size(void);
bool captcha_model_ready(void);

extern const int16_t *CM_W1, *CM_B1, *CM_W2, *CM_B2;
extern const int16_t *CM_W3, *CM_B3, *CM_W4, *CM_B4;
extern float CM_W1_S, CM_B1_S, CM_W2_S, CM_B2_S;
extern float CM_W3_S, CM_B3_S, CM_W4_S, CM_B4_S;

#endif
