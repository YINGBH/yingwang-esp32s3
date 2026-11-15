/*
 * 倒计时/闹钟提示音
 *
 * 板载喇叭是 I2S（BCLK=15, LRCK=16, DOUT=7），没有 codec 芯片。
 * 只在响铃时临时把 I2S 通道拉起来，响完就关掉，不常驻占内存。
 * 音量 0~100 存在 NVS（命名空间 beep），USB 网页里可调。
 */
#ifndef _BEEP_H_
#define _BEEP_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 开始循环响铃（直到 beep_alarm_stop） */
void beep_alarm_start(void);

/** 停止响铃（没在响也无所谓） */
void beep_alarm_stop(void);

/** 是否有闹钟在响 */
bool beep_alarm_active(void);

/** 音量 0-100（0 静音），会存进 NVS */
/** 按键提示音（20ms 短音） */
void beep_tick(void);
void beep_set_tick_mute(bool mute);
bool beep_tick_muted(void);

/** 播放单声道 PCM（16kHz int16），阻塞到播完 */
void beep_play_pcm(const int16_t *pcm, int samples);

void beep_set_volume(int percent);

int beep_volume(void);

#ifdef __cplusplus
}
#endif

#endif  // _BEEP_H_
