/*
 * 全局运行状态
 *
 * 主程序和网页后台是两个任务，需要一个地方放共享状态：
 *   - 当前状态（已认证/登录中/失败…）和一行详情
 *   - 网页发来的动作请求（重试登录、重连 WiFi、重启）
 *   - 最近日志环形缓冲（网页上能看日志，不用接串口）
 */
#ifndef _APP_STATE_H_
#define _APP_STATE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    APP_ST_BOOT = 0,
    APP_ST_WIFI,
    APP_ST_PROBE,
    APP_ST_LOGIN,
    APP_ST_ONLINE,
    APP_ST_NOWIFI,
    APP_ST_ERROR,
} app_state_t;

/* 网页请求的动作 */
enum {
    APP_ACT_NONE      = 0,
    APP_ACT_RETRY     = 1 << 0,   /* 立刻重新探测/登录 */
    APP_ACT_RECONNECT = 1 << 1,   /* 断开重连 WiFi（配置改过时用） */
    APP_ACT_REBOOT    = 1 << 2,   /* 重启板子 */
    APP_ACT_REDRAW    = 1 << 3,   /* 立刻重画屏幕（换了背景图之后用） */
};

void app_state_init(void);

/** 主程序设置状态。detail 可以为 NULL */
void app_state_set(app_state_t st, const char *detail);

/** 读取状态，返回状态码，detail 拷贝到调用者缓冲 */
app_state_t app_state_get(char *detail, size_t cap);

/** 状态的中文名和颜色（网页和屏幕共用一套措辞） */
const char *app_state_name(app_state_t st);
const char *app_state_color(app_state_t st);

/** 开机以来运行了多少秒 */
uint32_t app_state_uptime_s(void);

/* ── 网页发来的动作 ───────────────────────────── */
void app_state_request(uint32_t actions);

/** 主程序取走动作标志（取走即清零） */
uint32_t app_state_take_actions(void);

#endif  // _APP_STATE_H_
