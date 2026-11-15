/*
 * 板上日志环（logring）
 *
 * 为什么要有它：
 *   现在看日志必须一直开着串口，而开串口会复位板子、USB 活动还会干扰 2.4G。
 *   把日志留在板子里，事后一次性 dump 出来，就能既看得到、又不影响运行。
 *
 * 做法：
 *   · 钩住 esp_log_set_vprintf，把每条日志同时写进 PSRAM 里的环形缓冲
 *   · 环形缓冲按字节存，"读"的时候按 '\n' 切行
 *   · 不碰 flash：不擦不写，零干扰，崩溃前也能保留（只到重启为止）
 *
 * 用法（串口/网页）：
 *   LOG         dump 最近 100 行
 *   LOG 500     dump 最近 500 行
 *   LOGC        清空
 */
#ifndef _LOGRING_H_
#define _LOGRING_H_

#include <stddef.h>

/** 启动日志环并挂上日志钩子（越早调用越好，放在 app_main 开头） */
void logring_start(void);

/** 当前缓冲里有多少字节 / 丢过多少字节（缓冲写满时覆盖最旧的） */
size_t logring_used(void);
unsigned logring_dropped(void);

/**
 * 取最后 n 行日志。
 * dst 由调用方提供；返回写入的字节数（以 '\0' 结尾）。
 * 行数按 '\n' 统计，n<=0 时默认 100。
 */
size_t logring_tail(char *dst, size_t dst_size, int n);

/** 清空 */
void logring_clear(void);

#endif  // _LOGRING_H_
