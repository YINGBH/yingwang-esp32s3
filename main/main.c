/**
 * 联通校园宽带自动登录 · ESP32-S3（Nologo 星智 1.54 寸屏）
 *
 * 全本地运行：WiFi → 探测劫持 → 抓 AC 参数 → 取验证码 → 本地识别
 *           → RSA 加密 → 提交登录 → 循环守护
 *
 * 六个界面，UP/DOWN 切换（验证码页只在登录时临时占用）：
 *   1. 状态：WiFi、IP、信号、门户
 *   2. 信息：时间、日期、天气、在线时长、重连次数、流量
 *   3. 时钟：整屏时间，支持背景图
 *   4. 日历：月视图 + 节假日
 *   5. 诊断：板载日志滚动（最近若干条 ESP_LOG，环里取）
 *   6. 系统：版本、运行时长、内存、复位原因、芯片、SSID/IP/信号、账号、AC、模型
 *
 * BOOT：立即重试；验证码上屏时按 BOOT = 这张识别错了，换一张
 */

#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include <time.h>
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_clk_tree.h"
#include "esp_chip_info.h"
#include "esp_image_format.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "wifi_sta.h"
#include "portal_client.h"

#include "bsp_display.h"
#include "bsp_power.h"
#include "bsp_buttons.h"
#include "audio_level.h"
#include "time_weather.h"
#include "app_state.h"
#include "config_store.h"
#include "esl_ble.h"
#include "screen_bg.h"
#include "calendar.h"
#include "lunar.h"
#include "calendar_remote.h"
#include "usb_console.h"
#include "captcha_loader.h"
#include "stats.h"
#include "beep.h"
#include "logring.h"

#define BTN_SCAN_MODE 0   /* 1=按键扫描诊断固件 */
static const char *TAG = "main";
#define FW_AUTHOR "YING"            /* 作者签名 */

/* ------------------------------------------------------------------ 状态 */

typedef enum {
    ST_BOOT, ST_WIFI, ST_PROBE, ST_LOGIN, ST_ONLINE, ST_NOWIFI, ST_ERROR
} state_t;

typedef enum {
    SC_STATUS = 0, SC_INFO, SC_CLOCK, SC_CALENDAR, SC_DIAG, SC_SYS, SC_ESL,
    SC_TIMER,            /* 倒计时：BOOT 确认，UP/DN 加减 1 分钟 */
    SC_STOPWATCH,        /* 秒表：BOOT 开始/暂停，UP 归零，DN 翻页 */
    SC_ALARMS,           /* 作息页：显示 4 个闹钟 + 倒计时 + 秒表（只读汇总） */
    SC_MODEL,            /* 验证码模型信息：不进翻页循环，只能状态页长按 BOOT 进 */
    SC_CAPTCHA,          /* 不是可循环页面，登录时临时占用 */
    SC_ALARM,            /* 闹钟响铃页：不进翻页循环 */
    SC_COUNT
} screen_t;

/* UP/DOWN 只在前面这几屏之间循环（不含模型页和验证码页）。
   模型页是"隐藏页"，只从状态页长按 BOOT 进入。 */
#define SC_CYCLABLE SC_MODEL

/* 跨任务共享，都是单字节/单字写入，不需要锁 */
static volatile int      s_state = ST_BOOT;
static int64_t now_ms(void);            /* 前置声明 */
static volatile int      s_screen = SC_CLOCK;
/* 按键推送倒计时：0 = 没有待执行的推送，否则到点再推 */
static int64_t           s_esl_push_at_ms = 0;
static bool              s_esl_push_test_card = false;

/* 时钟页增量刷新 + 长按状态（本次误删后补回） */
static bool              s_clock_full = false;
static int               s_clock_minute = -1;
static int               s_clock_second = -1;

static volatile bool     s_ui_freeze = false;   /* 截图用：暂停界面刷新 */
static volatile bool     s_force_auth = false;  /* 强制走一次门户认证 */
static volatile bool     s_portal_auth = true;  /* 是否做门户认证（板子负责这天打一次卡） */
static int s_last_login_day = 0;   /* 上次完成每日登录的日期，20261103 这种 */
static int s_esl_push_day = 0;     /* 上次推墨水屏的日期 */
static int64_t  s_esl_try_ms = 0;   /* 上次尝试推墨水屏的时间 */
static int s_stuck_cnt = 0;        /* 有 IP 但完全不通的连续次数 */
static int64_t s_last_ok_probe_ms = 0;  /* 上次探测成功的时间 */
static int64_t s_last_dns_fix_ms = 0;   /* 上次重设 DNS 的时间 */
static int64_t s_online_since_ms = 0;  /* 本次进入"已认证"的时刻 */
static int               s_boot3_cnt = 0;      /* 状态页连按 BOOT 计数 */
static int64_t           s_boot3_ms = 0;       /* 上次按的时间 */
static int               s_bl_level = -1;       /* 当前背光（夜间调暗用） */
static bool               s_was_online = false;          /* 上次探测是不是在线 */
static int64_t            s_last_ok_ms = 0;             /* 上次记认证成功的时间 */

/* ---- 语音备忘（录音） ----------------
   板端只有 audio_rec_* 这套采集 API，没有落到 flash、也没有对应的串口命令；
   网页上的录音面板因此已移除（见 docs/踩坑记录.md）。这里不再定义录音长度常量。 */

/* ---- 闹钟 ---------------- */
/* 定时推送价签（每天中午一次）：默认关闭。
   联网后 30 秒的每日刷屏已经会真推，多推一次只会多一次
   BLE 抢射频导致的 WiFi 掉线。需要时改成 1 即可恢复。 */
#define ENABLE_PERIODIC_ESL_PUSH 0

/* ---- 防残影 ---------------- */
/* 亮屏时每隔这么多分钟把整屏翻转刷新一次（0 = 关闭）。
   IPS 面板固定显示同一幅高对比画面会留下"鬼影"，而且**切页也不会消失**——
   这是面板特性，固件能做的只有：定期整体翻转 + 熄屏时把画面清掉。 */
#ifndef ANTI_STICK_MIN
#define ANTI_STICK_MIN 30
#endif

#define ALARM_MAX 4
typedef struct {
    uint8_t hour, min;
    uint8_t mask;      /* bit0=周一 … bit6=周日；0 表示每天 */
    bool    en;
} alarm_t;
static alarm_t  s_alarms[ALARM_MAX];
static volatile bool s_alarm_ringing = false;
static int      s_alarm_last_min = -1;   /* 上次触发过的分钟数，防重复 */

/* ---- 倒计时 ---- */
static int      s_timer_state = 0;    /* 0=设置 1=运行 2=暂停 3=到点 */
static int      s_timer_min = 5;      /* 设定分钟数 */
static int64_t  s_timer_end_ms = 0;   /* 运行时的结束时刻 */
static int64_t  s_timer_left_ms = 0;  /* 暂停/到点时剩下的毫秒 */

/* ---- 秒表（独立页）---- */
static bool     s_sw_running = false;
static int64_t  s_sw_start_ms = 0;    /* 本次开始时刻 */
static int64_t  s_sw_base_ms = 0;     /* 暂停前累计的毫秒 */

/* ---- 声控提醒 ---- */
static volatile bool s_noise_alert = false;   /* 正在提醒"环境很吵" */
static int64_t  s_noise_until_ms = 0;         /* 提醒持续到这个时刻 */
#define NOISE_PCT      70        /* 电平超过 70% 算吵 */
#define NOISE_HOLD_MS  5000      /* 连续 5 秒才提醒 */
#define NOISE_SHOW_MS  30000     /* 提醒显示 30 秒 */
/* 夜间省电：WiFi 大部分时间关闭，整点才开一会儿对时/取天气 */
static volatile bool     s_night_idle = false;
static volatile int      s_boot_pressed = 0;
/* 最近一次登录用的 AC，系统页显示 */
static char              s_last_ac[48] = "";

/* 离线多久就重启（连不上 WiFi 时的自愈） */
#define OFFLINE_REBOOT_MS  (5LL * 60 * 1000)   /* 实测按复位就能恢复，5 分钟连不上直接重启 */

/* ---- 电源管理状态（ui_task 和按键回调都要用，放在前面）---- */
static volatile int64_t s_last_input_ms = 0;   /* 最近一次按键时刻 */
static bool             s_dimmed = false;        /* 背光已降到 30% */
static volatile bool    s_screen_off = false;  /* 当前是否息屏 */
/* 息屏但背光没关（off_bright > 0 的低亮度常显）：画面要继续更新，
   否则时钟会冻在熄屏那一刻 */
static bool             s_aod = false;
static volatile bool    s_force_redraw = false;/* 唤醒后整屏重画 */
static void alarm_tick(void);      /* 前置声明 */
static void pm_tick(void);
static void pm_wake(void);   /* 和 portal_wlan_t.wlan_ac_ip 同宽 */
static char              s_detail[40] = "";

/* ------------------------------------------------------------------ 工具 */

static const char *state_text(int st, uint16_t *color)
{
    switch (st) {
    case ST_BOOT:   *color = TH_INFO;   return "启动";
    case ST_WIFI:   *color = TH_ACCENT; return "连接中";
    case ST_PROBE:  *color = TH_ACCENT; return "检测中";
    case ST_LOGIN:  *color = COLOR_ORANGE; return "登录中";
    case ST_ONLINE: *color = TH_OK;  return "已认证";
    case ST_NOWIFI: *color = TH_WARN;    return "未连接";
    case ST_ERROR:  *color = TH_WARN;    return "失败";
    default:        *color = TH_DIM;   return "就绪";
    }
}

static void set_state(int st, const char *detail)
{
    s_state = st;
    if (detail != NULL) {
        snprintf(s_detail, sizeof(s_detail), "%s", detail);
    }
    /* 屏幕和网页共用同一套状态措辞 */
    app_state_set((app_state_t) st, detail);
}

static int text_w(const char *s, uint8_t scale)
{
    return bsp_display_text_width(s, scale);
}

/* 顶部标题条：左边标题，右边状态灯 */
static void draw_header(const char *title, int state)
{
    uint16_t color;
    state_text(state, &color);
    const char *text = title;

    bsp_display_fill_rect(0, 0, 240, 24, color);
    bsp_display_text(6, 4, text, COLOR_BLACK, color, 1);
}

/* 一行「标签  值」 */
static int draw_row(int y, const char *label, const char *value, uint16_t color)
{
    bsp_display_text(8, y, label, TH_DIM, TH_BG, 1);
    bsp_display_text(8 + text_w(label, 1) + 4, y, value, color, TH_BG, 1);
    return y + 18;
}

/* 把秒数变成「1时23分」 */
static void fmt_duration(uint32_t sec, char *out, size_t cap)
{
    uint32_t h = sec / 3600;
    uint32_t m = (sec % 3600) / 60;
    uint32_t s = sec % 60;
    if (h > 0) {
        snprintf(out, cap, "%lu时%lu分", (unsigned long) h, (unsigned long) m);
    } else if (m > 0) {
        snprintf(out, cap, "%lu分%lu秒", (unsigned long) m, (unsigned long) s);
    } else {
        snprintf(out, cap, "%lu秒", (unsigned long) s);
    }
}

/* ---------------------------------------------------------------- 界面一 */

static void render_status(void)
{
    uint16_t color;
    const char *st = state_text(s_state, &color);
    char buf[48];

    bsp_display_fill_rect(0, 24, 240, 216, TH_BG);
    bsp_deco_draw_grid();

    /* 顶部色条 + 大号状态字 */
    bsp_display_fill_rect(0, 24, 240, 4, color);
    int w = text_w(st, 2);
    bsp_display_text((240 - w) / 2, 36, st, color, TH_BG, 2);

    /* 数据新鲜度：距上次成功拉取有多少秒（0 = 还没有数据）
       放在大状态字下面那一行，一眼看出"现在显示的数据过期多久了" */
    {
        int age = time_weather_age_s();
        char abuf[40];
        uint16_t ac;
        if (age <= 0) {
            snprintf(abuf, sizeof(abuf), "数据: 无");
            ac = TH_DIM;
        } else {
            snprintf(abuf, sizeof(abuf), "数据: %d 秒前", age);
            /* 1 小时内绿、2 小时内黄、更久红（本来就是每小时拉一次） */
            ac = age < 3600 ? TH_OK : (age < 7200 ? TH_ACCENT : TH_WARN);
        }
        int aw = text_w(abuf, 1);
        bsp_display_text((240 - aw) / 2, 72, abuf, ac, TH_BG, 1);
    }

    int y = 96;
    y = draw_row(y, "WiFi", wifi_sta_connected() ? wifi_sta_ssid() : "--",
                 wifi_sta_connected() ? TH_FG : TH_DIM);
    y = draw_row(y, "IP", wifi_sta_connected() ? wifi_sta_ip() : "--", TH_FG);

    if (wifi_sta_connected()) {
        int rssi = wifi_sta_rssi();
        snprintf(buf, sizeof(buf), "%d dBm", rssi);
        uint16_t c = rssi > -60 ? TH_OK : (rssi > -75 ? TH_ACCENT : TH_WARN);
        y = draw_row(y, "信号", buf, c);
    }

    {
        bool chg = bsp_power_is_charging();
        y = draw_row(y, "电源", chg ? "充电中" : "电池供电",
                     chg ? TH_OK : TH_ACCENT);
    }

    if (s_detail[0]) {
        y = draw_row(y, "详情", s_detail, TH_INFO);
    }

    /* 运行时长 + 可用内存：一眼看出板子状态 */
    {
        int64_t up_s = esp_timer_get_time() / 1000000;
        int hh = (int) (up_s / 3600);
        int mm2 = (int) ((up_s % 3600) / 60);
        size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        snprintf(buf, sizeof(buf), "%d小时%d分  内存 %uK", hh, mm2, (unsigned) (free_int / 1024));
        y = draw_row(y, "运行", buf, TH_DIM);
    }

    bsp_display_text(6, 214, "BOOT 重试  连按3次 重新认证", TH_DIM, TH_BG, 1);
}

/* ------------------------------------------------------------- 墨水价签页 */

/* 验证码模型信息页：状态页长按 BOOT 进入 */

static void render_model(void)
{
    char buf[64];
    bsp_display_fill_rect(0, 24, 240, 216, TH_BG);
    bsp_deco_draw_grid();

    captcha_model_info_t mi;
    memset(&mi, 0, sizeof(mi));
    captcha_model_load();          /* 只用一次，重复调用直接返回上次结果 */
    captcha_model_get_info(&mi);

    int y = 32;
    y = draw_row(y, "状态", mi.ready ? "已加载" : "未加载",
                 mi.ready ? TH_OK : TH_WARN);
    /* 这个模型是给哪所学校训的（app_config.h 里的 APP_SCHOOL） */
    y = draw_row(y, "学校", APP_SCHOOL, TH_FG);

    if (mi.ready) {
        snprintf(buf, sizeof(buf), "ESL1-%06X", (unsigned) (mi.crc & 0xFFFFFFu));
        y = draw_row(y, "版本", buf, TH_INFO);

        snprintf(buf, sizeof(buf), "%u K", (unsigned) (mi.params / 1024));
        y = draw_row(y, "参数", buf, TH_FG);

        snprintf(buf, sizeof(buf), "%u KB", (unsigned) (mi.bytes / 1024));
        y = draw_row(y, "大小", buf, TH_FG);

        y = draw_row(y, "张量", "8 个（INT16 量化）", TH_DIM);
        y = draw_row(y, "输入", "62 x 25 x 3", TH_DIM);
        y = draw_row(y, "输出", "4 位 x 9 类", TH_DIM);
        y = draw_row(y, "来源", "storage 分区映射", TH_DIM);
    } else {
        y = draw_row(y, "提示", "先烧 model_q.bin 到 storage", TH_ACCENT);
    }

    bsp_display_text(6, 222, "长按 BOOT 回状态页", TH_DIM, TH_BG, 1);
}

static void render_esl(void)
{
    bsp_display_fill_rect(0, 24, 240, 216, TH_BG);
    bsp_deco_draw_grid();

    char st[48];

    /* ---------- 1) 倒计时中（按了 BOOT，3 秒后开始推） ---------- */
    if (s_esl_push_at_ms != 0) {
        int64_t left = s_esl_push_at_ms - esp_timer_get_time() / 1000;
        int sec = (int) ((left + 999) / 1000);
        if (sec < 0) {
            sec = 0;
        }
        snprintf(st, sizeof(st), "%d 秒后推送", sec);
        int sc_cd = (text_w(st, 2) <= 228) ? 2 : 1;
        bsp_display_text_center(sc_cd == 2 ? 52 : 60, st, TH_ACCENT, TH_BG, sc_cd);
        bsp_display_text_center(96, "先按一下价签唤醒它", TH_FG, TH_BG, 1);
        bsp_display_text_center(120, "再按 BOOT 取消", TH_DIM, TH_BG, 1);

        /* 倒计时进度条：3 秒走完 */
        int64_t total = 3000;
        int64_t done = total - (left > 0 ? left : 0);
        int w = (int) (done * 196 / total);
        if (w < 0) w = 0;
        if (w > 196) w = 196;
        bsp_display_fill_rect(22, 150, 196, 14, TH_PANEL);
        if (w > 0) {
            bsp_display_fill_rect(22, 150, w, 14, TH_ACCENT);
        }
        return;
    }

    /* ---------- 2) 推图中：进度条 ---------- */
    if (esl_ble_state() == ESL_PUSHING) {
        int pct = esl_ble_progress();
        snprintf(st, sizeof(st), "推图中 %d%%", pct);
        int sc_pg = (text_w(st, 2) <= 228) ? 2 : 1;
        bsp_display_text_center(sc_pg == 2 ? 52 : 60, st, TH_INFO, TH_BG, sc_pg);
        bsp_display_fill_rect(22, 100, 196, 18, TH_PANEL);
        int w = 196 * pct / 100;
        if (w > 0) {
            bsp_display_fill_rect(22, 100, w, 18, TH_OK);
        }
        bsp_display_text_center(136, esl_ble_status(), TH_DIM, TH_BG, 1);
        bsp_display_text_center(160, "请勿离开价签附近", TH_DIM, TH_BG, 1);
        return;
    }

    /* ---------- 3) 空闲：状态 + 说明 ---------- */
    snprintf(st, sizeof(st), "%s", esl_ble_status());
    /* 按宽度自动选字号，绝不超出屏幕 */
    {
        int sc_idle = 2;
        while (sc_idle > 1 && text_w(st, sc_idle) > 228) {
            sc_idle--;
        }
        bsp_display_text_center(sc_idle == 2 ? 46 : 52, st, TH_INFO, TH_BG, sc_idle);
    }

    bsp_display_text(8, 96, "墨水价签 HINK-E029A14", TH_DIM, TH_BG, 1);
    bsp_display_text(8, 116, "296x128 黑白红三色", TH_DIM, TH_BG, 1);

    bsp_display_text(8, 146, "短按 BOOT 推送信息页", TH_FG, TH_BG, 1);
    bsp_display_text(8, 166, "长按 BOOT 推送测试卡", TH_FG, TH_BG, 1);
    bsp_display_text(8, 186, "价签要先关掉手机的连接", TH_ACCENT, TH_BG, 1);

    bsp_display_text(6, 222, "BOOT 刷新价签  UP/DN 切页", TH_DIM, TH_BG, 1);
}

