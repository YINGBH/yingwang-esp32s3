/*
 * 主机端交叉验证（不需要 ESP32 硬件）
 *
 * 用 PC 上的 gcc 编译，喂入 训练/dump_bin.py 导出的裸数据，
 * 结果应当和 Python 训练的模型完全一致。
 *
 * 编译：
 *   gcc host_main.c ../components/captcha/captcha_infer.c \
 *       -I../components/captcha -O2 -o host_test
 *
 * 运行：
 *   ./host_test test_samples.bin test_expected.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "captcha_infer.h"

#define SAMPLE_BYTES (CAPTCHA_IMG_H * CAPTCHA_IMG_W * 3)

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <samples.bin> <expected.txt>\n", argv[0]);
        return 2;
    }

    FILE *fb = fopen(argv[1], "rb");
    if (!fb) {
        fprintf(stderr, "打不开 %s\n", argv[1]);
        return 2;
    }
    FILE *fe = fopen(argv[2], "r");
    if (!fe) {
        fprintf(stderr, "打不开 %s\n", argv[2]);
        fclose(fb);
        return 2;
    }

    unsigned char buf[SAMPLE_BYTES];
    char got[CAPTCHA_CHARS + 1];
    char want[64];

    int total = 0;
    int correct = 0;
    int mismatch = 0;

    while (fgets(want, sizeof(want), fe)) {
        want[strcspn(want, "\r\n")] = '\0';
        if (want[0] == '\0') {
            continue;
        }
        if (fread(buf, 1, SAMPLE_BYTES, fb) != SAMPLE_BYTES) {
            fprintf(stderr, "样本数据不足，第 %d 条\n", total + 1);
            break;
        }

        if (captcha_recognize(buf, got) != 0) {
            fprintf(stderr, "识别失败，第 %d 条\n", total + 1);
            mismatch++;
            break;
        }

        total++;
        if (strcmp(got, want) == 0) {
            correct++;
        } else {
            mismatch++;
            if (mismatch <= 10) {
                printf("  第 %d 条: 识别=%s 正确=%s\n", total, got, want);
            }
        }
    }

    if (total > 0) {
        printf("\n共 %d 条，正确 %d 条，准确率 %.2f%%\n",
               total, correct, 100.0 * correct / total);
    }

    fclose(fb);
    fclose(fe);
    return (total > 0) ? 0 : 1;
}