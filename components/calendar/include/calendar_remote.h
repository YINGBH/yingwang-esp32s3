/*
 * 日历在线补全
 *
 * 为什么需要：内置表是编译进去的，明年国务院发新通知后得重新烧录。
 * 这个模块让板子自己把新数据拉下来存进 NVS，存好之后照样离线可用。
 *
 * 策略（不是"每天联网"）：
 *   - 每 7 天拉一次，拉到了就存着，平时纯离线
 *   - 内置表没覆盖到当前年份时，强制拉一次
 *   - 拉不到就静默用内置表，日历照样能看
 *
 * 存储格式（紧凑二进制，比存 JSON 省内存，开机不用解析）：
 *   [magic u32][count u16][pool_len u16] + count*{date u32, flags u8, name_off u8} + 名字池
 */
#ifndef _CALENDAR_REMOTE_H_
#define _CALENDAR_REMOTE_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/** 开从 NVS 载入覆盖层（很快，开机就能调） */
esp_err_t cal_remote_init(void);

/** 设置数据地址模板，里面用 {year} 占位 */
void cal_remote_set_url(const char *tmpl);

/** 覆盖层里查一天，查到返回名字，没查到返回 NULL */
const char *cal_remote_lookup(uint32_t date, uint8_t *flags);

/** 覆盖层是否已包含某年的放假数据（判断要不要联网补） */
bool cal_remote_covers(int year);

/** 上次拉取成功的时间（unix 秒），0 表示从来没成功过 */
uint32_t cal_remote_last_ts(void);

/** 覆盖层里的日期范围 */
void cal_remote_range(int *from_year, int *to_year, int *count);

/**
 * 联网拉取指定年份。
 * @param force  true 时忽略"7 天内不重复拉"的限制
 * @return ESP_OK 表示拉到了并已存好
 */
esp_err_t cal_remote_fetch(int year, bool force);

#endif  // _CALENDAR_REMOTE_H_