#include "calendar.h"
#include "calendar_remote.h"

#include <stddef.h>

static bool is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

bool cal_valid(int year, int month, int day)
{
    if (year < 2000 || year > 2099) return false;
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > cal_days_in_month(year, month)) return false;
    return true;
}

int cal_days_in_month(int year, int month)
{
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap(year)) return 29;
    return days[month - 1];
}

int cal_first_weekday(int year, int month)
{
    /* 按日历规则起算，蔡勒公式的常用变形 */
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year;
    if (month < 3) y -= 1;
    int w = (y + y / 4 - y / 100 + y / 400 + t[month - 1] + 1) % 7;
    if (w < 0) w += 7;
    return w;      /* 0 = 周日 */
}

bool cal_lookup(int year, int month, int day, cal_day_t *out)
{
    if (out != NULL) {
        out->name = NULL;
        out->flags = 0;
    }
    if (!cal_valid(year, month, day)) return false;

    uint32_t key = (uint32_t) (year * 10000 + month * 100 + day);

    /* 在线补全的数据优先：那是官方最新通知，可能是内置表没有的年份 */
    uint8_t remote_flags = 0;
    const char *remote_name = cal_remote_lookup(key, &remote_flags);
    if (remote_name != NULL) {
        if (out != NULL) {
            out->name = remote_name;
            out->flags = remote_flags;
        }
        return true;
    }

    /* 表是按日期排好序的，直接二分 */
    int lo = 0, hi = CAL_COUNT - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t v = cal_table[mid].date;
        if (v == key) {
            if (out != NULL) {
                out->flags = cal_table[mid].flags;
                out->name = (cal_table[mid].name < CAL_NAME_COUNT)
                                ? cal_names[cal_table[mid].name] : NULL;
            }
            return true;
        }
        if (v < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return true;       /* 合法日期，只是不是什么节日 */
}

/* 日期加一天 */
static void next_day(int *y, int *m, int *d)
{
    (*d)++;
    if (*d > cal_days_in_month(*y, *m)) {
        *d = 1;
        (*m)++;
        if (*m > 12) {
            *m = 1;
            (*y)++;
        }
    }
}

bool cal_next_holiday(int year, int month, int day, int *days_ahead, const char **name)
{
    int y = year, m = month, d = day;

    /* 最多往后找一年，2024-2031 都有数据，够用 */
    for (int i = 0; i < 366; i++) {
        cal_day_t cd;
        if (!cal_lookup(y, m, d, &cd)) {
            break;      /* 超出数据范围 */
        }
        if ((cd.flags & CAL_FLAG_OFF) && cd.name != NULL) {
            if (days_ahead) *days_ahead = i;
            if (name) *name = cd.name;
            return true;
        }
        next_day(&y, &m, &d);
    }
    return false;
}