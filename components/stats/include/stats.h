/*
 * 认证统计：按天记 认证成功 / 认证失败 / 掉线 次数，存 NVS（保留 7 天）
 *
 * 为什么放 NVS 而不是 RAM：设备可能随时被拔电，统计要能跨重启累积；
 * 每天一条记录，写频率很低（一天几次），不用担心 NVS 磨损。
 */
#ifndef _STATS_H_
#define _STATS_H_

#include <stdint.h>

typedef struct {
    uint32_t day;      /* YYYYMMDD，0 表示空槽 */
    uint16_t ok;       /* 认证成功次数 */
    uint16_t fail;     /* 认证失败次数 */
    uint16_t drop;     /* 掉线次数 */
} stats_day_t;

/** 从 NVS 载入历史（没数据就清零） */
void stats_init(void);

/** 记一次认证成功（只在"从离线变成在线"时调用，别每次探测都记） */
void stats_note_ok(void);

/** 记一次认证失败 */
void stats_note_fail(void);

/** 记一次掉线（WiFi 断开） */
void stats_note_drop(void);

/** 今天的统计；任何参数都可以传 NULL */
void stats_today(uint16_t *ok, uint16_t *fail, uint16_t *drop);

/** 最近 7 天合计 */
void stats_week(uint32_t *ok, uint32_t *fail, uint32_t *drop);

#endif  // _STATS_H_
