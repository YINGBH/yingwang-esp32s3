/*
 * 追踪标签被动扫描：只做「分类 + 记账」，扫描本身由 esl_ble 驱动
 *
 * 为什么这样分层：BLE 协议栈归 esl_ble 管（懒加载、自动扫描、空闲释放都在那边），
 * 外部再起一个 discovery 会互相抢、还会把协议栈拆掉（踩过）。
 * 所以这里只提供：
 *   tracker_scan_begin()   清空表，准备接收
 *   tracker_scan_feed()    esl_ble 的 GAP 回调把广播喂进来
 *   tracker_scan_finish()  扫描结束，记下耗时
 */
#ifndef _TRACKER_SCAN_H_
#define _TRACKER_SCAN_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 厂商分类 */
typedef enum {
    TRK_UNKNOWN = 0,
    TRK_APPLE_OFB,     /* 苹果 Find My（AirTag 等） */
    TRK_GOOGLE_FMDN,   /* 谷歌 Find My Device Network */
    TRK_SAMSUNG,       /* 三星 SmartTag */
    TRK_TILE,          /* Tile */
    TRK_DULT,          /* 跨平台反跟踪广播（苹果+谷歌） */
    TRK_HUAWEI,        /* 华为（只能到厂商级） */
    TRK_XIAOMI,        /* 小米（只能到厂商级） */
    TRK_VENDOR_COUNT
} trk_vendor_t;

/* 一个被扫到的设备（对外快照） */
typedef struct {
    uint8_t  addr[6];
    uint8_t  vendor;
    uint8_t  conf;
    int8_t   rssi_last;
    uint16_t seen;
    uint32_t dur_ms;
    uint8_t  flags;        /* bit0 near_owner（DULT/Apple） */
} trk_entry_t;

/** 开始一轮：清空表 */
void tracker_scan_begin(void);

/** 喂一条广播（由 esl_ble 的 GAP 回调调用） */
void tracker_scan_feed(const uint8_t *addr, int8_t rssi, const uint8_t *ad, int len);

/** 结束一轮：记耗时 */
void tracker_scan_finish(uint32_t cost_ms);

bool tracker_scan_busy(void);
int  tracker_scan_count(void);
uint32_t tracker_scan_last_cost_ms(void);
uint32_t tracker_scan_last_age_ms(void);
int  tracker_scan_vendor_count(trk_vendor_t v);
int  tracker_scan_top(trk_entry_t *out, int max);
void tracker_scan_clear(void);

#endif  // _TRACKER_SCAN_H_
