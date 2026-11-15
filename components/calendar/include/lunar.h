/*
 * 农历 + 二十四节气
 *
 * 农历：1900~2100 年的标准压缩表（每年 1 个 32 位数：闰月 + 12/13 个月的大小），
 *       从 1900 年正月初一（公历 1900-01-31）开始累加天数换算。
 * 节气：通用寿星公式 day = int(Y*0.2422 + C) - int(Y/4)（21 世纪），
 *       C 表按节气顺序给出；个别年份有 1 天偏差，做界面上够用。
 */
#ifndef _LUNAR_H_
#define _LUNAR_H_

#include <stdbool.h>

typedef struct {
    int  year;          /* 农历年 */
    int  month;         /* 1~12，闰月用负数表示，如 -6 = 闰六月 */
    int  day;           /* 1~30 */
    bool valid;
} lunar_date_t;

/** 公历 → 农历。超出 1900~2100 返回 valid=false */
void lunar_from_solar(int y, int m, int d, lunar_date_t *out);

/** 农历月名，如 "正月" "腊月" "闰六月" */
const char *lunar_month_name(int month);

/** 农历日名，如 "初一" "十五" "廿三" "三十" */
const char *lunar_day_name(int day);

/** 今天如果是节气，返回节气名（如 "霜降"），否则 NULL */
const char *solar_term_of(int y, int m, int d);

/** 下一个节气（含今天）：返回名字和还有几天，找不到返回 NULL */
const char *solar_term_next(int y, int m, int d, int *days_ahead);

#endif  // _LUNAR_H_