/* ---------------------------------------------------------------- 界面二 */

static void render_info(void)
{
    char buf[64];
    time_weather_t tw;
    time_weather_get(&tw);

    bsp_display_fill_rect(0, 24, 240, 216, TH_BG);
    bsp_deco_draw_grid();

    /* 时间：大号 */
    if (tw.time_valid) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tw.hour, tw.minute, tw.second);
        int w = text_w(buf, 3);
        bsp_display_text((240 - w) / 2, 30, buf, TH_FG, TH_BG, 3);

        static const char *wd[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        snprintf(buf, sizeof(buf), "%d月%d日 %s", tw.month, tw.day,
                 (tw.weekday >= 0 && tw.weekday < 7) ? wd[tw.weekday] : "");
        bsp_display_text_center(82, buf, TH_DIM, TH_BG, 1);
    } else {
        bsp_display_text_center(48, "对时中", TH_ACCENT, TH_BG, 2);
    }

    int y = 106;
    if (tw.weather_valid) {
        snprintf(buf, sizeof(buf), "%s %dC   湿度 %d%%", tw.desc, tw.temp_c, tw.humidity);
        y = draw_row(y, "天气", buf, TH_INFO);
    } else {
        y = draw_row(y, "天气", tw.fetching ? "获取中" : "无数据", TH_DIM);
    }

    if (tw.daily_valid && tw.sunrise[0] != '\0') {
        snprintf(buf, sizeof(buf), "%s  日落 %s", tw.sunrise, tw.sunset);
        y = draw_row(y, "日出", buf, TH_FG);
    } else {
        y = draw_row(y, "日出", "--", TH_DIM);
    }

    if (tw.aqi_valid) {
        const char *lvl = tw.quality[0] ? tw.quality
                        : (tw.aqi <= 50 ? "优" : (tw.aqi <= 100 ? "良" : (tw.aqi <= 150 ? "轻度" : "差")));
        uint16_t ac = tw.aqi <= 50 ? TH_OK : (tw.aqi <= 100 ? TH_ACCENT : TH_WARN);
        snprintf(buf, sizeof(buf), "%d %s  PM2.5 %d", tw.aqi, lvl, tw.pm25);
        y = draw_row(y, "空气", buf, ac);
    } else {
        y = draw_row(y, "空气", "--", TH_DIM);
    }

    {
        static const char *wdays[] = {"今天", "明天", "后天"};
        for (int i = 1; i <= 2; i++) {
            if (tw.daily_valid) {
                const char *d0 = tw.fc[i].desc[0] ? tw.fc[i].desc
                                : time_weather_wmo_text(tw.fc[i].code);
                snprintf(buf, sizeof(buf), "%s %d/%d", d0, tw.fc[i].tmin, tw.fc[i].tmax);
                y = draw_row(y, wdays[i], buf, TH_FG);
            } else {
                y = draw_row(y, wdays[i], "--", TH_DIM);
            }
        }
    }

    (void) y;   /* 行位置由下面的固定坐标接管 */

    /* 位置和后台地址都是运行时值，网页改了立刻反映到屏幕上 */
    app_settings_t cfg;
    config_store_snapshot(&cfg);

    /* ---- 底部两行：操作提示 + 数据新鲜度 ----
       注意：屏幕只有 240 高，16px 的字最靠下只能画到 y=224，
       以前两块都画在 236，导致下面那行被截掉。 */
    int ms = time_weather_manual_state();
    if (ms == 1) {
        bsp_display_text(6, 198, "正在更新天气…", TH_ACCENT, TH_BG, 1);
    } else if (ms == 2) {
        bsp_display_text(6, 198, "天气已更新", TH_OK, TH_BG, 1);
    } else if (ms == 3) {
        bsp_display_text(6, 198, "更新失败，稍后再试", TH_WARN, TH_BG, 1);
    } else {
        bsp_display_text(6, 198, "长按 BOOT 更新天气", TH_INFO, TH_BG, 1);
    }

    {
        int age = time_weather_age_s();
        char abuf[96];
        uint16_t ac;
        if (!s_was_online && tw.weather_valid) {
            int mins = age > 0 ? age / 60 : 0;
            snprintf(abuf, sizeof(abuf), "离线 · 数据 %d 分钟前  %s",
                     mins > 0 ? mins : 1, cfg.city);
            ac = TH_ACCENT;
        } else if (age > 0) {
            snprintf(abuf, sizeof(abuf), "数据 %d 秒前  %s", age, cfg.city);
            ac = age < 3600 ? TH_DIM : (age < 7200 ? TH_ACCENT : TH_WARN);
        } else {
            snprintf(abuf, sizeof(abuf), "数据 无  %s", cfg.city);
            ac = TH_DIM;
        }
        bsp_display_text(6, 218, abuf, ac, TH_BG, 1);
    }
}

/* ---------------------------------------------------------------- 界面三 */

/* ---------------------------------------------------------------- 倒计时页 */

static void render_timer(void)
{
    char buf[32];
    int64_t now = esp_timer_get_time() / 1000;
    int64_t left;

    bsp_display_fill_rect(0, 24, 240, 216, TH_BG);
    bsp_deco_draw_grid();

    if (s_timer_state == 1) {
        left = s_timer_end_ms - now;
        if (left < 0) left = 0;
    } else if (s_timer_state == 2 || s_timer_state == 3) {
        left = s_timer_left_ms;
    } else {
        left = (int64_t) s_timer_min * 60000;
    }

    int sec = (int) ((left + 999) / 1000);
    snprintf(buf, sizeof(buf), "%02d:%02d", sec / 60, sec % 60);

    uint16_t c = TH_FG;
    if (s_timer_state == 1) {
        c = (sec <= 10) ? TH_WARN : TH_OK;
    } else if (s_timer_state == 2) {
        c = TH_ACCENT;
    } else if (s_timer_state == 3) {
        c = ((now / 400) % 2) ? TH_WARN : COLOR_ORANGE;   /* 闪一下 */
    }

    int w = text_w(buf, 4);
    bsp_display_text((240 - w) / 2, 74, buf, c, TH_BG, 4);

    const char *st = (s_timer_state == 0) ? "设置中，BOOT 开始"
                   : (s_timer_state == 1) ? "运行中，BOOT 暂停"
                   : (s_timer_state == 2) ? "已暂停，BOOT 继续"
                   : "时间到！按任意键停止";
    bsp_display_text_center(148, st, TH_DIM, TH_BG, 1);

    if (s_timer_state == 0) {
        snprintf(buf, sizeof(buf), "%d 分钟", s_timer_min);
        bsp_display_text_center(176, buf, TH_INFO, TH_BG, 1);
    }
    bsp_display_text(6, 200, "长按 BOOT 回状态页", TH_DIM, TH_BG, 1);
    bsp_display_text(6, 222, "BOOT 确认   UP/DN 调时·翻页", TH_DIM, TH_BG, 1);
}

/* 秒表页：BOOT 开始/暂停，UP 归零，DN 翻页 */
static void render_stopwatch(void)
{
    char buf[32];
    int64_t now = esp_timer_get_time() / 1000;

    bsp_display_fill_rect(0, 24, 240, 216, TH_BG);
    bsp_deco_draw_grid();

    int64_t el = s_sw_base_ms + (s_sw_running ? (now - s_sw_start_ms) : 0);
    int sec = (int) (el / 1000);
    if (sec > 99 * 60 + 59) {
        sec = 99 * 60 + 59;
    }
    snprintf(buf, sizeof(buf), "%02d:%02d", sec / 60, sec % 60);

    int w = text_w(buf, 4);
    bsp_display_text((240 - w) / 2, 74, buf,
                     s_sw_running ? TH_OK : TH_FG, TH_BG, 4);

    bsp_display_text_center(148,
                            s_sw_running ? "运行中，BOOT 暂停" : "已暂停，BOOT 继续",
                            TH_DIM, TH_BG, 1);
    if (!s_sw_running && el == 0) {
        bsp_display_text_center(176, "BOOT 开始", TH_INFO, TH_BG, 1);
    }
    bsp_display_text(6, 200, "长按 BOOT 回状态页", TH_DIM, TH_BG, 1);
    bsp_display_text(6, 222, "UP 归零   DN 翻页", TH_DIM, TH_BG, 1);
}
/* ---------------------------------------------------------- 语音备忘页 */
static void render_alarm(void)
{
    static bool blink = false;
    blink = !blink;
    bsp_display_fill_rect(0, 24, 240, 216, blink ? TH_WARN : TH_BG);
    uint16_t fg = blink ? TH_FG : TH_WARN;

    bsp_display_text_center(70, "闹钟", fg, blink ? TH_WARN : TH_BG, 4);
    char buf[32];
    time_weather_t tw;
    time_weather_get(&tw);
    if (tw.time_valid) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tw.hour, tw.minute);
        int w = text_w(buf, 3);
        bsp_display_text((240 - w) / 2, 120, buf, fg, blink ? TH_WARN : TH_BG, 3);
    }
    bsp_display_text_center(182, "按任意键停止", fg, blink ? TH_WARN : TH_BG, 1);
}



/* 小铃铛图标（12x12），用 fill_rect 逐点亮，颜色跟状态走 */
static void draw_bell_icon(int x, int y, uint16_t color)
{
    /* 每一行 12 位，最高位是最左边一列 */
    static const uint16_t rows[12] = {
        0x060,  /* ....##...... */
        0x0F0,  /* ...####..... */
        0x1F8,  /* ..######.... */
        0x1F8,  /* ..######.... */
        0x3FC,  /* .########... */
        0x3FC,  /* .########... */
        0x7FE,  /* ###########. */
        0x7FE,  /* ###########. */
        0xFFF,  /* ############ */
        0xFFF,  /* ############ */
        0x0F0,  /* ...####..... 铃舌 */
        0x0F0,  /* ...####..... */
    };
    for (int r = 0; r < 12; r++) {
        for (int c = 0; c < 12; c++) {
            if (rows[r] & (uint16_t) (0x800 >> c)) {
                bsp_display_fill_rect(x + c, y + r, 1, 1, color);
            }
        }
    }
}

/* ---------------------------------------------------------------- 作息页
 *
 * 设计规则（参考 TRMNL 屏幕设计指南）：
 *   - 区域要少而强，不要做"仪表盘堆砌"
 *   - 深底上不要用暗灰写字（在低色深屏上会直接消失）
 *   - 紧凑屏先砍第三优先级（秒表），而不是把它缩小硬塞
 *   - 所有元素对齐到网格，不要逐像素手挪
 *
 * 网格：
 *   列线  COL_NUM=8   COL_TIME=26   COL_WD=112   COL_EN=200
 *   行线  首个闹钟 y=52，行距 34（2 倍字高 32 + 2 空隙）
 *         闹钟区 52..186，分隔线 188，倒计时 196..228
 */
#define COL_NUM   8
#define COL_TIME  26
#define COL_WD    112
#define COL_EN    200

static void render_alarms(void)
{
    char buf[48];
    bsp_display_fill_rect(0, 24, 240, 216, TH_BG);
    bsp_deco_draw_grid();

    time_weather_t tw;
    time_weather_get(&tw);

    /* 启用数量，给标题行一个有用的补充信息 */
    int enabled = 0;
    for (int i = 0; i < ALARM_MAX; i++) {
        if (s_alarms[i].en) {
            enabled++;
        }
    }

    /* ---------------- 标题行 ---------------- */
    snprintf(buf, sizeof(buf), "闹钟 %d/%d", enabled, ALARM_MAX);
    bsp_display_text(COL_NUM, 26, buf, TH_DIM, TH_BG, 1);

    /* 右上角：铃铛图标（有启用的闹钟时高亮，全关时暗） */
    {
        int bx = 232 - 12;
        int by = 25;
        draw_bell_icon(bx, by, enabled > 0 ? TH_ACCENT : TH_PANEL);

        if (tw.time_valid) {
            snprintf(buf, sizeof(buf), "现在 %02d:%02d", tw.hour, tw.minute);
            int w = text_w(buf, 1);
            bsp_display_text(bx - 6 - w, 26, buf, TH_FG, TH_BG, 1);
        }
    }

    /* 标题下的分隔线（2px，规范要求结构线不要细于 2px） */
    bsp_display_fill_rect(0, 44, 240, 45, TH_PANEL);

    /* ---------------- 4 个闹钟 ---------------- */
    static const char *WD = "一二三四五六日";
    int y = 52;
    for (int i = 0; i < ALARM_MAX; i++) {
        const alarm_t *a = &s_alarms[i];
        uint16_t c_num  = a->en ? TH_FG : TH_DIM;
        uint16_t c_time = a->en ? TH_FG : TH_DIM;

        /* ① 序号 */
        snprintf(buf, sizeof(buf), "%d", i + 1);
        bsp_display_text(COL_NUM, y + 8, buf, c_num, TH_BG, 1);

        /* ② 时间：2 倍字号，固定列线 */
        if (a->en) {
            snprintf(buf, sizeof(buf), "%02d:%02d", a->hour, a->min);
        } else {
            snprintf(buf, sizeof(buf), "--:--");
        }
        bsp_display_text(COL_TIME, y, buf, c_time, TH_BG, 2);

        /* ③ 星期：固定列线，垂直居中（行高 32，1 倍字高 16 → y+8） */
        if (a->en) {
            if (a->mask == 0 || (a->mask & 0x7F) == 0x7F) {
                bsp_display_text(COL_WD, y + 8, "每天", TH_INFO, TH_BG, 1);
            } else {
                int days[7];
                int cnt = 0;
                for (int d = 0; d < 7; d++) {
                    if (a->mask & (1 << d)) {
                        days[cnt++] = d;
                    }
                }
                char wd[20];
                int n = 0;
                if (cnt <= 3) {
                    for (int k = 0; k < cnt; k++) {
                        memcpy(wd + n, &WD[days[k] * 3], 3);
                        n += 3;
                    }
                } else {
                    memcpy(wd + n, &WD[days[0] * 3], 3);
                    n += 3;
                    wd[n++] = '~';
                    memcpy(wd + n, &WD[days[cnt - 1] * 3], 3);
                    n += 3;
                }
                wd[n] = '\0';
                bsp_display_text(COL_WD, y + 8, wd, TH_INFO, TH_BG, 1);
            }
        }

        /* ④ 开关：固定列线 */
        bsp_display_text(COL_EN, y + 8, a->en ? "开" : "关",
                         a->en ? TH_OK : TH_DIM, TH_BG, 1);

        y += 34;
    }

    /* ---------------- 分隔线 ---------------- */
    bsp_display_fill_rect(0, 186, 240, 187, TH_PANEL);

    /* ---------------- 倒计时（次要重点，用 2 倍字号） ---------------- */
    int64_t now = esp_timer_get_time() / 1000;
    int64_t left;
    const char *tst;
    uint16_t tc;

    switch (s_timer_state) {
    case 1:
        left = s_timer_end_ms - now;
        if (left < 0) {
            left = 0;
        }
        tst = "运行中";
        tc = (left <= 10000) ? TH_WARN : TH_OK;
        break;
    case 2:
        left = s_timer_left_ms;
        tst = "暂停";
        tc = TH_ACCENT;
        break;
    case 3:
        left = 0;
        tst = "到点";
        tc = TH_WARN;
        break;
    default:
        left = (int64_t) s_timer_min * 60000;
        tst = "待机";
        tc = TH_FG;
        break;
    }

    int sec = (int) ((left + 999) / 1000);
    /* 标签 3 个汉字 = 24px（x=8..32），数字从 x=72 起，中间留 40px 空隙 */
    bsp_display_text(COL_NUM, 201, "倒计时", TH_DIM, TH_BG, 1);
    snprintf(buf, sizeof(buf), "%02d:%02d", sec / 60, sec % 60);
    bsp_display_text(72, 193, buf, tc, TH_BG, 2);
    bsp_display_text(168, 201, tst, tc, TH_BG, 1);
}



/* 复位原因翻译成人话 */
static const char *reset_text(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "上电";
    case ESP_RST_SW:       return "软复位";
    case ESP_RST_PANIC:    return "崩溃";
    case ESP_RST_INT_WDT:  return "中断看门狗";
    case ESP_RST_TASK_WDT: return "任务看门狗";
    case ESP_RST_WDT:      return "看门狗";
    case ESP_RST_DEEPSLEEP:return "深睡唤醒";
    case ESP_RST_BROWNOUT: return "掉电";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "其它";
    }
}

/* 系统信息页：固件、内存、WiFi、账号、模型 */
/* 固件体积：校验一次要 200ms，缓存起来，别每帧都做 */
static uint32_t fw_size_kb(void)
{
    static uint32_t cached = 0;
    static bool done = false;
    if (!done) {
        done = true;
        const esp_partition_t *run = esp_ota_get_running_partition();
        if (run) {
            esp_image_metadata_t meta;
            esp_partition_pos_t pos = { .offset = run->address, .size = run->size };
            if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta) == ESP_OK) {
                cached = meta.image_len / 1024;
            }
        }
    }
    return cached;
}

