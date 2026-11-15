/*
 * 验证码本地识别（ESP32-S3）
 *
 * 模型：训练/train.py 导出，权重在 captcha_model.h（float32，存 Flash）
 * 结构：conv16 -> pool -> conv32 -> pool -> fc64 -> 4 头 x 9 类
 * 精度：见 训练/model/meta.json
 */
#ifndef CAPTCHA_INFER_H
#define CAPTCHA_INFER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAPTCHA_IMG_W 62
#define CAPTCHA_IMG_H 25
#define CAPTCHA_CHARS 4

/**
 * 识别验证码。
 *
 * @param rgb   CAPTCHA_IMG_H x CAPTCHA_IMG_W x 3 的 RGB 数据（行优先）。
 *              直接从 PNG 解码后的像素即可，无需预处理。
 * @param out  输出缓冲区，至少 5 字节，写入 4 位数字 + '\0'
 * @return     0 成功，-1 参数错误
 */
int captcha_recognize(const uint8_t *rgb, char out[CAPTCHA_CHARS + 1]);

/** 同上，另外输出每一位的置信度（softmax 概率，0~1；可为 NULL） */
int captcha_recognize_ex(const uint8_t *rgb, char out[CAPTCHA_CHARS + 1], float conf[CAPTCHA_CHARS]);

#ifdef __cplusplus
}
#endif

#endif /* CAPTCHA_INFER_H */