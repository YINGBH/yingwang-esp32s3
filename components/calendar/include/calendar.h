/*
 * 日历 / 节假日查询
 *
 * 数据表是构建时生成的（工具/生成日历.py），固件里只有一张排序好的表，
 * 不带农历算法，省 flash 也省内存。
 *
 * 数据来源：国务院办公厅通知整理的开源数据（法定放假 + 调休上班），
 *           加上农历传统节日（春节/元宵/端午/七夕/中元/中秋/重阳/腊八/除夕）
 *           和固定公历节日（情人节/儿童节/教师节/圣诞节…）。
 */
#ifndef _CALENDAR_H_
#define _CALENDAR_H_

#include <stdbool.h>
#include <stdint.h>

#include "calendar_data.h"

typedef struct {
    const char *name;    /* 节日名，普通日子是 NULL */
    uint8_t     flags;   /* CAL_FLAG_* 组合 */
} cal_day_t;

/** 查某天是什么日子。不是节日也返回 true（out->name = NULL） */
bool cal_lookup(int year, int month, int day, cal_day_t *out);

/** 这个月有多少天 */
int cal_days_in_month(int year, int month);

/** 这个月 1 号是星期几，0 = 周日 */
int cal_first_weekday(int year, int month);

/**
 * 往后找下一个法定放假日。
 * @param days_ahead 还有多少天（0 = 就是今天）
 * @param name       节日名
 * @return 找到返回 true
 */
bool cal_next_holiday(int year, int month, int day, int *days_ahead, const char **name);

/** 日期合法性 */
bool cal_valid(int year, int month, int day);

#endif  // _CALENDAR_H_