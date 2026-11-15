/*
 * 验证码本地识别（ESP32-S3）
 *
 * 与 训练/reference_infer.py 一一对应：
 *   预处理  : v = max(0, (R - max(G,B)) / 255)
 *   conv1   : 16@3x3 pad1 + ReLU + maxpool2   -> 16x12x31
 *   conv2   : 32@3x3 pad1 + ReLU + maxpool2   -> 32x6x15
 *   fc      : 2880 -> 64, ReLU
 *   heads   : 4 个独立 softmax，各 9 类（字符集 0-8）
 *
 * 卷积与池化是融合的：直接对每个池化窗口的 4 个位置算卷积再取 max，
 * 这样中间缓冲只有池化后的尺寸，省下约 4 倍内存。
 */

#include "esp_attr.h"
#include "captcha_infer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "captcha_loader.h"

#include <stddef.h>

/* 静态缓冲：总计约 42KB，放 BSS 即可 */
EXT_RAM_BSS_ATTR static float s_in[CM_IN_H * CM_IN_W];          /* 25*62  = 1550  */
EXT_RAM_BSS_ATTR static float s_f1[CM_C1 * CM_PH1 * CM_PW1];    /* 16*12*31 = 5952 */
EXT_RAM_BSS_ATTR static float s_f2[CM_C2 * CM_PH2 * CM_PW2];    /* 32*6*15  = 2880 */
EXT_RAM_BSS_ATTR static float s_a3[CM_FC];                      /* 64             */

/**
 * 融合 conv3x3(pad=1) + ReLU + maxpool2
 *
 * @param in   布局 [c][y][x]，尺寸 Cin x Hin x Win
 * @param w    布局 [o][c][ky][kx]，尺寸 Cout x Cin x 3 x 3
 * @param out  布局 [o][y][x]，尺寸 Cout x Ho x Wo，Ho=Hin/2, Wo=Win/2
 */
/* 把 int16 权重 × scale 预先转成 float，缓存在 PSRAM（每层只做一次） */
/* 权重预缩放缓存（一次只缓存一层，层间会重建） */
static const char *TAG = "captcha";
/* FC 权重（368KB）缓存到 PSRAM，避免每次推理都从 flash 顺序读 */
static int16_t *s_w3_psram = NULL;

static float *s_wcache = NULL;
static int s_wcache_n = -1;

static float *scale_weights(const int16_t *w16, int n, float ws, int *cached_n)
{
    if (*cached_n == n) {
        return s_wcache;                    /* 之前算过同样长度，直接复用 */
    }
    if (s_wcache != NULL) {
        free(s_wcache);
        s_wcache = NULL;
    }
    /* 权重是内层循环最热的数组，必须放内部 RAM（PSRAM 逐次访问慢一倍以上） */
    float *buf = heap_caps_malloc((size_t) n * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = heap_caps_malloc((size_t) n * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        *cached_n = -1;                     /* 标记：无法预缩放，走老路 */
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        buf[i] = (float) w16[i] * ws;
    }
    s_wcache = buf;
    *cached_n = n;
    return buf;
}

/* 融合 conv3x3(pad=1) + ReLU + maxpool2
   布局：in [c][y][x] / w [o][c][ky][kx] / out [o][y][x] */
static void conv_pool(const float *in, int Hin, int Win, int Cin,
                      const float *w, const float *b,
                      int Cout, float *out, int Ho, int Wo)
{
    for (int o = 0; o < Cout; o++) {
        const float bias = b ? b[o] : 0.0f;
        const float *wo = w + (size_t) o * Cin * 9;

        for (int y = 0; y < Ho; y++) {
            for (int x = 0; x < Wo; x++) {
                float best = -1e30f;

                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        const int yy = 2 * y + dy;
                        const int xx = 2 * x + dx;
                        float acc = bias;

                        /* 竖直方向的有效范围，只算一次 */
                        int ky0 = (yy == 0) ? 1 : 0;
                        int ky1 = (yy == Hin - 1) ? 1 : 2;
                        /* 水平方向同理 */
                        int kx0 = (xx == 0) ? 1 : 0;
                        int kx1 = (xx == Win - 1) ? 1 : 2;

                        for (int c = 0; c < Cin; c++) {
                            const float *cp = in + (size_t) c * Hin * Win;
                            const float *wp = wo + (size_t) c * 9;

                            for (int ky = ky0; ky <= ky1; ky++) {
                                const float *row = cp + (yy + ky - 1) * Win;
                                const float *wrow = wp + ky * 3;
                                for (int kx = kx0; kx <= kx1; kx++) {
                                    acc += wrow[kx] * row[xx + kx - 1];
                                }
                            }
                        }
                        if (acc > best) {
                            best = acc;
                        }
                    }
                }
                out[(o * Ho + y) * Wo + x] = (best > 0.0f) ? best : 0.0f;
            }
        }
    }
}

