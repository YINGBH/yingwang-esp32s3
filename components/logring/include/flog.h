/*
 * 板上日志 —— flash 持久化部分
 *
 * 目的：RAM 环形缓冲（logring.c）在复位/重启后就没了，
 *       而排查问题往往需要"重启之前"的日志。这里把日志同时写进
 *       storage 分区的空闲区，重启后还能用 LOGF 拉出来。
 *
 * 位置（storage 分区内偏移，和模型/背景图/录音区不冲突）：
 *   0x000000  验证码模型
 *   0x060000  背景图（到 0x080000）
 *   0x080000  录音区（到 0x380000）
 *   0x380000  ── 本模块：日志区 380KB（95 页 × 4KB）──
 *
 * 页格式（每页 4KB，记录是 [u16 len][bytes]）：
 *   +0  u32 magic  = 'FLOG'
 *   +4  u32 seq    页序号（递增，用于排序）
 *   +8  u32 crc    保留（0）
 *   +12 u32 保留
 *   +16 起：一条条记录 [u16 len][len 字节文本]
 *           读到 len==0 或 0xFFFF 就是本页结束
 *
 * 写入策略：先在 RAM 里攒 2~3KB，再一次性写一页内的连续区域；
 *           一页写满就换下一页（换页时擦除该页）。
 *           这样擦除频率约为"每写满 4KB 日志一次"，对 WiFi 影响很小。
 */
#ifndef _FLOG_H_
#define _FLOG_H_

#include <stddef.h>
#include <stdint.h>

/** 初始化（找 storage 分区、启动 flush 任务）。失败也不影响其它功能 */
void flog_start(void);

/** 追加一段日志文本（内部只做内存拷贝，真正写 flash 由任务做） */
void flog_append(const char *text, size_t len);

/** 读最近 n 行（n<=0 默认 300）。返回写入字节数 */
size_t flog_tail(char *dst, size_t dst_size, int n);

/** 清空日志区（擦除所有页） */
int flog_clear(void);

#endif  // _FLOG_H_
