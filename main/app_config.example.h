/*
 * app_config.h 的模板 —— 复制成 app_config.h 再改，别直接改这个文件
 *
 *     Windows:  copy app_config.example.h app_config.h
 *     Linux/mac: cp app_config.example.h app_config.h
 *
 * app_config.h 里有明文账号密码，所以它被 .gitignore 排除，不进版本库。
 * 改完编译即可；这些值也会写进 NVS，之后在网页后台也能改。
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ── WiFi ───────────────────────────────────────── */
#define APP_WIFI_SSID       "YOUR_WIFI_SSID"
#define APP_WIFI_PASS       "YOUR_WIFI_PASSWORD"

/* ── 宽带账号（校园网 / 运营商）─────────────────────
 * 这两项是登录门户用的账号密码，填你自己的。
 * ───────────────────────────────────────────────── */
#define APP_USER            "YOUR_ACCOUNT"
#define APP_PASSWORD        "YOUR_PASSWORD"
#define APP_STATIC_IP       ""                  /* 空 = 用 DHCP */
#define APP_ROT             0                   /* 屏幕方向 0/90/180/270 */

/* ── 校园网门户（也可以不改这里）──────────────────────
 * 这三项只是「出厂默认值」：烧进去之后能在**网页后台**
 * 或串口 `SET portal_base=...` 里改，写进 NVS，掉电不丢，不用重编。
 *
 *   portal_base  : 门户地址，形如 http://10.0.0.1:8090
 *   portal_ac_ip : AC 的 IP（跳转 URL 里的 wlanacip）
 *   portal_ssid  : 校园网 SSID（大小写必须和实际完全一致）
 * ───────────────────────────────────────────────── */
#define APP_PORTAL_BASE     "http://YOUR_PORTAL_IP:8090"
#define APP_PORTAL_AC_IP    "YOUR_AC_IP"
#define APP_PORTAL_SSID     "YOUR_CAMPUS_SSID"

/* ── 学校（纯展示）──────────────────────────────────
 * 显示在「模型」页，说明这个模型是给哪所学校训的。
 * 换学校时改这里；留空则显示「未配置」。
 * ───────────────────────────────────────────────── */
#define APP_SCHOOL          "太原理工大学"

/* ── 行为 ───────────────────────────────────────── */
#define APP_CHECK_INTERVAL_S   15   /* 在线时多久检查一次 */
#define APP_RETRY_MAX          3    /* 单轮内最多试几次登录 */

/* ── 省电 ───────────────────────────────────────── */
#define APP_SCREEN_OFF_MIN     1    /* 无操作几分钟后关背光，0=常亮 */
#define APP_BRIGHTNESS         80   /* 背光亮度 0-100 */
#define APP_PS_DIM_WHEN_IDLE   1    /* 息屏时 WiFi 进深度省电（掉线延迟会大一点） */

/* ── 日历数据（在线补全用）───────────────────────────
 * {year} 会被换成具体年份；拿不到就用板子内置表。
 * ───────────────────────────────────────────────── */
#define APP_CAL_URL \
    "https://cdn.jsdelivr.net/gh/NateScarlet/holiday-cn@master/{year}.json"

/* ── 位置（天气用，经纬度保留 2 位小数就够）─────────── */
#define APP_WEATHER_CITY    "北京"
#define APP_WEATHER_LAT     "39.90"
#define APP_WEATHER_LON     "116.41"

#endif /* APP_CONFIG_H */