int captcha_recognize_ex(const uint8_t *rgb, char out[CAPTCHA_CHARS + 1],
                  float conf[CAPTCHA_CHARS])
{
    if (rgb == NULL || out == NULL) {
        return -1;
    }
    int64_t t0 = esp_timer_get_time();
    /* 模型放在 storage 分区里，第一次用的时候映射进来 */
    if (!captcha_model_ready() && captcha_model_load() != ESP_OK) {
        return -2;
    }

    /* ---------- 预处理：红色富余（白底为 0，红字为 1） ---------- */
    for (int y = 0; y < CM_IN_H; y++) {
        for (int x = 0; x < CM_IN_W; x++) {
            const uint8_t *p = rgb + ((size_t)y * CM_IN_W + x) * 3;
            const int r = p[0];
            const int g = p[1];
            const int b = p[2];
            const int mx = (g > b) ? g : b;
            int v = r - mx;
            if (v < 0) {
                v = 0;
            }
            s_in[y * CM_IN_W + x] = (float)v / 255.0f;
        }
    }

    int64_t t_pre = esp_timer_get_time();
    /* ---------- 两层卷积 ---------- */
    /* 每层权重先乘上 scale 转成 float（只做一次，之后走缓存） */
    int n1 = CM_C1 * 1 * 9;
    float *w1f = scale_weights(CM_W1, n1, CM_W1_S, &s_wcache_n);
    if (w1f != NULL) {
        float b1f[CM_C1];
        for (int i = 0; i < CM_C1; i++) {
            b1f[i] = (float) CM_B1[i] * CM_B1_S;
        }
        conv_pool(s_in, CM_IN_H, CM_IN_W, 1, w1f, b1f, CM_C1,
                  s_f1, CM_PH1, CM_PW1);
    }

    int n2 = CM_C2 * CM_C1 * 9;
    /* 注意：缓存只能存一层，这里重新算 */
    s_wcache_n = -1;
    float *w2f = scale_weights(CM_W2, n2, CM_W2_S, &s_wcache_n);
    if (w2f != NULL) {
        float *b2f = heap_caps_malloc(sizeof(float) * CM_C2, MALLOC_CAP_8BIT);
        if (b2f == NULL) {
            b2f = malloc(sizeof(float) * CM_C2);
        }
        if (b2f != NULL) {
            for (int i = 0; i < CM_C2; i++) {
                b2f[i] = (float) CM_B2[i] * CM_B2_S;
            }
            conv_pool(s_f1, CM_PH1, CM_PW1, CM_C1, w2f, b2f, CM_C2,
                      s_f2, CM_PH2, CM_PW2);
            free(b2f);
        }
    }

    int64_t t_conv = esp_timer_get_time();
    /* ---------- 全连接 + ReLU ---------- */
    for (int f = 0; f < CM_FC; f++) {
        s_a3[f] = (float) CM_B3[f] * CM_B3_S;
    }
    for (int i = 0; i < CM_FLAT; i++) {
        const float v = s_f2[i] * CM_W3_S;
        const int16_t *row = CM_W3 + (size_t)i * CM_FC;
        for (int f = 0; f < CM_FC; f++) {
            s_a3[f] += v * (float) row[f];
        }
    }
    for (int f = 0; f < CM_FC; f++) {
        if (s_a3[f] < 0.0f) {
            s_a3[f] = 0.0f;
        }
    }

    int64_t t_fc = esp_timer_get_time();
    /* ---------- 4 个输出头 ---------- */
    for (int k = 0; k < CM_NDIGIT; k++) {
        int best_c = 0;
        float best_v = -1e30f;
        float logits[CM_NCLS];

        for (int cls = 0; cls < CM_NCLS; cls++) {
            float acc = (float) CM_B4[k * CM_NCLS + cls] * CM_B4_S;
            const int16_t *base = CM_W4 + ((size_t)k * CM_FC) * CM_NCLS;

            for (int f = 0; f < CM_FC; f++) {
                acc += (float) base[(size_t)f * CM_NCLS + cls]
                       * CM_W4_S * s_a3[f];
            }
            logits[cls] = acc;
            if (acc > best_v) {
                best_v = acc;
                best_c = cls;
            }
        }
        out[k] = (char)('0' + best_c);

        if (conf != NULL) {
            /* softmax 出这一位的最大概率，当作置信度 */
            float sum = 0.0f;
            for (int cls = 0; cls < CM_NCLS; cls++) {
                sum += expf(logits[cls] - best_v);
            }
            conf[k] = (sum > 0.0f) ? (1.0f / sum) : 0.0f;
        }
    }
    out[CAPTCHA_CHARS] = '\0';
    int64_t t_all = esp_timer_get_time();
    ESP_LOGI(TAG, "阶段耗时: 预处理 %lld / 卷积 %lld / 全连接 %lld / 输出头 %lld ms",
             (long long) ((t_pre - t0) / 1000),
             (long long) ((t_conv - t_pre) / 1000),
             (long long) ((t_fc - t_conv) / 1000),
             (long long) ((t_all - t_fc) / 1000));

    return 0;
}

int captcha_recognize(const uint8_t *rgb, char out[CAPTCHA_CHARS + 1])
{
    return captcha_recognize_ex(rgb, out, NULL);
}