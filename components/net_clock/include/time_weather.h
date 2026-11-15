/*
 * 对时 + 天气
 *
 * 时间：SNTP 同步到阿里云 NTP（国内延迟低），同步后写入系统时间。
 * 天气：Open-Meteo 免费接口，不需要 API key。
 *
 * 天气每 30 分钟拉一次，失败不影响主流程 —— 屏幕只是少一行字。
 */
#ifndef _TIME_WEATHER_H_
#define _TIME_WEATHER_H_

#include <stdbool.h>

typedef struct {
    /* 时间 */
    bool time_valid;
    int  year;        /* 日历要用 */
    int  hour;
    int  minute;
    int  second;
    int  month;
    int  day;
    int  weekday;      /* 0 = 周日 */

    /* 天气 */
    bool        weather_valid;
    int         temp_c;      /* 温度，摄氏度 */
    int         humidity;    /* 相对湿度 % */
    const char *desc;        /* 中文描述，静态字符串 */
    int         code;        /* WMO 天气代码 */

    /* 扩展：预报 / 日出日落 / 空气质量 */
    bool daily_valid;
    char sunrise[8];         /* "06:42" */
    char sunset[8];          /* "17:12" */
    struct {
        int code;            /* WMO（国外源用） */
        int tmax;
        int tmin;
        char desc[16];       /* 国内源的文字描述，如 "中雨" */
    } fc[3];                 /* 今天 / 明天 / 后天 */

    char quality[8];         /* 空气质量文字：优 / 良 / 轻度污染 */

    bool aqi_valid;
    int  aqi;                /* US AQI */
    int  pm25;               /* µg/m³ */

    bool fetching;           /* 正在拉天气 */
    int  last_update_s;      /* 距上次成功更新多少秒，-1 = 从未成功 */
} time_weather_t;

/**
 * 启动对时和天气任务。
 * 位置从外面传进来，避免为了一个 app_config.h 去依赖 main 组件。
 */
void time_weather_start(const char *lat, const char *lon, const char *city);

/** 取快照 */
void time_weather_get(time_weather_t *out);

/** WMO 天气代码 → 中文描述（信息页预报行要用） */
const char *time_weather_wmo_text(int code);

/** 立即重新拉一次天气（联网恢复后调用） */
/** 强制立即刷新天气（忽略每天 8 点的限制） */
/** 全局 TLS 串行锁：每次 TLS 峰值要 30KB 连续内部 RAM，
    天气/空气质量/日历不要同时握手 */
void time_weather_tls_lock(void);
void time_weather_tls_unlock(void);

void time_weather_force_refresh(void);

void time_weather_refresh(void);

/** 手动刷新状态：0=无 1=进行中 2=成功 3=失败（8 秒后自动清） */
int time_weather_manual_state(void);

/** 距上次成功拉取多少秒（0=不知道） */
int time_weather_age_s(void);

#endif  // _TIME_WEATHER_H_