static void render_sys(void)
{
    char buf[64];
    bsp_display_fill_rect(0, 24, 240, 216, TH_BG);
    bsp_deco_draw_grid();

    app_settings_t cfg;
    config_store_snapshot(&cfg);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_mb = 0;
    esp_flash_get_size(NULL, &flash_mb);
    flash_mb /= (1024 * 1024);

    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();

    /* 当前固件的真实体积（只解析一次，缓存在 fw_size_kb 里） */
    uint32_t fw_kb = fw_size_kb();

    int y = 28;
    snprintf(buf, sizeof(buf), "v%s 正式版", desc ? desc->version : "?");
    y = draw_row(y, "固件", buf, TH_OK);

    snprintf(buf, sizeof(buf), "%.11s %s", desc ? desc->date : "--", FW_AUTHOR);
    y = draw_row(y, "编译", buf, TH_DIM);

    fmt_duration(app_state_uptime_s(), buf, sizeof(buf));
    y = draw_row(y, "运行", buf, TH_INFO);

    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    snprintf(buf, sizeof(buf), "%uK 低%uK",
             (unsigned) (heap_free / 1024), (unsigned) (heap_min / 1024));
    y = draw_row(y, "内存", buf, heap_min < 20 * 1024 ? TH_WARN : TH_OK);

    if (run && fw_kb) {
        snprintf(buf, sizeof(buf), "%uK/%uM %u%%", (unsigned) fw_kb,
                 (unsigned) (run->size / (1024 * 1024)),
                 (unsigned) (fw_kb * 1024 * 100 / run->size));
        y = draw_row(y, "固件占用", buf, TH_FG);
    } else {
        y = draw_row(y, "固件占用", "--", TH_DIM);
    }


    /* 模型 */
    size_t msz = captcha_model_size();
    if (msz > 0) {
        snprintf(buf, sizeof(buf), "%uK 已加载", (unsigned) (msz / 1024));
        y = draw_row(y, "模型", buf, TH_OK);
    } else {
        y = draw_row(y, "模型", "未烧录", TH_WARN);
    }

    y = draw_row(y, "WiFi", wifi_sta_connected() ? wifi_sta_ssid() : "--",
                 wifi_sta_connected() ? TH_FG : TH_DIM);
    y = draw_row(y, "IP", wifi_sta_connected() ? wifi_sta_ip() : "--", TH_FG);

    if (wifi_sta_connected()) {
        int rssi = wifi_sta_rssi();
        snprintf(buf, sizeof(buf), "%d dBm", rssi);
        y = draw_row(y, "信号", buf,
                     rssi > -60 ? TH_OK : (rssi > -75 ? TH_ACCENT : TH_WARN));
    } else {
        y = draw_row(y, "信号", "--", TH_DIM);
    }

    y = draw_row(y, "账号", cfg.user[0] ? cfg.user : "未配置", TH_FG);

    /* 底部：复位原因 + 提示 */
    snprintf(buf, sizeof(buf), "%s", reset_text(esp_reset_reason()));
    bsp_display_text(6, 216, buf, TH_DIM, TH_BG, 1);
    bsp_display_text(150, 216, "BOOT 重试", TH_DIM, TH_BG, 1);
}

/* UTF-8 字符字节数（只处理 1/2/3/4 字节常见情况） */
static int utf8_bytes(unsigned char c)
{
    if ((c & 0x80) == 0x00) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    return 4;
}

/* 按"显示宽度"切块：ASCII 宽 1，中文宽 2；最多切成 max_chunk 块。
   返回实际块数，块内容写进 out（每块最多 cols 个显示位，'\0' 结尾） */
static int utf8_chunk(const char *s, int cols, char out[][64], int max_chunk)
{
    int n = 0;                 /* 已产生的块数 */
    int col = 0;
    int len = 0;
    out[0][0] = '\0';

    while (*s != '\0' && n < max_chunk) {
        int nb = utf8_bytes((unsigned char) *s);
        int w = (nb >= 3) ? 2 : 1;      /* 中文/全角算 2 位 */

        if (col + w > cols) {
            out[n][len] = '\0';
            n++;
            if (n >= max_chunk) {
                break;
            }
            col = 0;
            len = 0;
            out[n][0] = '\0';
        }
        for (int i = 0; i < nb && s[i] != '\0'; i++) {
            if (len < 63) {
                out[n][len++] = s[i];
            }
        }
        col += w;
        s += nb;
    }
    if (n < max_chunk) {
        out[n][len] = '\0';
        n++;
    }
    return n;
}

static void render_diag(void)
{
    bsp_display_fill_rect(0, 24, 240, 216, TH_BG);
    bsp_deco_draw_grid();

    /* ---------- 标题栏 ---------- */
    bsp_display_fill_rect(0, 24, 240, 20, TH_PANEL);
    bsp_display_text(8, 27, "板载日志", TH_FG, TH_PANEL, 1);
    uint16_t stc = TH_FG;
    const char *st = state_text(s_state, &stc);
    int sw = bsp_display_text_width(st, 1);
    bsp_display_text(232 - sw, 27, st, stc, TH_PANEL, 1);

    /* ---------- 日志面板 ---------- */
    const int panel_y = 46;
    const int row_h = 16;
    const int rows = 10;                 /* 面板能放的行数 */
    bsp_display_fill_rect(0, panel_y, 240, row_h * rows + 4, 0x0841);
    bsp_display_fill_rect(0, panel_y, 240, 1, 0x2945);

    EXT_RAM_BSS_ATTR static char logbuf[4096];
    logring_tail(logbuf, sizeof(logbuf), 10);

    /* 切出日志行指针 */
    const char *ln[12];
    int cnt = 0;
    const char *p = logbuf;
    while (*p != '\0' && cnt < 12) {
        const char *nl = strchr(p, '\n');
        if (nl == NULL) {
            /* 这一行没有换行符：说明是环形缓冲被覆盖后剩下的残行，
               丢掉，否则会和下一条日志粘在一起 */
            break;
        }
        /* 只收"看起来完整"的行：至少有 "I (" 这样的前缀 */
        if ((nl - p) >= 6 && (p[0] == 'I' || p[0] == 'W' || p[0] == 'E')) {
            ln[cnt++] = p;
        }
        p = nl + 1;
    }

    const int per_entry = 2;             /* 每条日志最多占 2 行 */
    int max_entry = rows / per_entry;    /* 10/2 = 5 条 */
    int first = (cnt > max_entry) ? (cnt - max_entry) : 0;

    int64_t nowm = esp_timer_get_time() / 1000;
    int y = panel_y + 2;

    for (int i = first; i < cnt; i++) {
        const char *raw = ln[i];

        /* 级别 + 色块颜色 */
        const char *s = raw;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        char lv = (*s == 'E' || *s == 'W' || *s == 'I') ? *s : 'I';
        uint16_t chip = (lv == 'E') ? TH_WARN
                      : (lv == 'W') ? TH_ACCENT : TH_OK;

        /* 去掉 "I (12345) " 前缀 */
        const char *msg = s;
        const char *paren = strchr(s, ')');
        if (paren != NULL && paren < s + 20) {
            msg = paren + 1;
            while (*msg == ' ') {
                msg++;
            }
        }

        /* 压缩空格，提高信息密度 */
        static char compact[192];
        int ci = 0;
        bool last_space = false;
        for (const char *q = msg; *q != '\0' && ci < (int) sizeof(compact) - 1; q++) {
            char c = *q;
            /* 跳过控制字符（\r 是串口换行留下的，显示出来会变成 '?'） */
            if ((unsigned char) c < 0x20) {
                continue;
            }
            if (c == ' ' || c == '\t' || c == ':') {
                if (!last_space) {
                    compact[ci++] = ' ';
                    last_space = true;
                }
            } else {
                compact[ci++] = c;
                last_space = false;
            }
        }
        compact[ci] = '\0';

        /* 切成 28 显示位的块 */
        static char chunks[8][64];
        int nc = utf8_chunk(compact, 28, chunks, 8);

        /* 超过 2 块就滚动：每 700ms 前进一块 */
        int win = 0;
        if (nc > per_entry) {
            win = (int) ((nowm / 700) % (nc - per_entry + 1));
        }

        /* 颜色：越旧越暗 */
        int age = cnt - 1 - i;
        uint16_t fg = (age == 0) ? TH_FG
                    : (age <= 2) ? 0xCE79
                    : (age <= 4) ? 0x8C71 : 0x630C;

        for (int r = 0; r < per_entry && (win + r) < nc; r++) {
            if (r == 0) {
                /* 第一行：彩色级别块 */
                bsp_display_fill_rect(3, y + 6, 3, 5, chip);
            } else {
                /* 续行：暗色竖线，提示"接上一条" */
                bsp_display_fill_rect(4, y + 2, 1, 12, 0x4208);
            }
            /* 正文两行都从 x=10 开始，纵向对齐 */
            bsp_display_text(10, y, chunks[win + r], fg, 0x0841, 1);
            y += row_h;
        }
        if (y > panel_y + row_h * rows - row_h) {
            break;
        }
    }

    if (cnt == 0) {
        bsp_display_text(10, panel_y + 8, "暂无日志", TH_DIM, 0x0841, 1);
    }

    /* ---------- 底部：内存 ---------- */
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t big_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    char foot[48];
    snprintf(foot, sizeof(foot), "内存 %uK  最大块 %uK",
             (unsigned) (free_int / 1024), (unsigned) (big_int / 1024));
    bsp_display_text(8, 214, foot, big_int > 20000 ? 0x5AEB : TH_ACCENT, TH_BG, 1);
}


static void draw_weather_icon(int ox, int oy, int code)
{
    enum { S = 24 };
    static uint16_t buf[S * S];
    const uint16_t SUN = 0xFE60, CLOUD = 0xCE79, RAIN = 0x4D5F, SNOW = 0xE71C;

    bool rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code >= 95;
    bool snow = (code >= 71 && code <= 77);
    bool cloudy = (code >= 2 && code <= 48) || rain || snow;
    bool sun = (code <= 1);

    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            uint16_t c = TH_BG;
            if (sun) {
                int dx = x - 12, dy = y - 12;
                if (dx * dx + dy * dy <= 36) c = SUN;
                else if ((x == 12 || y == 12 || abs(dx) == abs(dy)) &&
                         (abs(dx) > 8 || abs(dy) > 8) &&
                         (dx * dx + dy * dy < 121)) c = SUN;
            }
            if (cloudy) {
                int cx1 = x - 9, cy1 = y - 14;
                int cx2 = x - 15, cy2 = y - 11;
                bool body = (y >= 12 && y <= 18 && x >= 4 && x <= 20);
                if (cx1 * cx1 + cy1 * cy1 <= 16 || cx2 * cx2 + cy2 * cy2 <= 9 || body) c = CLOUD;
            }
            if (rain && y >= 19 && y <= 22 && (x % 5) == 2) c = RAIN;
            if (snow && y >= 19 && y <= 22 && (x % 6) == 3) c = SNOW;
            buf[y * S + x] = c;
        }
    }
    bsp_display_blit(ox, oy, S, S, buf);
}

/* 时钟页底部的声音条：麦克风实时音量 + 跑马灯 */



static void render_clock(void)
{
    char buf[64];
    time_weather_t tw;
    time_weather_get(&tw);

    bool has_bg = bg_has_image();

    bool bg_drawn = false;
    if (s_clock_full) {
        if (has_bg) {
            bg_draw();
            bg_drawn = true;
        } else {
            bsp_display_fill_rect(0, 0, 240, 240, TH_BG);
            bsp_deco_draw_grid();
        }
        bsp_deco_draw_grid();      /* 网格装饰（可关）：画在内容之前 */
        s_clock_full = false;
        s_clock_minute = -1;
        s_clock_second = -999;      /* 进页面强制整页画一次 */
    }

    int second_of_day = tw.time_valid ? (tw.hour * 3600 + tw.minute * 60 + tw.second) : -2;
    uint16_t bg_color = has_bg ? COLOR_BLACK : TH_BG;

    if (tw.time_valid && second_of_day == s_clock_second) {
        return;                                     /* 时间没变，什么都不用重画 */
    }
    s_clock_second = second_of_day;

    /* 有背景图：不垫黑条，每分钟整张底图重画一次，文字直接压在图上（透明底） */
    int bar_y = has_bg ? 148 : 74;
    if (has_bg) {
        if (!bg_drawn) {
            bg_draw();
        }
    } else {
        bsp_display_fill_rect(0, bar_y, 240, 240 - bar_y, bg_color);
    }
    int text_bg = has_bg ? -1 : (int) bg_color;

    if (tw.time_valid) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tw.hour, tw.minute, tw.second);
        int w = text_w(buf, 3);
        int ty = bar_y + (has_bg ? 4 : 10);
        int tx = (240 - w) / 2;
        if (has_bg) {
            /* 透明底 + 1px 黑色描边：压在亮背景上也看得清 */
            bsp_display_text(tx + 2, ty + 2, buf, COLOR_BLACK, -1, 3);
        }
        bsp_display_text(tx, ty, buf, TH_FG, text_bg, 3);

        static const char *wd[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        const char *day = (tw.weekday >= 0 && tw.weekday < 7) ? wd[tw.weekday] : "";

        cal_day_t cd;
        cal_lookup(tw.year, tw.month, tw.day, &cd);
        app_settings_t bcfg;
        config_store_snapshot(&bcfg);
        int bbm = (bcfg.birthday / 100) % 100;
        int bbd = bcfg.birthday % 100;
        bool today_bday = (bbm == tw.month && bbd == tw.day && bbd > 0);
        if (today_bday) {
            snprintf(buf, sizeof(buf), "%d月%d日 %s 生日快乐", tw.month, tw.day, day);
        } else if (cd.name != NULL) {
            snprintf(buf, sizeof(buf), "%d月%d日 %s %s", tw.month, tw.day, day, cd.name);
        } else {
            snprintf(buf, sizeof(buf), "%d月%d日 %s", tw.month, tw.day, day);
        }
        if (has_bg) {
            int dw = text_w(buf, 1);
            bsp_display_text((240 - dw) / 2 + 1, bar_y + 57, buf, COLOR_BLACK, -1, 1);
        }
        bsp_display_text_center(bar_y + (has_bg ? 56 : 62), buf, TH_DIM, text_bg, 1);
    } else {
        bsp_display_text_center(bar_y + 20, "对时中", TH_ACCENT, text_bg, 2);
    }

    /* 顶部 4 像素细条显示门户认证状态：绿=已认证 黄=登录中 红=失败 灰=未连接 */
    {
        uint16_t sc;
        state_text(s_state, &sc);
        bsp_display_fill_rect(0, 0, 240, 4, sc);
    }

    if (tw.weather_valid) {
        draw_weather_icon(6, 18, tw.code);
        snprintf(buf, sizeof(buf), "%s %dC", tw.desc, tw.temp_c);
        if (has_bg) {
            bsp_display_text(6 + 26 + 1, 22 + 1, buf, COLOR_BLACK, -1, 1);
        }
        bsp_display_text(6 + 26, 22, buf, TH_FG, has_bg ? -1 : COLOR_BLACK, 1);
    }

    if (!has_bg) {
        bsp_display_text_center(164, "网页上传图片可换背景", TH_DIM, bg_color, 1);
    }

}


/* -------------------------------------------------------------- 日历屏 */

/* 同一天内容不变，只有翻天才重画 */
static int s_cal_day = -1;

