#include "lunar.h"

#include <stdio.h>
#include <string.h>

/* 1900~2100 年农历压缩表：低 4 位是闰月（0 表示无闰月），
   其余位从高到低表示各月大小（1=30 天，0=29 天） */
static const uint32_t kLunarInfo[] = {
    0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5,
    0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930,
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530,
    0x05aa0, 0x076a3, 0x096d0, 0x04afb, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45,
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0,
    0x14b63,
};
#define LUNAR_YEARS ((int) (sizeof(kLunarInfo) / sizeof(kLunarInfo[0])))

/* 2000 年正月初一 = 公历 2000-02-05 */
#define BASE_YEAR 2000

static int leap_month(int y)
{
    return (int) (kLunarInfo[y - BASE_YEAR] & 0xF);
}

static int leap_days(int y)
{
    if (leap_month(y) == 0) {
        return 0;
    }
    return (kLunarInfo[y - BASE_YEAR] & 0x10000) ? 30 : 29;
}

static int month_days(int y, int m)
{
    return (kLunarInfo[y - BASE_YEAR] & (0x10000 >> m)) ? 30 : 29;
}

static int year_days(int y)
{
    int sum = 348;                       /* 12 个月 × 29 */
    for (int i = 0x8000; i > 0x8; i >>= 1) {
        sum += (kLunarInfo[y - BASE_YEAR] & i) ? 1 : 0;
    }
    return sum + leap_days(y);
}

/* 公历 → "距离 1900-01-31 多少天" */
static int days_from_base(int y, int m, int d)
{
    static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int days = 0;
    for (int yy = BASE_YEAR; yy < y; yy++) {
        days += ((yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0) ? 366 : 365;
    }
    for (int mm = 1; mm < m; mm++) {
        days += mdays[mm - 1];
        if (mm == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) {
            days += 1;
        }
    }
    days += d - 1;
    /* 2000-01-01 到 2000-02-05 是 35 天，那天是农历 2000 正月初一 */
    days -= 35;
    return days;
}

void lunar_from_solar(int y, int m, int d, lunar_date_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (y < BASE_YEAR || y > BASE_YEAR + LUNAR_YEARS - 1) {
        return;
    }

    int offset = days_from_base(y, m, d);
    if (offset < 0) {
        return;
    }

    int ly = BASE_YEAR;
    while (ly < BASE_YEAR + LUNAR_YEARS - 1 && offset >= year_days(ly)) {
        offset -= year_days(ly);
        ly++;
    }

    int lm = 1;
    int leap = leap_month(ly);
    bool is_leap = false;
    while (lm <= 12) {
        int dm = is_leap ? leap_days(ly) : month_days(ly, lm);
        if (offset < dm) {
            break;
        }
        offset -= dm;
        if (leap == lm && !is_leap) {
            is_leap = true;              /* 闰月插在本月之后 */
        } else {
            is_leap = false;
            lm++;
        }
    }

    if (lm > 12) {
        lm = 12;
    }
    out->year = ly;
    out->month = is_leap ? -lm : lm;
    out->day = offset + 1;
    out->valid = true;
}

const char *lunar_month_name(int month)
{
    static const char *names[] = {
        "", "正月", "二月", "三月", "四月", "五月", "六月",
        "七月", "八月", "九月", "十月", "冬月", "腊月"
    };
    static char buf[16];
    bool leap = month < 0;
    int m = leap ? -month : month;
    if (m < 1 || m > 12) {
        return "?";
    }
    if (leap) {
        snprintf(buf, sizeof(buf), "闰%s", names[m]);
        return buf;
    }
    return names[m];
}

const char *lunar_day_name(int day)
{
    static const char *digit[] = {"", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十"};
    static char buf[16];
    if (day < 1 || day > 30) {
        return "?";
    }
    if (day == 10) return "初十";
    if (day == 20) return "二十";
    if (day == 30) return "三十";
    int t = day / 10, u = day % 10;
    const char *prefix = (t == 0) ? "初" : (t == 1 ? "十" : (t == 2 ? "廿" : "三"));
    if (t == 1) {
        snprintf(buf, sizeof(buf), "十%s", digit[u]);
    } else {
        snprintf(buf, sizeof(buf), "%s%s", prefix, digit[u]);
    }
    return buf;
}

/* ---------------- 二十四节气（21 世纪通用公式） ---------------- */

static const char *kTermName[24] = {
    "小寒", "大寒", "立春", "雨水", "惊蛰", "春分",
    "清明", "谷雨", "立夏", "小满", "芒种", "夏至",
    "小暑", "大暑", "立秋", "处暑", "白露", "秋分",
    "寒露", "霜降", "立冬", "小雪", "大雪", "冬至"
};

/* 21 世纪各节气的 C 值（第 0 个是小寒，1 月） */
static const float kTermC[24] = {
    5.4055f, 20.12f, 3.87f, 18.73f, 5.63f, 20.646f,
    4.81f, 20.1f, 5.52f, 21.04f, 5.678f, 21.37f,
    7.108f, 22.83f, 7.5f, 23.13f, 7.646f, 23.042f,
    8.318f, 23.438f, 7.438f, 22.36f, 7.18f, 21.94f
};

/* 第 i 个节气落在第 (i/2 + 1) 月 */
static int term_day(int year, int idx)
{
    if (year < 2000 || year > 2099) {
        return -1;
    }
    int y = year % 100;
    float v = (float) y * 0.2422f + kTermC[idx] - (float) (y / 4);
    return (int) v;
}

const char *solar_term_of(int y, int m, int d)
{
    if (m < 1 || m > 12) {
        return NULL;
    }
    for (int k = 0; k < 2; k++) {
        int idx = (m - 1) * 2 + k;
        int day = term_day(y, idx);
        if (day == d) {
            return kTermName[idx];
        }
    }
    return NULL;
}

const char *solar_term_next(int y, int m, int d, int *days_ahead)
{
    if (days_ahead) {
        *days_ahead = -1;
    }
    /* 简化：只在本月和下月里找，够日历页用 */
    for (int offset_m = 0; offset_m <= 1; offset_m++) {
        int mm = m + offset_m;
        int yy = y;
        if (mm > 12) {
            mm -= 12;
            yy++;
        }
        for (int k = 0; k < 2; k++) {
            int idx = (mm - 1) * 2 + k;
            int day = term_day(yy, idx);
            if (day < 0) {
                continue;
            }
            int ahead = (offset_m == 0) ? (day - d) : (day + 30 - d);
            if (ahead >= 0) {
                if (days_ahead) {
                    *days_ahead = ahead;
                }
                return kTermName[idx];
            }
        }
    }
    return NULL;
}
