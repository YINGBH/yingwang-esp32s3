/*
 * 网络质量监控
 *
 * 每 5 秒对网关做一次 TCP 连接测延迟（不产生 HTTP 流量，也不会被打断），
 * 用连接耗时反映当前网络状况：
 *   - 正常      几十毫秒
 *   - 拥塞/丢包 几百毫秒，或者直接超时
 *
 * 同时记录 WiFi 重连次数、本次在线时长、应用层收发字节。
 *
 * 关于流量：ESP-IDF 没有暴露网卡的字节计数器（lwIP 只统计包数），
 * 所以这里统计的是本机 HTTP 请求产生的字节。
 * 这台设备只做门户登录和天气，应用层流量基本就是它的全部实际流量。
 */
#ifndef _NETMON_H_
#define _NETMON_H_

#include <stdbool.h>
#include <stdint.h>

#define NETMON_HISTORY 40

typedef struct {
    int      rtt_ms;            /* 最近一次，-1 = 超时 */
    int      rtt_avg_ms;        /* 最近 10 次的平均，-1 = 全失败 */
    int      loss_percent;      /* 最近 20 次失败的比例 */
    int      history[NETMON_HISTORY];  /* 环形历史，-1 = 失败 */
    int      history_count;     /* 已填充个数 */
    int      history_head;      /* 下一个写入位置 */

    uint32_t online_seconds;    /* 本次连续在线时长 */
    uint32_t reconnect_count;   /* 断线重连次数 */
    uint32_t bytes_in;          /* 应用层接收字节 */
    uint32_t bytes_out;         /* 应用层发送字节 */
    bool     probing;           /* 探测任务是否在跑 */
    bool     online;            /* 当前是否连上网关 */
} netmon_t;

/** 启动监控任务（需要 WiFi 已开始连接） */
void netmon_start(void);

/** 取快照 */
void netmon_get(netmon_t *out);

/** 上报一次 WiFi 重连（由主程序在检测到掉线时调用） */
void netmon_note_reconnect(void);

/** 累加应用层收发字节（HTTP 封装里调用） */
void netmon_add_bytes(uint32_t in, uint32_t out);

/** 立刻探测一次（不影响定时任务） */
int netmon_ping_once(void);

#endif  // _NETMON_H_