static void render_calendar(void)
{
    time_weather_t tw;
    time_weather_get(&tw);

    if (!tw.time_valid || tw.year < 2024) {
        bsp_display_fill_rect(0, 24, 240, 216, TH_BG);
        bsp_deco_draw_grid();
        bsp_display_text_center(110, "对时后显示日历", TH_ACCENT, TH_BG, 1);
        return;
    }

    /* 用 年月日 拼成 key，翻天才重画 */
    int key = tw.year * 10000 + tw.month * 100 + tw.day;
    if (key == s_cal_day) {
        return;
    }
    s_cal_day = key;

    char buf[64];

    bsp_display_fill_rect(0, 24, 240, 216, TH_BG);
    bsp_deco_draw_grid();

    /* 月份标题 + 农历月份 */
    lunar_date_t ld;
    lunar_from_solar(tw.year, tw.month, tw.day, &ld);
    if (ld.valid) {
        snprintf(buf, sizeof(buf), "%d年%d月  农历%s", tw.year, tw.month,
                 lunar_month_name(ld.month));
    } else {
        snprintf(buf, sizeof(buf), "%d年%d月", tw.year, tw.month);
    }
    bsp_display_text_center(30, buf, TH_FG, TH_BG, 1);

    /* 星期表头 */
    static const char *wd[] = {"日", "一", "二", "三", "四", "五", "六"};
    const int cell_w = 34, row_h = 22, x0 = 1, y0 = 72;

    for (int i = 0; i < 7; i++) {
        int cx = x0 + i * cell_w + (cell_w - 16) / 2;
        bsp_display_text(cx, 52, wd[i], TH_DIM, TH_BG, 1);
    }

    app_settings_t cal_cfg;
    config_store_snapshot(&cal_cfg);
    int cal_bm = (cal_cfg.birthday / 100) % 100;
    int cal_bd = cal_cfg.birthday % 100;
    bool bday_set = (cal_bm >= 1 && cal_bm <= 12 && cal_bd >= 1 && cal_bd <= 31);

    int first = cal_first_weekday(tw.year, tw.month);
    int days = cal_days_in_month(tw.year, tw.month);

    for (int d = 1; d <= days; d++) {
        int idx = first + d - 1;
        int col = idx % 7, row = idx / 7;
        int cx = x0 + col * cell_w;
        int cy = y0 + row * row_h;

        cal_day_t cd;
        cal_lookup(tw.year, tw.month, d, &cd);

        uint16_t fg = TH_FG;
        if (cd.flags & CAL_FLAG_OFF) {
            fg = TH_WARN;                 /* 法定放假 */
        } else if (cd.flags & CAL_FLAG_WORK) {
            fg = COLOR_ORANGE;              /* 调休上班 */
        } else if (cd.flags & CAL_FLAG_TRAD) {
            fg = TH_INFO;                /* 传统节日 */
        }

        snprintf(buf, sizeof(buf), "%d", d);
        int w = text_w(buf, 1);

        bool this_bday = bday_set && (tw.month == cal_bm && d == cal_bd);
        if (this_bday && d != tw.day) {
            /* 生日那天：红字 + 下面一个小红点 */
            bsp_display_text(cx + (cell_w - w) / 2, cy + 3, buf, TH_WARN, TH_BG, 1);
            int dotx = cx + cell_w / 2 - 2;
            bsp_display_fill_rect(dotx, cy + row_h - 6, 4, 3, TH_WARN);
            continue;
        }

        if (d == tw.day) {
            bsp_display_fill_rect(cx + 2, cy, cell_w - 4, row_h - 2, TH_INFO);
            bsp_display_text(cx + (cell_w - w) / 2, cy + 3, buf,
                             COLOR_BLACK, TH_INFO, 1);
        } else {
            bsp_display_text(cx + (cell_w - w) / 2, cy + 3, buf, fg, TH_BG, 1);
        }
    }

    /* 今天是什么日子 + 下一个假期 */
    cal_day_t today;
    cal_lookup(tw.year, tw.month, tw.day, &today);

    int bm = cal_bm;
    int bd = cal_bd;
    bool is_bday = bday_set && (tw.month == bm && tw.day == bd);

    const char *term = solar_term_of(tw.year, tw.month, tw.day);
    if (is_bday) {
        bsp_display_text(8, 202, "今天 生日快乐！", TH_WARN, TH_BG, 1);
    } else if (today.name != NULL) {
        snprintf(buf, sizeof(buf), "今天 %s", today.name);
        bsp_display_text(8, 202, buf, TH_ACCENT, TH_BG, 1);
    } else if (term != NULL) {
        snprintf(buf, sizeof(buf), "今天 %s", term);
        bsp_display_text(8, 202, buf, TH_INFO, TH_BG, 1);
    } else if (ld.valid) {
        snprintf(buf, sizeof(buf), "今天 农历%s", lunar_day_name(ld.day));
        bsp_display_text(8, 202, buf, TH_DIM, TH_BG, 1);
    } else {
        bsp_display_text(8, 202, "今天", TH_DIM, TH_BG, 1);
    }

    /* 生日倒计时（60 天内优先显示），否则显示下一个假期 */
    int bdays = -1;
    if (bday_set) {
        int y = tw.year;
        bool passed = (tw.month > bm) || (tw.month == bm && tw.day > bd);
        if (passed) {
            y++;
        }
        struct tm a = {0}, b = {0};
        a.tm_year = tw.year - 1900; a.tm_mon = tw.month - 1; a.tm_mday = tw.day;
        b.tm_year = y - 1900; b.tm_mon = bm - 1; b.tm_mday = bd;
        time_t ta = mktime(&a), tb = mktime(&b);
        if (ta != (time_t) -1 && tb != (time_t) -1) {
            bdays = (int) ((tb - ta) / 86400);
        }
    }

    if (is_bday) {
        bsp_display_text(8, 220, "生日快乐，今天你是主角", TH_DIM, TH_BG, 1);
    } else if (bdays >= 0 && bdays <= 60) {
        snprintf(buf, sizeof(buf), "距生日 %d/%d 还有 %d 天", bm, bd, bdays);
        bsp_display_text(8, 220, buf, TH_INFO, TH_BG, 1);
    } else {
        int ahead = 0;
        const char *hname = NULL;
        if (cal_next_holiday(tw.year, tw.month, tw.day, &ahead, &hname)) {
            if (ahead == 0) {
                snprintf(buf, sizeof(buf), "%s 放假中", hname);
            } else {
                snprintf(buf, sizeof(buf), "距%s %d 天", hname, ahead);
            }
            bsp_display_text(8, 220, buf, TH_DIM, TH_BG, 1);
        }
    }
}

/* ------------------------------------------------------------ 验证码界面 */

static void render_captcha(const portal_captcha_t *cap)
{
    char buf[64];

    bsp_display_clear(TH_BG);
    bsp_display_fill_rect(0, 0, 240, 24, TH_ACCENT);
    bsp_display_text(6, 4, "验证码", COLOR_BLACK, TH_ACCENT, 1);

    /* 62x25 放大 3 倍 = 186x75，居中 */
    int scale = 3;
    int w = cap->w * scale;
    int h = cap->h * scale;
    int x = (240 - w) / 2;
    int y = 44;

    bsp_display_fill_rect(x - 4, y - 4, w + 8, h + 8, TH_FG);
    bsp_display_image(x, y, cap->w, cap->h, cap->rgb, scale, false);

    snprintf(buf, sizeof(buf), "识别为 %s", cap->code);
    bsp_display_text(8, y + h + 24, buf, TH_FG, TH_BG, 2);

    bsp_display_text(8, y + h + 60, "识别错了就按 BOOT", TH_DIM, TH_BG, 1);
    bsp_display_text(8, 210, "3 秒后自动提交", TH_DIM, TH_BG, 1);
}

