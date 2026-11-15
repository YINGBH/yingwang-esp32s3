/*
 * 环境声音检测
 *
 * 这块板子有一个纯 I2S 的麦克风（WS=4 SCK=5 DIN=6，没有 codec 芯片），
 * 直接读 PCM 算 RMS 就能得到环境音量，不需要任何额外器件。
 *
 * 读数含义：
 *   percent  0-100，把 -70dBFS ~ 0dBFS 线性映射过来，肉眼直观
 *   db       实际分贝（dBFS），越小越安静
 *
 * 注意 dBFS 是相对满量程的分贝，不是声压级（没有校准），
 * 只适合看"吵不吵"/"有没有声音"，不能当专业噪声计。
 */
#ifndef _AUDIO_LEVEL_H_
#define _AUDIO_LEVEL_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    int   percent;      /* 0-100 当前音量（已平滑） */
    int   peak_percent; /* 最近一段时间的峰值 */
    float db;           /* dBFS，-90 表示静音 */
    bool  ready;        /* 麦克风初始化成功 */
} audio_level_t;

/** 初始化麦克风并启动采集任务 */
esp_err_t audio_level_start(void);

/** 取当前读数（线程安全，内部加锁） */
void audio_level_get(audio_level_t *out);

/** 重置峰值 */
void audio_level_reset_peak(void);

/** 暂停/恢复麦克风（按需使用，省 CPU） */
void audio_level_pause(bool pause);

/** 只置标志（电平任务让出 I2S），不关通道；录音用 */
void audio_level_hold(bool hold);

/* ---------------- 录音（16kHz 单声道 int16） ---------------- */

/** 开始录音到调用方提供的缓冲；cap_samples 为最大采样数（1 秒 = 16000） */
esp_err_t audio_rec_start(int16_t *dst, int cap_samples);

/** 停止录音并返回实际录到的采样数；也用于「等录音自然结束」后的收尾 */
int audio_rec_stop(void);

/** 是否正在录音 */
bool audio_rec_active(void);

/** 录音时的实时电平 0-100 */
int audio_rec_level(void);

/** 当前录音增益 */
float audio_rec_gain(void);

/** 调试：抓 0.5 秒原始采样，统计 min/max/RMS */
esp_err_t audio_level_probe(int32_t *out_min, int32_t *out_max,
                            double *out_rms, int32_t *samples, int n_samples);

#endif  // _AUDIO_LEVEL_H_