/* 验证码回调：显示 + 给用户 3 秒反悔时间 */
static bool on_captcha(const portal_captcha_t *cap, void *user)
{
    (void) user;

    s_screen = SC_CAPTCHA;
    s_boot_pressed = 0;

    render_captcha(cap);
    bsp_display_flush();

    for (int i = 0; i < 30; i++) {
        if (s_boot_pressed) {
            s_boot_pressed = 0;
            s_screen = SC_STATUS;
            set_state(ST_LOGIN, "换一张验证码");
            ESP_LOGW(TAG, "用户放弃这张验证码");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    s_screen = SC_STATUS;
    return true;
}

/* ------------------------------------------------------------ 诊断测试屏 */

#if DIAG_SCREEN
static void render_diag_test(void)
{
    bsp_display_fill_rect(0, 0, 240, 240, COLOR_BLACK);

    /* 1) 四个纯色方块：验证颜色映射 */
    bsp_display_fill_rect(10, 8, 52, 52, TH_WARN);
    bsp_display_fill_rect(70, 8, 52, 52, TH_OK);
    bsp_display_fill_rect(130, 8, 52, 52, COLOR_BLUE);
    bsp_display_fill_rect(190, 8, 52, 52, TH_FG);

    /* 2) 超大字符：验证字形 */
    bsp_display_text(10, 70, "风", TH_FG, COLOR_BLACK, 3);
    bsp_display_text(70, 70, "电", TH_FG, COLOR_BLACK, 3);
    bsp_display_text(130, 70, "A", TH_FG, COLOR_BLACK, 3);
    bsp_display_text(170, 70, "1", TH_FG, COLOR_BLACK, 3);

    /* 3) 普通字号的整行文字 */
    bsp_display_text(6, 126, "ABC abc 123 45%", TH_FG, COLOR_BLACK, 1);
    bsp_display_text(6, 148, "中文测试风扇灯", TH_INFO, COLOR_BLACK, 1);
    bsp_display_text(6, 170, "开关亮度定时", TH_ACCENT, COLOR_BLACK, 1);

    /* 4) 边框 */
    bsp_display_rect(4, 120, 232, 74, TH_DIM);

    bsp_display_text(6, 212, "DIAG-2", TH_DIM, COLOR_BLACK, 1);

    extern volatile uint32_t g_flush_ok, g_flush_timeout;
    static int64_t last_log;
    int64_t now = esp_timer_get_time() / 1000;
    if (now - last_log > 2000) {
        last_log = now;
        ESP_LOGI("diag", "flush ok=%lu timeout=%lu heap=%u",
                 (unsigned long) g_flush_ok, (unsigned long) g_flush_timeout,
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}
#endif

/* -------------------------------------------------------------- 界面任务 */

static void poll_longpress(void);
static void on_button(bsp_button_t btn, void *user);

static void ble_idle_release(void);

/* 开机 LOGO：品牌 + 版本 + 作者 */
static void draw_boot_logo(void)
{
    const esp_app_desc_t *ad = esp_app_get_description();
    bsp_display_clear(TH_BG);
    bsp_display_fill_rect(0, 0, 240, 4, TH_INFO);
    int lw = text_w("YING", 5);
    bsp_display_text((240 - lw) / 2, 76, "YING", TH_INFO, TH_BG, 5);
    char line[48];
    snprintf(line, sizeof(line), "YingWang v%s", ad ? ad->version : "?");
    bsp_display_text_center(140, line, TH_DIM, TH_BG, 1);
    bsp_display_text_center(160, "by YING", TH_DIM, TH_BG, 1);
    bsp_display_flush();
}

/* 串口调试用：把开机 LOGO 画出来并冻结界面，方便截图。
   3 秒后自动恢复，免得忘了 SPLASH 0 把界面冻死。 */
static esp_timer_handle_t s_logo_tmo = NULL;

static void logo_restore_cb(void *arg)
{
    (void) arg;
    s_ui_freeze = false;
    s_force_redraw = true;
}

void ui_show_logo(bool on)
{
    if (on) {
        draw_boot_logo();
        s_ui_freeze = true;
        if (s_logo_tmo == NULL) {
            const esp_timer_create_args_t args = {
                .callback = logo_restore_cb,
                .name = "logo_tmo",
            };
            esp_timer_create(&args, &s_logo_tmo);
        }
        if (s_logo_tmo) {
            esp_timer_stop(s_logo_tmo);
            esp_timer_start_once(s_logo_tmo, 3000LL * 1000);
        }
    } else {
        if (s_logo_tmo) {
            esp_timer_stop(s_logo_tmo);
        }
        s_ui_freeze = false;
        s_force_redraw = true;
    }
}

/* 串口调试用：切到指定页面 */
/* 截图期间冻结界面，避免抓出撕裂的帧 */
void ui_freeze(bool on)
{
    s_ui_freeze = on;
}

/* 串口/网页远程按键：只投递请求，由 UI 任务在它自己的上下文里执行，
   避免和按键任务/UI 任务并发改同一批状态 */
static volatile int s_pending_key = -1;
static TaskHandle_t s_ui_task_h = NULL;
static volatile int64_t s_key_at_us = 0;   /* 按键投递时刻，用来量响应延迟 */   /* 按键来了立刻叫醒界面任务 */

void ui_press(int which)
{
    if (which < 0 || which >= BSP_BTN_COUNT) {
        return;
    }
    s_pending_key = which;
    s_key_at_us = esp_timer_get_time();
    if (s_ui_task_h != NULL) {
        xTaskNotifyGive(s_ui_task_h);      /* 立刻处理，不用等下一轮 */
    }
}

/* 串口/网页切到倒计时或秒表页：0=倒计时 1=秒表 */
void ui_timer_mode(int sw)
{
    s_screen = sw ? SC_STOPWATCH : SC_TIMER;
    s_force_redraw = true;
    s_clock_full = true;
    s_cal_day = -1;
    s_ui_freeze = false;
}

/* 串口测试用：手动触发一次"环境很吵"提醒 */
void ui_noise_test(void)
{
    s_noise_alert = true;
    s_noise_until_ms = esp_timer_get_time() / 1000 + NOISE_SHOW_MS;
}

void ui_goto(int idx)
{
    /* 允许串口直接跳到模型页（9），但验证码页不能手动切 */
    if (idx < 0 || idx >= SC_COUNT || idx == SC_CAPTCHA) {
        return;
    }
    s_screen = idx;
    s_force_redraw = true;
    s_clock_full = true;
    s_cal_day = -1;
    s_ui_freeze = false;      /* 切页时顺手解冻，避免截图后卡在冻结状态 */
}
static void ble_exclusive_begin(void);
static void ble_exclusive_end(void);
static void esl_finish_task(void *arg);

/* 按键推送倒计时：到点了真正开始推 */
static void esl_push_tick(void)
{
    if (s_esl_push_at_ms == 0) {
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (now < s_esl_push_at_ms) {
        s_force_redraw = true;          /* 让界面刷新倒计时数字 */
        return;
    }
    s_esl_push_at_ms = 0;
    bool test_card = s_esl_push_test_card;
    s_esl_push_test_card = false;
    s_force_redraw = true;

    esl_state_t es = esl_ble_state();
    if (!(es == ESL_IDLE || es == ESL_DONE || es == ESL_ERROR)) {
        ESP_LOGW(TAG, "价签正忙：%s，这次不推", esl_ble_status());
        return;
    }
    ESP_LOGI(TAG, "倒计时结束，开始推送%s", test_card ? "测试卡" : "信息页");
    ble_exclusive_begin();
    if (esl_ble_push_manual(!test_card) == ESP_OK) {
        xTaskCreate(esl_finish_task, "eslfin", 3072, NULL, 2, NULL);
    } else {
        ble_exclusive_end();
    }
}

/* ---------------- 外观自动化 ---------------- */

static int s_last_bg_bucket = -1;
static bool s_last_is_day = true;

/* 从 "HH:MM" 里取小时，失败返回 -1 */
static int hh_of(const char *s)
{
    if (!s || !s[0]) {
        return -1;
    }
    int h = 0;
    if (sscanf(s, "%d", &h) != 1) {
        return -1;
    }
    return (h >= 0 && h < 24) ? h : -1;
}

/* 该用白天还是夜晚主题：优先用天气里的日出日落，没有就按 7~19 点 */
static bool theme_is_day(void)
{
    time_weather_t tw;
    memset(&tw, 0, sizeof(tw));
    time_weather_get(&tw);
    if (tw.time_valid) {
        int sh = hh_of(tw.sunrise);
        int eh = hh_of(tw.sunset);
        if (sh >= 0 && eh >= 0 && eh > sh) {
            int h = tw.hour;
            return h >= sh && h < eh;
        }
        return tw.hour >= 7 && tw.hour < 19;
    }
    return true;
}

static void apply_auto_theme(void)
{
    const app_settings_t *cfg = config_store_get();
    if (!cfg->theme_auto) {
        return;
    }
    bool day = theme_is_day();
    if (day == s_last_is_day) {
        return;
    }
    s_last_is_day = day;
    int want = day ? cfg->theme_day : cfg->theme_night;
    if (want < 0 || want >= BSP_THEME_COUNT) {
        want = 0;
    }
    bsp_theme_load_preset(want);
    bsp_theme_ensure_readable(bg_has_image());
    ESP_LOGI("app", "自动切换主题：%s -> %s", day ? "白天" : "夜晚",
             bsp_theme_name(want) ? bsp_theme_name(want) : "?");
    s_force_redraw = true;
}

/* 背景图轮换：每小时 / 每天在两张图之间换 */
static void apply_bg_rotate(void)
{
    const app_settings_t *cfg = config_store_get();
    if (!cfg->bg_rotate || !bg_has_image_at(0) || !bg_has_image_at(1)) {
        return;
    }
    time_weather_t tw;
    memset(&tw, 0, sizeof(tw));
    time_weather_get(&tw);
    if (!tw.time_valid) {
        return;
    }
    int bucket = (cfg->bg_rotate == 1) ? (tw.day * 24 + tw.hour) : tw.day;
    if (bucket == s_last_bg_bucket) {
        return;
    }
    s_last_bg_bucket = bucket;
    int slot = bucket % 2;
    if (slot != bg_slot()) {
        bg_set_slot(slot);
        ESP_LOGI("app", "背景图轮换到槽 %d", slot);
        s_force_redraw = true;
    }
}

static void ui_task(void *arg)
{
    (void) arg;
    s_ui_task_h = xTaskGetCurrentTaskHandle();
    int last_screen = -1;
    int64_t last_mem_log = 0;
    int64_t last_anti_stick = 0;

    while (true) {
        int sc = s_screen;

        if (sc == SC_CAPTCHA) {
            /* 验证码界面由回调自己画，这里不动 */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        poll_longpress();
        esl_push_tick();

        ble_idle_release();

        pm_tick();

        int64_t tick_now = esp_timer_get_time() / 1000;

        /* 内存体检：开机先来一行，之后每分钟一行，方便看蓝牙起来后还剩多少。
           顺带打刷屏计数：某个 16 行的带如果 DMA 超时没刷上去，屏幕上就会留下
           上一页的字（帧缓冲是对的，所以截图看不出来）。 */
        if (last_mem_log == 0 || tick_now - last_mem_log >= 60000) {
            last_mem_log = tick_now;
            extern volatile uint32_t g_flush_ok, g_flush_timeout;
            ESP_LOGI(TAG, "内存：内部 %uK/%uK（最大块 %uK） PSRAM %uK 历史最低 %uK | BLE=%s | 刷屏 ok=%lu 超时=%lu",
                     (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned) (heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned) (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                     (unsigned) (heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     esl_ble_stack_up() ? "开" : "关",
                     (unsigned long) g_flush_ok, (unsigned long) g_flush_timeout);
        }

        /* 残影对策②：亮屏时每 30 分钟整屏翻一次 黑 → 白 → 黑 再重画。
           IPS 面板长时间显示同一幅高对比画面会留下"鬼影"（切页也不消失），
           定期把整屏像素反向充一次电能明显减轻。0 = 关闭。 */
        if (ANTI_STICK_MIN > 0 && (!s_screen_off || s_aod) &&
            tick_now - last_anti_stick >= (int64_t) ANTI_STICK_MIN * 60 * 1000) {
            last_anti_stick = tick_now;
            bsp_display_clear(COLOR_BLACK);
            bsp_display_flush();
            vTaskDelay(pdMS_TO_TICKS(120));
            bsp_display_clear(TH_FG);
            bsp_display_flush();
            vTaskDelay(pdMS_TO_TICKS(120));
            s_force_redraw = true;      /* 下一轮把当前页整页重画 */
            ESP_LOGI(TAG, "防残影：整屏翻转刷新一次");
        }

        /* 倒计时到点：响铃 + 自动跳回倒计时页，屏幕闪红 */
        if (s_timer_state == 1 && tick_now >= s_timer_end_ms) {
            s_timer_state = 3;
            s_timer_left_ms = 0;
            beep_alarm_start();
            if (s_screen != SC_TIMER) {
                s_screen = SC_TIMER;
                s_force_redraw = true;      /* 从别的页跳回来，整屏重画 */
            }
            ESP_LOGW(TAG, "倒计时结束：响铃并跳回倒计时页");
        }

        if (s_ui_freeze) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* 声控提醒已按需求关闭（麦克风改为按需使用） */

        /* 网页/串口的远程按键：在这里执行，和真实按键同一上下文 */
        int pk = s_pending_key;
        if (pk >= 0) {
            s_pending_key = -1;
            if (s_key_at_us > 0) {
                int64_t lat = (esp_timer_get_time() - s_key_at_us) / 1000;
                ESP_LOGI(TAG, "按键响应 %lld ms", (long long) lat);
                s_key_at_us = 0;
            }
            on_button((bsp_button_t) pk, NULL);
        }

        /* 熄屏了就别画了：背光关着，画了也看不见，白耗 SPI 和 CPU。
           低亮度常显（s_aod）例外 —— 那种模式屏幕是亮的，要接着刷新时钟/数据。 */
        if (s_screen_off && !s_aod) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (s_force_redraw) {
            s_force_redraw = false;
            last_screen = -1;          /* 让下面整屏重画一次 */
            s_clock_full = true;
            s_cal_day = -1;
        }

#if DIAG_SCREEN
        render_diag_test();
        /* 不在诊断页超过 60 秒就把麦克风关掉（音频任务自己会停） */
        if (s_audio_on && sc != SC_DIAG && now_ms() - s_audio_idle_ms > 60000) {
            s_audio_on = false;
            audio_level_pause(true);
            ESP_LOGI(TAG, "离开诊断页 60 秒，停掉麦克风");
        }

        bsp_display_flush();
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
#endif

        if (sc != last_screen) {
            bsp_display_clear(TH_BG);
            s_clock_full = true;      /* 时钟屏整屏重画一次 */
            s_cal_day = -1;           /* 日历屏也重画 */
            last_screen = sc;
        }

        switch (sc) {
        case SC_STATUS:
            draw_header("状态", s_state);
            render_status();
            break;
        case SC_INFO:
            draw_header("信息", s_state);
            render_info();
            break;
        case SC_CLOCK:
            render_clock();           /* 全屏背景，不画标题条 */
            break;
        case SC_CALENDAR:
            draw_header("日历", s_state);
            render_calendar();
            break;
        case SC_DIAG:
            /* 诊断页只显示日志，不开麦克风（声音条已去掉）。
               录音功能需要时会自己初始化麦克风。 */
            draw_header("诊断", s_state);
            render_diag();
            break;
        case SC_SYS:
            draw_header("系统", s_state);
            render_sys();
            break;
        case SC_ESL:
            draw_header("价签", s_state);
            render_esl();
            break;
        case SC_TIMER:
            draw_header("倒计时", s_state);
            render_timer();
            break;
        case SC_STOPWATCH:
            draw_header("秒表", s_state);
            render_stopwatch();
            break;
        case SC_ALARMS:
            draw_header("作息", s_state);
            render_alarms();
            break;
        case SC_ALARM:
            draw_header("闹钟", s_state);
            render_alarm();
            break;
        case SC_MODEL:
            draw_header("模型", s_state);
            render_model();
            break;
        default:
            break;
        }

        apply_auto_theme();
        apply_bg_rotate();
        bsp_deco_draw();          /* 边框 / 角标图标 / 角标文字（可关） */
        bsp_display_flush();

        /* 倒计时/秒表要走到秒、时钟要跟手，其余页面可以慢一点；
           用 ulTaskNotifyTake：按键/长按立刻叫醒，不用等满一个周期 */
        alarm_tick();          /* 每秒查一次闹钟 */

        uint32_t wait_ms = (sc == SC_TIMER || sc == SC_STOPWATCH) ? 200
                           : (sc == SC_CLOCK ? 150
                           : (sc == SC_INFO ? 300 : 500));
        if (s_aod) {
            wait_ms = 1000;            /* 低亮度常显：一秒刷一次足够，省点 CPU/SPI */
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

/* ------------------------------------------------------------------ 按键 */

/* ------------------------------------------------------------ 电源管理
 *
 * 板子没有电池电压 ADC（只有充电检测脚），所以做不了电量百分比。
 * 能省的电都在"屏幕"和"WiFi"上：
 *   - 背光：无操作 N 分钟后关掉，任意键唤醒（这块屏常亮是大头，20~40mA）
 *   - WiFi：息屏时进 WIFI_PS_MAX_MODEM，亮屏回 MIN_MODEM
 *
 * 不做深睡：设备要 7x24 在线认证，睡了就没人守门户了。
 */
#define PM_PS_DIM_MS  (APP_PS_DIM_WHEN_IDLE ? 1 : 0)


static int64_t now_ms(void)
{
    return (int64_t) (esp_timer_get_time() / 1000);
}

static void pm_wake(void)
{
    s_dimmed = false;
    const app_settings_t *c = config_store_get();
    bsp_display_backlight(c->brightness > 0 ? c->brightness : 80);
    if (s_screen_off) {
        s_screen_off = false;
        s_aod = false;                 /* 退出低亮度常显 */
        s_force_redraw = true;         /* 背光关着的时候画面是旧的，唤醒要重画 */
        ESP_LOGI(TAG, "唤醒屏幕");
    }
    s_last_input_ms = now_ms();
    if (PM_PS_DIM_MS) {
        esp_wifi_set_ps(WIFI_PS_NONE);   /* USB 供电，不省电，保证网页秒开 */
    }
}

/** 定期调用：判断该不该息屏 */
static void pm_tick(void)
{
    const app_settings_t *c = config_store_get();
    int min = c->screen_off_min;

    /* 插着电（充电中）：默认保持常亮；网页里勾了「充电时也熄屏」才继续走省电逻辑 */
    if (bsp_power_is_charging() && !c->charge_off) {
        if (s_screen_off) {
            pm_wake();
        }
        s_last_input_ms = now_ms();
        return;
    }

    if (min <= 0) {
        if (s_screen_off) {
            pm_wake();          /* 网页上改成常亮了，立刻点亮 */
        }
        return;
    }
    if (s_screen_off) {
        return;
    }
    /* G1 夜间自动调暗：22:00~07:00 且没充电时，背光压到 20% */
    {
        time_weather_t tw;
        time_weather_get(&tw);
        if (tw.time_valid) {
            bool night = (tw.hour >= 22 || tw.hour < 7);
            int want = night ? 20 : (c->brightness > 0 ? c->brightness : 80);
            if (want != s_bl_level) {
                s_bl_level = want;
                bsp_display_backlight(want);
            }
        }
    }

    int64_t idle = now_ms() - s_last_input_ms;
    int64_t limit = (int64_t) min * 60 * 1000;

    /* 快到点前 30 秒先把背光降到 30%，提示"要熄屏了"，也更省电 */
    if (!s_dimmed && idle >= limit - 30000 && idle < limit) {
        s_dimmed = true;
        bsp_display_backlight(30);
        return;
    }
    if (idle < limit) {
        return;
    }
    uint8_t ob = c->off_bright;
    if (ob > c->brightness) {
        ob = c->brightness;          /* 熄屏亮度不该比正常亮度还高 */
    }
    if (ob == 0) {
        bsp_display_backlight(0);
        /* 残影对策①：面板会一直留着最后一幅静态画面（亮黄标题条 + 白字最狠），
           液晶长时间被同一组电压定住就会"记住"它。熄屏时顺手把整屏刷黑，
           等于给面板清场 —— 背光已经是 0，用户看不到这一下。 */
        bsp_display_clear(COLOR_BLACK);
        bsp_display_flush();
        ESP_LOGI(TAG, "无操作 %d 分钟，关背光省电（面板已清屏）", min);
    } else {
        /* 低亮度常显（AOD）：画面留着，只把背光压到 ob%。
           亮度低 → 液晶承受的电压低 → 残影比常亮慢得多，还能瞄一眼时间。 */
        bsp_display_backlight(ob);
        ESP_LOGI(TAG, "无操作 %d 分钟，背光降到 %u%% 常显", min, (unsigned) ob);
    }
    s_aod = (ob > 0);
    s_screen_off = true;
    if (PM_PS_DIM_MS) {
        esp_wifi_set_ps(WIFI_PS_NONE);   /* 不用深度省电，否则网页要等信标 */
    }
}

static void ble_idle_release(void)
{
    static int64_t idle_since = 0;
    esl_state_t est = esl_ble_state();
    bool busy = est == ESL_SCANNING || est == ESL_CONNECTING || est == ESL_PUSHING;

    if (busy || !esl_ble_stack_up()) {
        idle_since = 0;
        return;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (idle_since == 0) {
        idle_since = now_ms;
    } else if (now_ms - idle_since > 2000) {
        esl_ble_release();
        idle_since = 0;
    }
}
static volatile bool s_long_boot, s_long_up, s_long_down;

/* ---------------------------------------------- 蓝牙操作期间的内存保护
 *
 * 网页后台已经删掉了，这里保留空实现（调用点很多），
 * 将来接 USB 网页时再在这里做挂起/恢复。
 */
static void ble_exclusive_begin(void) { }
static void ble_exclusive_end(void) { }

static void esl_finish_task(void *arg);

/* ---------------------------------------- 定时推价签（夜间不推）
 *
 * 规则：
 *   23:00 ~ 07:00 只做网络认证，不推送
 *   其余时间每 3 小时推一次
 */
/* 预渲染任务：每 5 分钟把信息页画好，推送时直接发。
   独立任务的原因：主循环经常被门户登录/重连长时间占用，轮不到它。 */
static void esl_pre_task(void *arg)
{
    (void) arg;
    int64_t last = 0;
    while (true) {
        int64_t t = now_ms();
        if (t - last >= 5 * 60 * 1000) {
            esl_state_t es = esl_ble_state();
            if (es == ESL_IDLE || es == ESL_DONE || es == ESL_ERROR) {
                esp_err_t err = esl_ble_prepare(true);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "已预渲染墨水屏信息页");
                    last = t;
                } else {
                    ESP_LOGW(TAG, "预渲染失败: %s", esp_err_to_name(err));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

#if ENABLE_PERIODIC_ESL_PUSH
static void esl_schedule_task(void *arg)
{
    (void) arg;
    int64_t retry_at_ms = 0;      /* 失败后 10 分钟重试的时刻 */

    /* 开机先等一下，等对时和网络稳定 */
    vTaskDelay(pdMS_TO_TICKS(90 * 1000));

    int last_push_yday = -1;        /* 上次推送的"一年中第几天"，保证每天只推一次 */

    while (true) {
        time_t now = 0;
        time(&now);
        struct tm t;
        localtime_r(&now, &t);

        if (t.tm_year >= 120) {                 /* 时间已同步才有意义 */
            int hour = t.tm_hour;
            bool night = (hour >= 23 || hour < 7);

            if (!night) {
                /* 每天中午 12 点推一次（原来是每 3 小时）。
                   BLE 会抢 2.4G 射频，次数越少 WiFi 越稳 */
                int64_t nms = esp_timer_get_time() / 1000;
                bool at_noon = (hour == 12) &&
                               ((t.tm_yday != last_push_yday) ||
                                (retry_at_ms != 0 && nms >= retry_at_ms));
                if (at_noon) {
                    esl_state_t st = esl_ble_state();
                    if (st == ESL_IDLE || st == ESL_DONE || st == ESL_ERROR) {
                        last_push_yday = t.tm_yday;
                        size_t fi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                        size_t fbk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                        if (fbk < 60000) {
                            /* 最大连续块不够，BLE 起不来。先整理一次再判 */
                            ESP_LOGW(TAG, "推价签前最大块只有 %uKB，先整理内存",
                                     (unsigned) (fbk / 1024));
                            app_mem_tidy("推价签前");
                            fi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                            /* 整理后重新量一次最大块：BLE 要的是**连续**内存，
                               只看总空闲会误判（碎片多时总空闲够、最大块不够） */
                            fbk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                        }
                        if (fbk < 60000) {
                            /* 注意：这里绝对不能 continue，否则会跳过末尾的
                               vTaskDelay，变成 10ms 一次的刷屏死循环 */
                            ESP_LOGW(TAG, "内存碎片太多（空闲 %uKB，最大块 %uKB），5 分钟后再试",
                                     (unsigned) (fi / 1024), (unsigned) (fbk / 1024));
                            retry_at_ms = esp_timer_get_time() / 1000 + 5LL * 60 * 1000;
                        } else {
                            ESP_LOGI(TAG, "定时推送价签（每 3 小时）");
                            ble_exclusive_begin();
                            if (esl_ble_push_info() == ESP_OK) {
                                /* 等这次推送出结论：失败就 10 分钟后再试 */
                                esl_state_t st2 = esl_ble_state();
                                for (int i = 0; i < 300; i++) {
                                    st2 = esl_ble_state();
                                    if (st2 != ESL_SCANNING && st2 != ESL_CONNECTING
                                        && st2 != ESL_PUSHING) {
                                        break;
                                    }
                                    vTaskDelay(pdMS_TO_TICKS(200));
                                }
                                if (st2 == ESL_ERROR) {
                                    ESP_LOGW(TAG, "价签推送失败（%s），10 分钟后再试",
                                             esl_ble_status());
                                    retry_at_ms = esp_timer_get_time() / 1000 + 10LL * 60 * 1000;
                                } else {
                                    retry_at_ms = 0;
                                }
                                xTaskCreate(esl_finish_task, "eslfin", 3072, NULL, 2, NULL);
                            } else {
                                ble_exclusive_end();
                            }
                        }
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));   /* 每分钟查一次 */
    }
}
#endif  // ENABLE_PERIODIC_ESL_PUSH




/* 价签推送是异步的，等它结束再恢复网页 */
static void esl_finish_task(void *arg)
{
    (void) arg;
    for (int i = 0; i < 300; i++) {
        esl_state_t st = esl_ble_state();
        if (st == ESL_IDLE || st == ESL_DONE || st == ESL_ERROR) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ble_exclusive_end();
    vTaskDelete(NULL);
}

static void poll_longpress(void)
{
    static int64_t t0[3];
    static bool fired[3];
    const bsp_button_t btns[3] = {BSP_BTN_BOOT, BSP_BTN_UP, BSP_BTN_DOWN};
    int64_t now = esp_timer_get_time() / 1000;

    if (s_screen == SC_CAPTCHA) {
        t0[0] = t0[1] = t0[2] = 0;
        fired[0] = fired[1] = fired[2] = false;
        return;
    }

    for (int i = 0; i < 3; i++) {
        if (bsp_buttons_pressed(btns[i])) {
            if (t0[i] == 0) {
                t0[i] = now;
            } else if (!fired[i] && now - t0[i] > 900) {
                fired[i] = true;
                if (i == 0) {
                    if (s_screen == SC_ESL) {
                        s_esl_push_at_ms = esp_timer_get_time() / 1000 + 3000;
                        s_esl_push_test_card = true;
                        s_force_redraw = true;
                        ESP_LOGI(TAG, "长按：推送价签测试卡");
                    } else if (s_screen == SC_TIMER || s_screen == SC_STOPWATCH ||
                               s_screen == SC_MODEL) {
                        /* 倒计时/模型页：长按 BOOT 回状态页 */
                        s_screen = SC_STATUS;
                        s_force_redraw = true;
                        ESP_LOGI(TAG, "长按：回到状态页");
                    } else if (s_screen == SC_INFO) {
                        /* manual weather: di kaichang an li ji gengxin tianqi */
                        time_weather_force_refresh();
                        s_force_redraw = true;
                        ESP_LOGI(TAG, "长按：手动更新天气");
                    } else if (s_screen == SC_STATUS) {
                        /* 状态页长按：看验证码模型状态和版本（模型页只能这样进） */
                        s_screen = SC_MODEL;
                        s_force_redraw = true;
                        ESP_LOGI(TAG, "长按：查看模型信息");
                    } else {
                        /* 其它页（信息/时钟/日历/诊断/系统）长按 BOOT 回状态页 */
                        s_screen = SC_STATUS;
                        s_force_redraw = true;
                        ESP_LOGI(TAG, "长按：回到状态页");
                    }
                    s_long_boot = true;
                } else if (i == 1 || i == 2) {
                    if (i == 1) {
                        s_screen = (s_screen + SC_CYCLABLE - 1) % SC_CYCLABLE;
                        s_long_up = true;
                    } else {
                        s_screen = (s_screen + 1) % SC_CYCLABLE;
                        s_long_down = true;
                    }
                }
            }
        } else {
            t0[i] = 0;
            fired[i] = false;
        }
    }
}

/* ------------------------------------------------------ 扩展设置（G 组）
 * 单独用 NVS 命名空间 "ext"，不动主配置结构（避免结构体变化导致配置丢失）
 */
#define EXT_NS "ext"
static volatile bool s_tick_on = true;
static char s_mac_str[20];
static uint8_t  s_bat_hist[8];
static uint8_t  s_bat_hist_n = 0;

static void ext_load(void)
{
    nvs_handle_t h;
    if (nvs_open(EXT_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v = 1;
    if (nvs_get_u8(h, "tick", &v) == ESP_OK) {
        s_tick_on = (v != 0);
    }
    int32_t dl = 0;
    if (nvs_get_i32(h, "dlogin", &dl) == ESP_OK) {
        s_last_login_day = (int) dl;
    }
    int32_t ep = 0;
    if (nvs_get_i32(h, "eslpush", &ep) == ESP_OK) {
        s_esl_push_day = (int) ep;
    }
    uint8_t pa = 1;
    if (nvs_get_u8(h, "portal", &pa) == ESP_OK) {
        s_portal_auth = (pa != 0);
    }
    ESP_LOGI(TAG, "门户认证: %s", s_portal_auth ? "开（由板子完成每日登录）" : "关（不自动登录）");

    size_t mn = sizeof(s_mac_str);
    if (nvs_get_str(h, "mac", s_mac_str, &mn) == ESP_OK && s_mac_str[0] != '\0' &&
        strcmp(s_mac_str, "reset") != 0) {
        ESP_LOGI(TAG, "从 NVS 读到 MAC 伪装: %s", s_mac_str);
    } else {
        s_mac_str[0] = '\0';
    }

    size_t n = sizeof(s_bat_hist);
    if (nvs_get_blob(h, "batr", s_bat_hist, &n) == ESP_OK && n > 0 && n <= sizeof(s_bat_hist)) {
        s_bat_hist_n = (uint8_t) n;
    }
    nvs_close(h);
    beep_set_tick_mute(!s_tick_on);
    ESP_LOGI(TAG, "扩展设置：按键音%s，电量历史 %d 条",
             s_tick_on ? "开" : "关", s_bat_hist_n);
}

/* 设置 MAC 伪装：立即应用 + 存 NVS（重启后仍生效） */
/* 强制走一次门户认证（串口 AUTH 命令 / 状态页三连按） */
/* 设置屏幕方向（0/90/180/270），存 NVS 并立刻生效 */
void app_set_rotation(int deg)
{
    if (deg != 0 && deg != 90 && deg != 180 && deg != 270) {
        deg = 0;
    }
    app_settings_t s;
    config_store_snapshot(&s);
    s.rot = (uint16_t) deg;
    config_store_save(&s);
    bsp_display_set_rotation(deg);
    s_force_redraw = true;
    ESP_LOGW(TAG, "屏幕方向 -> %d 度", deg);
}


/* 清掉"今天已推墨水屏"的标记：调完会重新走一次每日刷屏 */
/* 串口调试用：手动进入/退出夜间休眠（验证关掉后能开回来） */
void app_wifi_night(bool on)
{
    if (on) {
        s_night_idle = true;
        wifi_sta_power(false);
        set_state(ST_NOWIFI, "夜间休眠");
    } else {
        s_night_idle = false;
        wifi_sta_power(true);
        set_state(ST_WIFI, "手动开网");
    }
}

void app_reset_esl_push(void)
{
    s_esl_push_day = 0;
    s_esl_try_ms = 0;
    nvs_handle_t h;
    if (nvs_open(EXT_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "eslpush", 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "已清除每日刷屏标记，下次联网会重新推送");
}

void app_force_auth(void)
{
    s_force_auth = true;
    /* 不再断开 WiFi：以前先重连，重连失败就白按了。
       现在直接用当前连接去做认证；真需要重连时由下面的等待逻辑处理。 */
    app_state_request(APP_ACT_RETRY);
    ESP_LOGW(TAG, "收到强制认证请求（不断开 WiFi，直接认证）");
}

void app_set_portal_auth(bool on)
{
    s_portal_auth = on;
    nvs_handle_t h;
    if (nvs_open(EXT_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "portal", on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "门户认证 -> %s", on ? "开" : "关");
}

void app_set_mac(const char *macstr)
{
    if (wifi_sta_set_mac(macstr) != ESP_OK) {
        return;
    }
    snprintf(s_mac_str, sizeof(s_mac_str), "%s", macstr ? macstr : "");
    nvs_handle_t h;
    if (nvs_open(EXT_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (s_mac_str[0] == '\0' || strcmp(s_mac_str, "reset") == 0) {
            nvs_erase_key(h, "mac");
        } else {
            nvs_set_str(h, "mac", s_mac_str);
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

void ext_set_tick(bool on)
{
    s_tick_on = on;
    beep_set_tick_mute(!on);
    nvs_handle_t h;
    if (nvs_open(EXT_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "tick", on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "按键音 -> %s", on ? "开" : "关");
}

bool ext_tick_on(void)
{
    return s_tick_on;
}

void ext_note_battery(int pct)
{
    if (pct <= 0 || pct > 100) {
        return;
    }
    if (s_bat_hist_n > 0 && s_bat_hist[s_bat_hist_n - 1] == (uint8_t) pct) {
        return;
    }
    if (s_bat_hist_n < sizeof(s_bat_hist)) {
        s_bat_hist[s_bat_hist_n++] = (uint8_t) pct;
    } else {
        memmove(s_bat_hist, s_bat_hist + 1, sizeof(s_bat_hist) - 1);
        s_bat_hist[sizeof(s_bat_hist) - 1] = (uint8_t) pct;
    }
    nvs_handle_t h;
    if (nvs_open(EXT_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "batr", s_bat_hist, s_bat_hist_n);
        nvs_commit(h);
        nvs_close(h);
    }
}

int ext_battery_trend(void)
{
    if (s_bat_hist_n < 2) {
        return 0;
    }
    return (int) s_bat_hist[s_bat_hist_n - 1] - (int) s_bat_hist[0];
}

int ext_battery_last(void)
{
    return s_bat_hist_n > 0 ? s_bat_hist[s_bat_hist_n - 1] : -1;
}

/* ------------------------------------------------------------ 闹钟存取 */
#define ALARM_NS "alarm"

static void alarm_load(void)
{
    nvs_handle_t h;
    if (nvs_open(ALARM_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    for (int i = 0; i < ALARM_MAX; i++) {
        char key[8], val[24];
        snprintf(key, sizeof(key), "a%d", i);
        size_t n = sizeof(val);
        if (nvs_get_str(h, key, val, &n) != ESP_OK) {
            continue;
        }
        int hh = 0, mm = 0, mask = 0, en = 0;
        if (sscanf(val, "%d:%d|%d|%d", &hh, &mm, &mask, &en) == 4) {
            s_alarms[i].hour = (uint8_t) hh;
            s_alarms[i].min = (uint8_t) mm;
            s_alarms[i].mask = (uint8_t) mask;
            s_alarms[i].en = en != 0;
        }
    }
    nvs_close(h);
}

/* 网页要显示当前闹钟设置，这里给个只读接口 */
bool app_alarm_get(int idx, int *hour, int *min, int *mask, bool *en)
{
    if (idx < 1 || idx > ALARM_MAX) {
        return false;
    }
    const alarm_t *a = &s_alarms[idx - 1];
    if (hour) *hour = a->hour;
    if (min)  *min  = a->min;
    if (mask) *mask = a->mask;
    if (en)   *en   = a->en;
    return true;
}

void alarm_set(int idx, int hour, int min, int mask, bool en)
{
    if (idx < 0 || idx >= ALARM_MAX) {
        return;
    }
    s_alarms[idx].hour = (uint8_t) hour;
    s_alarms[idx].min = (uint8_t) min;
    s_alarms[idx].mask = (uint8_t) mask;
    s_alarms[idx].en = en;
    nvs_handle_t h;
    if (nvs_open(ALARM_NS, NVS_READWRITE, &h) == ESP_OK) {
        char key[8], val[24];
        snprintf(key, sizeof(key), "a%d", idx);
        snprintf(val, sizeof(val), "%d:%d|%d|%d", hour, min, mask, en ? 1 : 0);
        nvs_set_str(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "闹钟%d = %02d:%02d 掩码%d %s", idx + 1, hour, min, mask, en ? "开" : "关");
}

const char *alarm_get(int idx)
{
    static char buf[24];
    if (idx < 0 || idx >= ALARM_MAX || !s_alarms[idx].en) {
        return "";
    }
    snprintf(buf, sizeof(buf), "%02d:%02d|%d|1", s_alarms[idx].hour,
             s_alarms[idx].min, s_alarms[idx].mask);
    return buf;
}

/* 每秒钟查一次：到点就响 */
static void alarm_tick(void)
{
    time_weather_t tw;
    time_weather_get(&tw);
    if (!tw.time_valid || s_alarm_ringing) {
        return;
    }
    int cur = tw.hour * 60 + tw.minute;
    if (cur == s_alarm_last_min) {
        return;
    }
    for (int i = 0; i < ALARM_MAX; i++) {
        if (!s_alarms[i].en) {
            continue;
        }
        if (s_alarms[i].hour != tw.hour || s_alarms[i].min != tw.minute) {
            continue;
        }
        if (s_alarms[i].mask != 0) {
            /* mask: bit0=周一 …；time_weather 的 weekday: 0=周日 */
            int wd = (tw.weekday + 6) % 7;      /* 转成 0=周一 */
            if (((s_alarms[i].mask >> wd) & 1) == 0) {
                continue;
            }
        }
        s_alarm_last_min = cur;
        s_alarm_ringing = true;
        s_screen = SC_ALARM;
        s_force_redraw = true;
        beep_alarm_start();
        ESP_LOGW(TAG, "闹钟响：%02d:%02d（第 %d 个）", tw.hour, tw.minute, i + 1);
        break;
    }
}

/* 短按 BOOT 触发一次水墨屏推送（任何页面都能用）。
   先倒计时 3 秒，这段时间里可以按一下价签把它唤醒；
   3 秒内再按一次 BOOT 取消。 */
static void btn_esl_push(void)
{
    esl_state_t es = esl_ble_state();
    if (es != ESL_IDLE && es != ESL_DONE && es != ESL_ERROR) {
        ESP_LOGW(TAG, "价签正忙：%s", esl_ble_status());
        return;
    }

    /* 拉天气/日历的 TLS 会临时占 30KB 左右，最多等 6 秒 */
    size_t free_int = 0;
    for (int i = 0; i < 20; i++) {
        free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (free_int >= 40000) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (free_int < 22000) {
        ESP_LOGE(TAG, "内存不足（%u），先别推价签", (unsigned) free_int);
        return;
    }
    if (free_int < 32000) {
        ESP_LOGW(TAG, "内存偏紧（%u），还是试一下", (unsigned) free_int);
    }

    if (s_esl_push_at_ms != 0) {
        s_esl_push_at_ms = 0;
        ESP_LOGI(TAG, "按键：取消本次推送");
        s_force_redraw = true;
        return;
    }
    s_esl_push_at_ms = esp_timer_get_time() / 1000 + 3000;
    s_esl_push_test_card = false;
    s_force_redraw = true;
    ESP_LOGI(TAG, "按键：3 秒后推送信息页（先唤醒价签）");
}

static void on_button(bsp_button_t btn, void *user)
{
    (void) user;

    pm_wake();          /* 任何按键都先唤醒屏幕 */
    if (s_tick_on) {
        beep_tick();    /* 短促按键音（可在网页/串口关掉） */
    }
    if (s_ui_task_h != NULL) {
        xTaskNotifyGive(s_ui_task_h);      /* 按键零等待，立刻重画 */
    }

    /* 闹钟在响：任意键按停，这一下不再触发别的动作；
       长按 BOOT 额外跳回状态页 */
    if (beep_alarm_active()) {
        bool to_status = (btn == BSP_BTN_BOOT && s_long_boot);
        beep_alarm_stop();
        if (s_alarm_ringing) {
            s_alarm_ringing = false;
            s_screen = SC_CLOCK;
            s_force_redraw = true;
        }
        s_timer_state = 0;
        s_long_boot = false;
        s_long_up = false;
        s_long_down = false;
        if (to_status) {
            s_screen = SC_STATUS;
            s_force_redraw = true;
            ESP_LOGI(TAG, "长按 BOOT：停止闹钟并回到状态页");
        } else {
            ESP_LOGI(TAG, "按键停止闹钟");
        }
        return;
    }

    /* 长按已经处理过：这次释放不再触发短按动作 */
    if (btn == BSP_BTN_BOOT && s_long_boot) { s_long_boot = false; return; }
    if (btn == BSP_BTN_UP && s_long_up) { s_long_up = false; return; }
    if (btn == BSP_BTN_DOWN && s_long_down) { s_long_down = false; return; }


    /* 价签页：短按 BOOT 推送信息页 */
    if (s_screen == SC_ESL && btn == BSP_BTN_BOOT) {
        btn_esl_push();
        return;
    }

    /* 倒计时页：BOOT 开始/暂停/继续/复位，UP/DN 加减 1 分钟 */
    if (s_screen == SC_TIMER && !s_force_redraw) {
        int64_t now = esp_timer_get_time() / 1000;

        if (btn == BSP_BTN_BOOT) {
            if (s_timer_state == 0) {
                s_timer_end_ms = now + (int64_t) s_timer_min * 60000;
                s_timer_state = 1;
                ESP_LOGI(TAG, "倒计时开始：%d 分钟", s_timer_min);
            } else if (s_timer_state == 1) {
                s_timer_left_ms = s_timer_end_ms - now;
                if (s_timer_left_ms < 0) s_timer_left_ms = 0;
                s_timer_state = 2;
                ESP_LOGI(TAG, "倒计时暂停");
            } else if (s_timer_state == 2) {
                s_timer_end_ms = now + s_timer_left_ms;
                s_timer_state = 1;
                ESP_LOGI(TAG, "倒计时继续");
            } else {
                s_timer_state = 0;
                ESP_LOGI(TAG, "倒计时复位");
            }
            return;
        }
        if (btn == BSP_BTN_UP) {
            if (s_timer_state == 1 || s_timer_state == 2) {
                /* 倒计时在跑：UP/DN 直接翻页，去别的界面看状态 */
                s_screen = (s_screen + SC_CYCLABLE - 1) % SC_CYCLABLE;
                return;
            }
            if (s_timer_state == 0 || s_timer_state == 3) {
                s_timer_state = 0;
                if (s_timer_min < 99) {
                    s_timer_min++;
                }
                ESP_LOGI(TAG, "倒计时设为 %d 分钟", s_timer_min);
            }
            return;
        }
        if (btn == BSP_BTN_DOWN) {
            if (s_timer_state == 1 || s_timer_state == 2) {
                s_screen = (s_screen + 1) % SC_CYCLABLE;
                return;
            }
            if (s_timer_state == 0 || s_timer_state == 3) {
                s_timer_state = 0;
                if (s_timer_min > 1) {
                    s_timer_min--;
                }
                ESP_LOGI(TAG, "倒计时设为 %d 分钟", s_timer_min);
            }
            return;
        }
    }

    switch (btn) {
    case BSP_BTN_BOOT:
        if (s_screen == SC_CAPTCHA) {
            s_boot_pressed = 1;      /* 验证码页：换一张 */
            break;
        }
        /* 三连按在**任意页面**都生效：以前只在状态页计数，
           用户在其他页面按了等于白按。 */
        {
            int64_t tnow = esp_timer_get_time() / 1000;
            if (tnow - s_boot3_ms > 4000) {
                s_boot3_cnt = 0;        /* 超过 4 秒重新计数 */
            }
            s_boot3_ms = tnow;
            s_boot3_cnt++;
            ESP_LOGW(TAG, "BOOT 第 %d 次（4 秒内连按 3 次触发强制认证）", s_boot3_cnt);

            if (s_boot3_cnt >= 3) {
                s_boot3_cnt = 0;
                ESP_LOGW(TAG, "连按 3 次 BOOT：强制走一遍门户登录");
                set_state(ST_PROBE, "重新认证中");
                s_force_auth = true;          /* 无条件走一遍登录 */
                app_state_request(APP_ACT_RETRY);
                break;
            }
        }
        /* 下面是"单次按 BOOT"的原有行为，只在状态页做重试 */
        if (s_screen == SC_STATUS) {
            if (s_force_redraw) {
                s_boot3_cnt = 0;
                break;
            }
            s_boot_pressed = 1;
            ESP_LOGI(TAG, "手动触发重试（连按 %d 次）", s_boot3_cnt);
            set_state(ST_PROBE, "手动重试");
            app_state_request(APP_ACT_RETRY);
        }
        break;
    case BSP_BTN_UP:
        if (s_force_redraw) {
            break;
        }
        if (s_screen != SC_CAPTCHA) {
            s_screen = (s_screen + SC_CYCLABLE - 1) % SC_CYCLABLE;
        }
        break;
    case BSP_BTN_DOWN:
        if (s_force_redraw) {
            break;
        }
        if (s_screen != SC_CAPTCHA) {
            s_screen = (s_screen + 1) % SC_CYCLABLE;
        }
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- 主逻辑 */

static bool wait_for_wifi(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (!wifi_sta_connected()) {
        if (waited >= timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;
    }
    return true;
}

/* 登录退避（定义在后面，这里先声明：do_login_round 要用） */
static bool login_backoff_active(void);
static void login_note_result(bool ok);


/* 登录一次会走 TLS + HTTP + 表单解析，缓冲区来来去去把内部 RAM 打成碎片：
   实测登录后"空闲 84KB / 最大块 40KB"，低于推墨水屏要的 60KB。
   ESP-IDF 没有堆压缩，能做的是把还挂着的连接关掉、让缓冲还回去，
   并把最大块打出来，好在日志里看清推送时机。 */
static void app_mem_tidy(const char *why)
{
    portal_keepalive_close();          /* 释放保活连接占的 socket 和缓冲 */
    vTaskDelay(pdMS_TO_TICKS(150));

    size_t fi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t fb = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (fb < 60000) {
        ESP_LOGW(TAG, "内存整理(%s)：空闲 %uKB 最大块 %uKB —— 低于推送门槛 60KB",
                 why, (unsigned) (fi / 1024), (unsigned) (fb / 1024));
    } else {
        ESP_LOGI(TAG, "内存整理(%s)：空闲 %uKB 最大块 %uKB",
                 why, (unsigned) (fi / 1024), (unsigned) (fb / 1024));
    }
}

static void do_login_round(const portal_wlan_t *w)
{
    ESP_LOGI(TAG, "需要登录 (user_ip=%s ac_ip=%s)", w->wlan_user_ip, w->wlan_ac_ip);
    snprintf(s_last_ac, sizeof(s_last_ac), "%s", w->wlan_ac_ip);

    /* 账号密码从配置里读，网页后台随时能改 */
    app_settings_t cfg;
    config_store_snapshot(&cfg);

    char msg[PORTAL_MAX_MSG] = {0};
    portal_login_t r = portal_login(cfg.user, cfg.password, w, msg, sizeof(msg));



    login_note_result(r == PORTAL_LOGIN_OK || r == PORTAL_LOGIN_ALREADY);
    if (r == PORTAL_LOGIN_OK || r == PORTAL_LOGIN_ALREADY) {
        /* 强制登录（AUTH 命令 / BOOT 三连按）不经过探测的"离线→在线"判断，
           原来这种情况统计里不记，出现"明明登录成功但今日成功 0"。
           这里补记一次，60 秒内不重复。 */
        int64_t okms = esp_timer_get_time() / 1000;
        if (okms - s_last_ok_ms > 60000) {
            s_last_ok_ms = okms;
            stats_note_ok();
            s_was_online = true;
        }
    }
    switch (r) {
    case PORTAL_LOGIN_OK:
        ESP_LOGI(TAG, "登录成功");
        set_state(ST_ONLINE, "登录成功");
        break;
    case PORTAL_LOGIN_ALREADY:
        ESP_LOGI(TAG, "账号已在线：%s", msg);
        set_state(ST_ONLINE, "账号已在线");
        break;
    case PORTAL_LOGIN_CAPTCHA:
        set_state(ST_ERROR, "验证码错误");
        break;
    case PORTAL_LOGIN_FAILED:
        ESP_LOGE(TAG, "登录失败：%s", msg);
        set_state(ST_ERROR, msg[0] ? msg : "服务器拒绝");
        break;
    default:
        set_state(ST_ERROR, "网络错误");
        break;
    }

    /* 登录流程结束：把残留连接放掉，顺手记一下内存状况 */
    app_mem_tidy("登录后");
}

/* 老版本的 app_config.h 里没有这三项：给个兜底，升级后不至于编译不过 */
#ifndef APP_PORTAL_BASE
#define APP_PORTAL_BASE "http://YOUR_PORTAL_IP:8090"
#endif
#ifndef APP_PORTAL_AC_IP
#define APP_PORTAL_AC_IP "YOUR_AC_IP"
#endif
#ifndef APP_PORTAL_SSID
#define APP_PORTAL_SSID "YOUR_CAMPUS_SSID"
#endif
#ifndef APP_SCHOOL
#define APP_SCHOOL "未配置"
#endif

/* 把编译期默认值填进配置结构，NVS 空的时候用 */
static void build_defaults(app_settings_t *d)
{
    memset(d, 0, sizeof(*d));
    strlcpy(d->wifi_ssid, APP_WIFI_SSID, sizeof(d->wifi_ssid));
    strlcpy(d->wifi_pass, APP_WIFI_PASS, sizeof(d->wifi_pass));
    strlcpy(d->user, APP_USER, sizeof(d->user));
    strlcpy(d->password, APP_PASSWORD, sizeof(d->password));
    strlcpy(d->lat, APP_WEATHER_LAT, sizeof(d->lat));
    strlcpy(d->lon, APP_WEATHER_LON, sizeof(d->lon));
    strlcpy(d->city, APP_WEATHER_CITY, sizeof(d->city));
    strlcpy(d->cal_url, APP_CAL_URL, sizeof(d->cal_url));
    strlcpy(d->static_ip, APP_STATIC_IP, sizeof(d->static_ip));
    strlcpy(d->portal_base, APP_PORTAL_BASE, sizeof(d->portal_base));
    strlcpy(d->portal_ac_ip, APP_PORTAL_AC_IP, sizeof(d->portal_ac_ip));
    strlcpy(d->portal_ssid, APP_PORTAL_SSID, sizeof(d->portal_ssid));
    d->rot = APP_ROT;
    d->brightness = APP_BRIGHTNESS;
    d->screen_off_min = APP_SCREEN_OFF_MIN;
    d->birthday = 0;        /* 0 = 不提醒生日 */          /* 生日：10 月 31 日 */
}


/* ------------------------------------------------------- 日历在线补全 */

/*
 * 每 7 天拉一次（用户要求的频率）：
 *   - 内置表没覆盖当前年份 → 立刻补
 *   - 已经覆盖 → 7 天内不再拉（cal_remote_fetch 内部判断）
 *   - 拉不到就静默用内置表，日历照样能用
 */
static void cal_refresh_task(void *arg)
{
    (void) arg;

    /* 开机先让 WiFi、对时、登录这些正事跑完 */
    vTaskDelay(pdMS_TO_TICKS(20000));

    for (;;) {
        time_weather_t tw;
        time_weather_get(&tw);

        const char *ip = wifi_sta_ip();
        bool online = ip != NULL && ip[0] != '\0';

        if (online && tw.time_valid && tw.year >= 2024) {
            if (!cal_remote_covers(tw.year)) {
                ESP_LOGI(TAG, "内置表没有 %d 年的安排，联网补一次", tw.year);
                cal_remote_fetch(tw.year, true);
            } else {
                /* 内部有 7 天限制，到点才会真的发请求 */
                cal_remote_fetch(tw.year, false);
            }

            /* 只拉当年。
               以前 11、12 月会顺手拉明年的，但明年的假期安排要等国务院公布，
               在那之前数据源里是空数组，拉了也只会多一条"没解析出数据"的警告。 */
        }

        vTaskDelay(pdMS_TO_TICKS(6 * 3600 * 1000));   /* 6 小时看一次 */
    }
}

/* ---------------------------------------------------------- 按键扫描固件 */
#if BTN_SCAN_MODE
/* 注意：GPIO19/20 是 ESP32-S3 的 USB D-/D+，绝不能当普通输入，
   否则 USB 直接掉线，串口和烧录都失联。43/44 是 UART 控制台，同理跳过。 */
static const int kScanPins[] = {
    0, 1, 2, 3, 4, 5, 6, 7,
    11, 12, 13, 14, 15, 16, 17, 18, 21,
    38, 39, 40, 41, 42, 45, 46, 47, 48,
};
#define SCAN_N ((int) (sizeof(kScanPins) / sizeof(kScanPins[0])))

static void button_scan_mode(void)
{
    int base[SCAN_N];

    for (int i = 0; i < SCAN_N; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << kScanPins[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "===== 按键扫描开始 =====");
    for (int i = 0; i < SCAN_N; i++) {
        base[i] = gpio_get_level(kScanPins[i]);
        ESP_LOGI(TAG, "GPIO %2d 初始电平 %d", kScanPins[i], base[i]);
    }
    ESP_LOGI(TAG, "请依次按下每个按键，观察下面的变化日志");

    while (true) {
        for (int i = 0; i < SCAN_N; i++) {
            int v = gpio_get_level(kScanPins[i]);
            if (v != base[i]) {
                base[i] = v;
                ESP_LOGW(TAG, ">>> GPIO %d 电平 -> %d %s", kScanPins[i], v,
                         v == 0 ? "（按下？）" : "（松开）");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
#endif

/* ---------------------------------------------- 夜间 WiFi 省电
 *
 * 22:00 ~ 06:00：关闭 WiFi（这时候路由器一般是关的，开着只会刷 reason=201）。
 * 06:00：重新打开 WiFi，等联网后自动走门户认证。
 * 白天：WiFi 常开。
 */
static void night_wifi_task(void *arg)
{
    (void) arg;
    bool morning_sync_done = false;

    vTaskDelay(pdMS_TO_TICKS(120 * 1000));      /* 等开机流程稳定 */

    while (true) {
        time_t now = 0;
        time(&now);
        struct tm t;
        localtime_r(&now, &t);

        if (t.tm_year >= 120) {
            int hour = t.tm_hour;
            /* 22:00 ~ 06:00 = 省电时段（用户要求：路由器这时段是关的，
               板子开着也连不上，直接关 WiFi） */
            bool night = (hour >= 22) || (hour < 6);

            if (!night) {
                /* 白天：如果夜里关过网，这里重新打开 */
                if (s_night_idle) {
                    s_night_idle = false;
                    wifi_sta_power(true);
                    ESP_LOGW(TAG, "06:00 到了：重新打开 WiFi，等联网后自动认证");
                    set_state(ST_WIFI, "早上开网");
                }

                /* 6 点这一段醒来一次：等联网后对时 + 拉天气 */
                if (hour == 6 && !morning_sync_done) {
                    morning_sync_done = true;
                    for (int i = 0; i < 50 && !wifi_sta_connected(); i++) {
                        vTaskDelay(pdMS_TO_TICKS(500));
                    }
                    ESP_LOGI(TAG, "早上 6 点：只对时，天气交给 8 点那次");
                    /* 天气改成每天 8 点全量拉，这里不再重复请求 */
                }
            } else {
                /* 省电时段：
                   - 插着电：不省电，WiFi 保持常开；
                   - 用电池：先把门户认证搞定，认证不上就最多再等 10 分钟，
                     之后再照常关网（认证优先） */
                /* 用户要求：22:00~06:00 一律关 WiFi。
                   路由器夜里是关的，开着只会刷 reason=201。 */
                if (!s_night_idle) {
                    s_night_idle = true;
                    wifi_sta_power(false);
                    set_state(ST_NOWIFI, "夜间休眠");
                    ESP_LOGW(TAG, "22:00~06:00 省电：已关闭 WiFi");
                }

                morning_sync_done = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20 * 1000));
    }
}


/* ---------------- 每日登录 ---------------- */
/* 校园网规则：登录一次，WiFi 下所有设备都能上网。
   板子负责每天完成这一次登录。
   每天 6:00 之后检查一次，当天没登录过就主动认证。 */
/* 只有登录成功（或确认已在线）才标记"今天已完成" */
static void mark_login_done(void)
{
    time_t now = time(NULL);
    if (now < 1600000000L) {
        return;
    }
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int day = (tm_now.tm_year + 1900) * 10000 + (tm_now.tm_mon + 1) * 100 + tm_now.tm_mday;
    if (s_last_login_day == day) {
        return;
    }
    s_last_login_day = day;
    nvs_handle_t h;
    if (nvs_open(EXT_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "dlogin", day);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "每日登录：今天(%d)已完成，标记成功", day);
}

/* 失败后至少隔 5 分钟再试 */
static int64_t s_last_login_try_ms = 0;

/* ---- 自动登录退避 ----
   失败后按 30 秒 / 1 分 / 2 分 / 5 分 / 10 分 / 30 分 逐级拉长，
   避免网络异常时每秒重试把门户刷爆（实测一夜几百次，有锁号风险）。
   手动触发（BOOT 连按 3 次、串口 AUTH）不走退避，随时可试。 */
static int64_t s_login_backoff_until = 0;   /* 早于这个时刻不再自动尝试 */
static int     s_login_fail_streak   = 0;

static bool login_backoff_active(void)
{
    if (s_login_backoff_until == 0) {
        return false;
    }
    return (esp_timer_get_time() / 1000) < s_login_backoff_until;
}

static void login_note_result(bool ok)
{
    if (ok) {
        if (s_login_fail_streak > 0) {
            ESP_LOGI(TAG, "自动登录成功，退避清零（之前连续失败 %d 次）",
                     s_login_fail_streak);
        }
        s_login_fail_streak = 0;
        s_login_backoff_until = 0;
        return;
    }
    if (s_login_fail_streak < 6) {
        s_login_fail_streak++;
    }
    static const int steps[] = { 30, 60, 120, 300, 600, 1800 };
    int idx = s_login_fail_streak - 1;
    if (idx > 5) {
        idx = 5;
    }
    s_login_backoff_until = (esp_timer_get_time() / 1000)
                            + (int64_t) steps[idx] * 1000;
    ESP_LOGW(TAG, "自动登录失败第 %d 次，%d 秒内不再尝试",
             s_login_fail_streak, steps[idx]);
}

static void daily_login_check(void)
{
    time_t now = time(NULL);
    if (now < 1600000000L) {        /* 时间还没同步，别乱登 */
        return;
    }
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int day = (tm_now.tm_year + 1900) * 10000 + (tm_now.tm_mon + 1) * 100 + tm_now.tm_mday;
    if (s_last_login_day == day) {
        return;                     /* 今天已经登过 */
    }
    /* 每天 6:00 开始尝试，失败每 5 分钟重试一次，直到成功。
       6:00~7:30 有 18 次机会，覆盖路由器开机误差和 WiFi 抖动。 */
    if (tm_now.tm_hour < 6) {
        return;
    }
    /* 失败后每 5 分钟重试一次：6 点路由器可能还没起来，
       或者关联要等一会儿，不能试一次就放弃一整天 */
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_last_login_try_ms != 0 && now_ms - s_last_login_try_ms < 5 * 60 * 1000) {
        return;
    }
    if (login_backoff_active()) {
        return;                     /* 自动登录正在退避，别刷门户 */
    }
    s_last_login_try_ms = now_ms;
    ESP_LOGW(TAG, "每日登录：今天(%d)还没成功登录过，主动认证一次", day);
    s_force_auth = true;
    app_state_request(APP_ACT_RETRY);
}

void app_main(void)
{
    logring_start();       /* 越早越好：后面的日志都能留住 */
#if BTN_SCAN_MODE
    button_scan_mode();
#endif

    /* 新固件第一次跑起来就标记有效；只有存在 otadata 分区时才需要（单 factory 分区会报 not found otadata） */
    if (esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL) != NULL) {
        esp_ota_mark_app_valid_cancel_rollback();
    }
    {
        const esp_partition_t *run = esp_ota_get_running_partition();
        esp_app_desc_t desc;
        if (esp_ota_get_partition_description(run, &desc) == ESP_OK) {
            ESP_LOGI(TAG, "固件 %s 运行在分区 %s（编译 %s %s）",
                     desc.version, run ? run->label : "?", desc.date, desc.time);
        }
    }
    bsp_power_hold();     /* 电池供电必须锁电源，否则松手断电 */

    app_state_init();

    ESP_LOGI(TAG, "==================== 启动 ====================");
    ESP_LOGI(TAG, "CPU %u MHz | 内部 RAM %uK | PSRAM %uK",
             (unsigned) CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
             (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "芯片: %s %d 核  Flash %u MB",
             chip.model == CHIP_ESP32S3 ? "ESP32-S3" : "其它",
             chip.cores, (unsigned) (flash_size / (1024 * 1024)));

    if (bsp_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "屏幕初始化失败，继续跑");
    } else {
        /* 开机 LOGO，1.2 秒后进主界面 */
        draw_boot_logo();
        vTaskDelay(pdMS_TO_TICKS(1200));
    }

    /* 按配置应用屏幕方向（0/90/180/270） */
    bsp_display_set_rotation((int) config_store_get()->rot);

    bsp_buttons_init();
    bsp_buttons_on_click(on_button, NULL);
    bsp_buttons_start();

    /* 界面任务尽早起来：WiFi 连接/对时的过程都能看到 */
    fw_size_kb();          /* 预热：系统页第一次打开就不用等 200ms */
    xTaskCreate(ui_task, "ui", 6144, NULL, 2, NULL);

    /* 环境声音按需启动：不再开机常开，进诊断页才开（省 CPU/内存）。
       见 ui_task 里对 SC_DIAG 的处理 */

    /* NVS 要先起，配置从里面读 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    bg_init();      /* 自定义背景图存在 storage 分区 */

    /* 配置：NVS 里有就用 NVS 的（网页改过），没有就用编译期默认值 */
    app_settings_t defaults;
    build_defaults(&defaults);
    config_store_init(&defaults);
    app_settings_t cfg;
    config_store_snapshot(&cfg);

    /* 站点参数交给门户组件：门户地址 / AC IP / 校园网 SSID 都能在网页或串口里改 */
    portal_set_site(cfg.portal_base, cfg.portal_ac_ip, cfg.portal_ssid);
    /* 主题：0..4 用内置，0xFF 用自定义（bg + accent） */
    if (cfg.theme_id == 0xFF) {
        bsp_theme_custom(cfg.theme_bg, cfg.theme_accent);
    } else if (cfg.theme_id < BSP_THEME_COUNT) {
        bsp_theme_load_preset(cfg.theme_id);
    } else {
        bsp_theme_load_preset(0);
    }
    bsp_theme_ensure_readable(bg_has_image());
    bsp_deco_set(cfg.deco_border, cfg.deco_icon, cfg.deco_tag);
    bsp_deco_set_grid(cfg.deco_grid != 0);
    /* 主题昼夜 / 背景轮换：运行时直接读 config（下面 apply_* 里读） */

    usb_console_init();                      /* USB 串口网页（Web Serial）命令行 */
    stats_init();
    alarm_load();          /* 从 NVS 读闹钟 */
    ext_load();            /* 按键音开关 + 价签电量历史 */

    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "YingWang v%s by %s（%s）", desc ? desc->version : "?",
             FW_AUTHOR, desc ? desc->date : "?");
    /* 日志里不打全账号和 SSID：诊断页、截图、提 issue 都会把日志带出去，
       账号是手机号（PII），SSID 里也可能带名字。只留够排错的头尾。 */
    char user_mask[32];
    char ssid_mask[40];
    {
        size_t n = strlen(cfg.user);
        if (n <= 7) {
            strlcpy(user_mask, "***", sizeof(user_mask));
        } else {
            snprintf(user_mask, sizeof(user_mask), "%.3s****%.4s", cfg.user,
                     cfg.user + n - 4);
        }
        size_t m = strlen(cfg.wifi_ssid);
        if (m <= 4) {
            strlcpy(ssid_mask, "***", sizeof(ssid_mask));
        } else {
            snprintf(ssid_mask, sizeof(ssid_mask), "%.3s***(%u 字符)",
                     cfg.wifi_ssid, (unsigned) m);
        }
    }
    ESP_LOGI(TAG, "账号: %s  WiFi: %s", user_mask, ssid_mask);

    cal_remote_init();                       /* 载入上次存下来的在线日历 */
    cal_remote_set_url(cfg.cal_url);
    xTaskCreate(cal_refresh_task, "cal", 8192, NULL, 3, NULL);

    bsp_display_backlight(cfg.brightness);
    s_last_input_ms = now_ms();          /* 电源管理：从现在开始算空闲 */
    /* 关掉省电：这颗路由器的信标间隔 102ms，modem sleep 下
   每次收发都要等信标，网页会慢到打不开 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    set_state(ST_WIFI, "等待路由器");
    if (s_mac_str[0] != '\0') {
        wifi_sta_set_mac(s_mac_str);      /* 必须在 start/connect 之前 */
    }
    wifi_sta_set_static_ip(cfg.static_ip);   /* 必须在 start 之前，否则先走了 DHCP */
    if (wifi_sta_start(cfg.wifi_ssid, cfg.wifi_pass) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 启动失败");
        set_state(ST_NOWIFI, "WiFi 启动失败");
    }

    /* 网络监控和天气要在联网后才有意义，先起任务让它们自己等 */

    /* 墨水屏页面预渲染（独立任务，每 5 分钟一次） */
    xTaskCreate(esl_pre_task, "eslpre", 4096, NULL, 1, NULL);

#if ENABLE_PERIODIC_ESL_PUSH
    /* 定时推价签（已默认关闭，见 ENABLE_PERIODIC_ESL_PUSH 说明） */
    xTaskCreate(esl_schedule_task, "eslsch", 4096, NULL, 2, NULL);
#endif

    /* 夜间 WiFi 省电：23:00-07:00 只在整点开一会儿 */
    xTaskCreate(night_wifi_task, "nightwifi", 4096, NULL, 2, NULL);

    if (!wait_for_wifi(60000)) {
        /* 只用一个 WiFi：连不上就继续后台重连，不换成别的配置。
           长时间连不上由「离线 30 分钟重启」兜底。 */
        ESP_LOGW(TAG, "60 秒还没连上 WiFi，继续重连");
        set_state(ST_NOWIFI, "密码或信号有问题");
    } else {
        set_state(ST_PROBE, wifi_sta_ip());
        /* WiFi 起来后才能真正设置省电模式；开机那次太早，会被驱动忽略 */
        esp_wifi_set_ps(WIFI_PS_NONE);

        /* 网页后台不在这里自动启动：由状态页长按 BOOT 控制 */
    }

    time_weather_start(cfg.lat, cfg.lon, cfg.city);

    /* 验证码上屏 */
    portal_set_captcha_callback(on_captcha, NULL);

    ESP_LOGI(TAG, "空闲内存: %u KB",
             (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    int fail_streak = 0;
    bool absent_logged = false;      /* "目标不在附近"只记一次日志 */
    const uint32_t ABSENT_WAIT_S = 60;
    bool was_connected = wifi_sta_connected();
    bool need_wifi_reload = false;

    for (;;) {
        /* 夜间省电：WiFi 关着，主循环先歇着，别去探测/重连。
           但重启请求得处理，不然网页点了"重启"要等到早上 7 点 */
        if (s_night_idle) {
            uint32_t night_acts = app_state_take_actions();
            if (night_acts & APP_ACT_REBOOT) {
                ESP_LOGW(TAG, "夜间收到重启请求");
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
            if (night_acts & ~APP_ACT_REBOOT) {
                app_state_request(night_acts & ~APP_ACT_REBOOT);  /* 别的动作留到白天 */
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* ---- 网页 / 按键发来的动作 ---- */
        uint32_t acts = app_state_take_actions();

        if (acts & APP_ACT_REBOOT) {
            ESP_LOGW(TAG, "收到重启请求");
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
        if (acts & APP_ACT_RECONNECT) {
            need_wifi_reload = true;
        }
        if (acts & APP_ACT_RETRY) {
            ESP_LOGI(TAG, "收到重试请求");
            fail_streak = 0;
            set_state(ST_PROBE, "手动重试");
        }
        if (acts & APP_ACT_REDRAW) {
            ESP_LOGI(TAG, "收到重画请求（背景图已更新）");
            s_force_redraw = true;
            s_clock_full = true;
            s_cal_day = -1;
        }

        if (need_wifi_reload) {
            need_wifi_reload = false;
            config_store_snapshot(&cfg);
            ESP_LOGW(TAG, "配置已更新，重连 WiFi: %s", cfg.wifi_ssid);
            set_state(ST_WIFI, "应用新配置");
            wifi_sta_reload(cfg.wifi_ssid, cfg.wifi_pass);

            if (!wait_for_wifi(120000)) {
                /* 只保留一组 WiFi：不改回去也不换别的，只报状态 */
                ESP_LOGW(TAG, "新配置 120 秒没连上，保持这组配置继续重连");
                set_state(ST_NOWIFI, "密码或信号有问题");
            }
            was_connected = wifi_sta_connected();
            continue;
        }

        /* ---- 连接状态 ---- */
        bool connected = wifi_sta_connected();

        /* 内存看门狗：内部内存掉到 15KB 以下就重启（反复重连/BLE 会漏）。
           推价签期间 BLE 会临时占 70KB+，这期间不判。 */
        {
            size_t hfree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            bool esl_busy = (esl_ble_state() == ESL_PUSHING || esl_ble_state() == ESL_SCANNING);
            if (hfree < 15000 && !esl_busy) {
                ESP_LOGE(TAG, "内部内存只剩 %u 字节，重启板子", (unsigned) hfree);
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        }

        /* 离线看门狗：连续 5 分钟连不上 WiFi 就重启一次。
           有些 AP/驱动卡死后，只有重启才能重新关联上。 */
        {
            static int64_t offline_since = 0;
            int64_t tnow = now_ms();
            if (connected) {
                offline_since = 0;
            } else if (offline_since == 0) {
                offline_since = tnow;
            } else if (tnow - offline_since >= OFFLINE_REBOOT_MS) {
                /* 以前这里是 esp_restart()。但"连不上"分两种：
                   驱动卡死（必须重启）和上游断网（重启没用）。
                   重启的代价是中断墨水屏推送、统计漏记，
                   表现成反复重启、屏幕不动，看起来像死机。
                   改成重连 WiFi，然后重新计时。 */
                ESP_LOGE(TAG, "离线已超过 %lld 分钟，重连 WiFi（不重启）",
                         (long long) (OFFLINE_REBOOT_MS / 60000));
                wifi_sta_force_reconnect();
                offline_since = tnow;          /* 再来一轮 */
            }
        }


        /* ---- 早高峰看门狗（06:00~07:30）----
           路由器刚开机时经常"关联成功又被踢"，一直对着同一个 AP 重试没用。
           这里连续 30 分钟没拿到 IP，就把 WiFi 驱动整个重启一次继续试。 */
        {
            static int64_t s_wifi_down_ms = 0;
            time_t nowt = time(NULL);
            struct tm tmw;
            localtime_r(&nowt, &tmw);
            bool morning = (nowt > 1600000000L) &&
                           (tmw.tm_hour == 6 || (tmw.tm_hour == 7 && tmw.tm_min < 30));
            int64_t nm = esp_timer_get_time() / 1000;

            if (wifi_sta_ip()[0] != '\0' || !morning || s_night_idle) {
                s_wifi_down_ms = 0;
            } else if (s_wifi_down_ms == 0) {
                s_wifi_down_ms = nm;
            } else if (nm - s_wifi_down_ms >= 30 * 60 * 1000) {
                ESP_LOGE(TAG, "早上 30 分钟没连上 WiFi，硬重启 WiFi 再试");
                wifi_sta_hard_restart();
                s_wifi_down_ms = nm;       /* 再来一轮 30 分钟 */
            }
        }

        if (was_connected && !connected) {
            stats_note_drop();
            set_state(ST_WIFI, "掉线重连");
        }
        was_connected = connected;

        if (connected) {
            portal_client_set_my_ip(wifi_sta_ip());
        }

        if (!connected) {
            esl_ble_set_net_ready(false);
        }

        if (!connected) {
            /*
             * 策略（和手机端一致）：
             *   目标热点不在附近 → 安静等着，不空转重连
             *   在附近 → 重连，最多等 30 秒拿 IP
             */
            app_settings_t cf;
            config_store_snapshot(&cf);
            int8_t rssi = wifi_sta_target_rssi(cf.wifi_ssid);
            if (rssi <= -120) {
                set_state(ST_NOWIFI, "目标不在附近");
                if (!absent_logged) {
                    ESP_LOGI(TAG, "附近没有 %s，等着（不重连）", cf.wifi_ssid);
                    absent_logged = true;
                }
                vTaskDelay(pdMS_TO_TICKS(ABSENT_WAIT_S * 1000));
                continue;
            }
            absent_logged = false;
            char b[48];
            snprintf(b, sizeof(b), "重连 %d dBm", rssi);
            set_state(ST_WIFI, b);
            wifi_sta_reload(cf.wifi_ssid, cf.wifi_pass);
            wait_for_wifi(30000);
            continue;
        }

        /* ---- 探测并登录 ---- */
        portal_wlan_t w;
        memset(&w, 0, sizeof(w));
        /* 已经在线就不要再弹"检测中"：静默探测，只有结果变了才改状态。
           否则每 15 秒屏幕闪一次，看着像一直在重连 */
        bool was_online = (s_state == ST_ONLINE);
        if (!was_online && fail_streak == 0) {
            set_state(ST_PROBE, wifi_sta_ip());
        }
        daily_login_check();           /* 每天一次的主动登录 */

        /* DNS 解析失败 → 立刻重设 DNS（lwIP 里的服务器条目会莫名失效） */
        if (portal_take_dns_failed()) {
            int64_t nowd = esp_timer_get_time() / 1000;
            if (nowd - s_last_dns_fix_ms > 30000) {
                s_last_dns_fix_ms = nowd;
                ESP_LOGW(TAG, "检测到 DNS 解析失败，换下一个 DNS 候选");
                wifi_sta_dns_rotate();
            }
        }

        /* 按时间看门狗：只要"有 IP 但连续 N 分钟探测不成功"就动手，
           不再依赖探测返回的具体类型（以前卡在"检测中"就不会触发） */
        {
            int64_t nowm = esp_timer_get_time() / 1000;
            if (s_last_ok_probe_ms == 0) {
                s_last_ok_probe_ms = nowm;
            }
            int64_t no_ok_ms = nowm - s_last_ok_probe_ms;
            if (wifi_sta_ip()[0] != '\0' && no_ok_ms > 3 * 60 * 1000) {
                ESP_LOGE(TAG, "已 %lld 秒没有探测成功，主动重连 WiFi",
                         (long long) (no_ok_ms / 1000));
                s_last_ok_probe_ms = nowm;      /* 再来一轮 3 分钟 */
                s_stuck_cnt++;
                /* 只重连，不重启：
                   重启会让板子陷入"刚连上又重启"的循环，反而永远连不上。 */
                wifi_sta_force_reconnect();
            }
        }

        portal_probe_t probe = portal_probe(&w);

        /* 手动要求强制认证：不管探测怎么说，都直接走一遍门户登录。
           典型场景：你知道网络断了/认证过期，想立刻重登一次。 */
        if (s_force_auth) {
            s_force_auth = false;
            ESP_LOGW(TAG, "强制认证请求（探测结果=%d）", (int) probe);

            /* WiFi 没连上就先等它（最多 30 秒），否则发什么都白搭 */
            int waited = 0;
            while (wifi_sta_ip()[0] == '\0' && waited < 30) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                waited++;
            }
            if (wifi_sta_ip()[0] == '\0') {
                ESP_LOGE(TAG, "强制认证失败：WiFi 30 秒内没连上");
                set_state(ST_ERROR, "WiFi 没连上");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }

            /* 真·强制：不管探测结果如何，都走一遍门户登录。
               但登录前必须有真实的 wlanUserIp（强制认证时没有 302 跳转，
               门户首页给的 IP 是空的，提交空值等于白烧验证码）。 */
            if (w.wlan_user_ip[0] == '\0' || w.wlan_ac_ip[0] == '\0') {
                if (!portal_refresh_params(&w)) {
                    set_state(ST_ERROR, "拿不到门户参数");
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    continue;
                }
            }
            ESP_LOGW(TAG, "开始强制门户认证（user_ip=%s ac_ip=%s）",
                     w.wlan_user_ip, w.wlan_ac_ip);
            set_state(ST_LOGIN, "强制认证");
            do_login_round(&w);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        if (probe == PORTAL_ONLINE) {
            s_last_ok_probe_ms = esp_timer_get_time() / 1000;
            s_stuck_cnt = 0;
            mark_login_done();      /* 已在线 = 今天的登录任务已完成 */

            /* 每天第一次连上网之后，刷新一次墨水屏。
               等 30 秒是为了让天气/空气质量先拉下来，推上去的才是新数据。 */
            time_t now_t = time(NULL);
            if (now_t > 1600000000L && s_online_since_ms == 0) {
                s_online_since_ms = esp_timer_get_time() / 1000;
            }
            if (now_t > 1600000000L && s_online_since_ms != 0) {
                struct tm tm_now2;
                localtime_r(&now_t, &tm_now2);
                int today = (tm_now2.tm_year + 1900) * 10000
                            + (tm_now2.tm_mon + 1) * 100 + tm_now2.tm_mday;
                int64_t online_ms = (esp_timer_get_time() / 1000) - s_online_since_ms;
                int64_t now_m = esp_timer_get_time() / 1000;
                /* 失败后 10 分钟再试一次；成功后今天不再推 */
                if (s_esl_push_day != today && online_ms > 30000 &&
                    (s_esl_try_ms == 0 || now_m - s_esl_try_ms >= 10 * 60 * 1000)) {
                    s_esl_try_ms = now_m;
                    ESP_LOGW(TAG, "每日刷屏：今天(%d)第一次连上网，刷新墨水屏", today);
                    /* BLE 起来要一块 60KB 左右的连续内部内存。登录刚结束时
                       碎片多（实测空闲 82KB 但最大块只有 34KB），先整理一下
                       能明显提高这一步的成功率；整理完还是不够就让 prepare 自己
                       报错，交给下面"10 分钟后再试"的分支。 */
                    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 60000) {
                        app_mem_tidy("每日刷屏前");
                    }
                    /* 关键：prepare 只把画面渲染到内存，不会连价签。
                       真正发出去要用 push_info（扫描→连接→推图→断开）。 */
                    esp_err_t er = esl_ble_prepare(true);
                    if (er == ESP_OK) {
                        ESP_LOGI(TAG, "每日刷屏：画面已渲染，开始推送到价签");
                        er = esl_ble_push_info();
                    }
                    ESP_LOGI(TAG, "每日刷屏结果: %s", esp_err_to_name(er));
                    /* BLE 和 WiFi 共用 2.4G，推图期间 WiFi 容易漏 beacon
                       被 AP 判超时踢掉（日志里 reason=2 占了七成）。
                       推完先等 3 秒让射频稳住，再继续探测。 */
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    if (er == ESP_OK) {
                        /* 只有真的推成功才记账 */
                        s_esl_push_day = today;
                        nvs_handle_t hh;
                        if (nvs_open(EXT_NS, NVS_READWRITE, &hh) == ESP_OK) {
                            nvs_set_i32(hh, "eslpush", today);
                            nvs_commit(hh);
                            nvs_close(hh);
                        }
                    } else {
                        ESP_LOGW(TAG, "推送没成功，10 分钟后再试");
                    }
                }
            }
            /* 只在"离线→在线"时记一次，且最少隔 60 秒：网络抖动不会刷爆计数 */
            if (!s_was_online) {
                s_was_online = true;
                int64_t now_ms = esp_timer_get_time() / 1000;
                if (now_ms - s_last_ok_ms > 60000) {
                    s_last_ok_ms = now_ms;
                    stats_note_ok();
                }
            }
            if (fail_streak) {
                ESP_LOGI(TAG, "网络已恢复");
                fail_streak = 0;
            }
            portal_client_set_my_ip(wifi_sta_ip());   /* 登录时用当前 IP */
            set_state(ST_ONLINE, wifi_sta_ip());
            esl_ble_set_net_ready(true);      /* 允许推价签 */
            esl_ble_note_authed();        /* 墨水屏状态取最近 10 分钟的记录 */
            /* 只在"刚连上"时预渲染一次：以前每个探测周期都渲染一遍，
               每 15 秒刷一次，白白占 CPU、还可能反复刷新墨水屏。 */
            if (!s_was_online) {
                esp_err_t perr = esl_ble_prepare(true);
                if (perr != ESP_OK) {
                    ESP_LOGW(TAG, "预渲染失败(%s)，推送时再现场画", esp_err_to_name(perr));
                } else {
                    ESP_LOGI(TAG, "已预渲染墨水屏信息页（首次联网）");
                }
            }
            vTaskDelay(pdMS_TO_TICKS(APP_CHECK_INTERVAL_S * 1000));
            continue;
        }

        if (probe == PORTAL_UNREACHABLE) {
            /* 有 IP 却完全不通：常见于 lwIP/DNS 卡死（今天实测卡了 4 小时）。
               自己不会恢复，需要主动重连甚至重启。 */
            if (wifi_sta_ip()[0] != '\0') {
                s_stuck_cnt++;
                if (s_stuck_cnt == 8) {
                    ESP_LOGW(TAG, "连续 %d 次有 IP 却不通，主动重连 WiFi", s_stuck_cnt);
                    wifi_sta_force_reconnect();
                } else if (s_stuck_cnt >= 40) {
                    /* 以前这里也重启。有 IP 但外网不通，通常是上游 DNS/网关问题，
                       重启解决不了，反而把推送和统计打断。改成重连并归零计数。 */
                    ESP_LOGE(TAG, "连续 %d 次不通（约 10 分钟），重连 WiFi（不重启）",
                             s_stuck_cnt);
                    wifi_sta_force_reconnect();
                    s_stuck_cnt = 0;
                    s_last_ok_probe_ms = esp_timer_get_time() / 1000;
                }
            }
            set_state(ST_ERROR, "网络不可达");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        s_stuck_cnt = 0;      /* 只要没走到"不可达"就清零 */

        /* 透明桥接：认证由路由器统一完成，板子不需要登录。
           这里绝不能提交门户登录，否则可能把整条会话顶掉。 */
        if (!s_portal_auth && !s_force_auth) {
            set_state(ST_ERROR, "需要认证（已关闭自动登录）");
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }

        if (login_backoff_active()) {
            /* 退避中：只显示状态，不再提交，等退避时间到 */
            set_state(ST_ERROR, "登录失败，退避中");
            vTaskDelay(pdMS_TO_TICKS(APP_CHECK_INTERVAL_S * 1000));
            continue;
        }
        set_state(ST_LOGIN, w.wlan_ac_ip);
        do_login_round(&w);

        /* 连上之后盯 30 秒：门户一冒头就再登一次，通了立刻收工 */
        bool ok = false;
        for (int i = 0; i < 10; i++) {
            portal_wlan_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            portal_probe_t pr2 = portal_probe(&tmp);
            if (pr2 == PORTAL_ONLINE) {
                ok = true;
                break;
            }
            if (pr2 == PORTAL_NEED_LOGIN) {
                ESP_LOGI(TAG, "又冒出门户，再登一次（第 %d 次）", i + 1);
                do_login_round(&tmp);
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
        }

        if (ok) {
            fail_streak = 0;
            set_state(ST_ONLINE, wifi_sta_ip());
            time_weather_refresh();     /* 刚联网，顺手更新天气 */
            vTaskDelay(pdMS_TO_TICKS(APP_CHECK_INTERVAL_S * 1000));
        } else {
            fail_streak++;
            s_was_online = false;
            stats_note_fail();
            /* 失败退避：15 → 30 → 60 → 120 秒封顶 */
            uint32_t wait_s = 15u << (fail_streak > 3 ? 3 : (fail_streak - 1));
            if (wait_s > 120) wait_s = 120;
            char buf[40];
            snprintf(buf, sizeof(buf), "第 %d 次失败", fail_streak);
            set_state(ST_ERROR, buf);
            vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));
        }
    }
}
