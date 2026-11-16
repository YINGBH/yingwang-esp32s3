/*
 * ESP32-S3 用 BLE 驱动盒马墨水价签（HINK-E029A14-A1 / 296x128 / 黑白红）
 *
 * 信息页排版（与电脑端 esl_page_v3.py 一致）：
 *
 *   ┌───────────────────────────────────────────┐
 *   │ 📶 已认证 ✓        晴 6C 63%RH 🔋 50%     │ 状态栏
 *   ├───────────────────────────────────────────┤ 色带（平时黑底/节日红底）
 *   │ ███ 10月29日 周四 ███████████████████████ │ 平时居中；节日左日期右谚语
 *   │ 日 一 二 三 四 五 六          ☀ 56x56     │ 日历 7 列 x 26px
 *   │        26 27 28 29 30 31  1                │ 周末红 / 节日红 / 今天反白
 *   │  2  3  4  5  6  7  8                       │
 *   │  9 10 11 12 13 14 15                       │
 *   └───────────────────────────────────────────┘
 *
 * 协议（逆向微信小程序）：
 *   1F1F 特征：E0 型号 / DD 对时 / E2 刷新 / E4 蓝牙名开关 / DF 保存
 *   4B646063：00 00 → 02 00 00 → 03+c+偏移+数据 → 01 01
 */
#include "esl_ble.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_bt.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "app_state.h"
#include "bsp_power.h"
#include "calendar.h"
#include "esl_font.h"
#include "time_weather.h"

/* 连接失败自动重试计数 */
static int s_retry;

static const char *TAG = "esl_ble";

#define ESL_W 296
#define ESL_H 128
#define ROW_BYTES (ESL_H / 8)
#define PLANE     (ESL_W * ROW_BYTES)
#define CHUNK     500    /* 上限 500：MTU 谈得大就用大分片，包数更少 */

#define INK_BLACK 0
#define INK_RED   1
#define INK_WHITE 2

static const ble_uuid128_t IMG_UUID = BLE_UUID128_INIT(
        0xfe, 0x82, 0xea, 0x56, 0x53, 0xe6, 0x41, 0x89,
        0xa7, 0xf3, 0x64, 0x62, 0x63, 0x60, 0x64, 0x4b);
static const ble_uuid128_t CTL_UUID = BLE_UUID128_INIT(
        0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
        0x00, 0x10, 0x00, 0x00, 0x1f, 0x1f, 0x00, 0x00);

static volatile esl_state_t s_state = ESL_IDLE;
static volatile char s_status[64] = "待机";

static uint8_t s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_img_chr = 0;
static uint16_t s_ctl_chr = 0;
static uint16_t s_bat_chr = 0;
static uint8_t  s_epd_bat = 0xFF;
static bool     s_scanning = false;

static uint8_t *s_img = NULL;
static uint8_t s_pkt[CHUNK + 8];
static int s_chunk = CHUNK;          /* 按协商到的 MTU 算，MTU-7，最大 CHUNK */
static int s_phase = 0;
static int s_off = 0;
static bool s_want_info = true;

/* main 每次状态变化时更新：已认证才允许推价签 */
static volatile bool esl_net_ready = false;

void esl_ble_set_net_ready(bool ready)
{
    if (esl_net_ready != ready) {
        ESP_LOGI(TAG, "网络%s，%s推价签", ready ? "就绪" : "未就绪",
                 ready ? "允许" : "拒绝");
    }
    esl_net_ready = ready;
}

/* 预渲染：画面提前画好，推送时只发数据，不在蓝牙会话里算 */
static int s_img_bat = -2;          /* 画面里画进去的电量（-2=未画过） */
static uint8_t *s_last_img = NULL;  /* 上次推送成功时的画面（用来判断"有没有变"） */
static bool s_last_img_valid = false;

/* 局部刷新：只发变化的数据段，然后发 01 00（厂商 webtool 的"局部更新"） */
static bool s_partial = false;
static int  s_pmin = 0;
static int  s_pend = 0;
static int64_t s_img_built_ms = 0;
static bool    s_img_built_info = true;
#define IMG_FRESH_MS   (10LL * 60 * 1000)   /* 画面 10 分钟内都算新鲜 */

/* 最近一次"认证成功"的时刻：墨水屏状态取最近 10 分钟的记录 */
static volatile int64_t s_last_authed_ms = 0;
#define AUTHED_WINDOW_MS  (10LL * 60 * 1000)

/* 整次推送的总超时：回调一丢也不会永远卡在扫描/连接/推图 */
static esp_timer_handle_t s_tmo = NULL;
/* 价签漏答时整轮重试的定时器 */
static esp_timer_handle_t s_retry_tmo = NULL;
/* 扫描超时后立刻重扫用的定时器（不能在 NimBLE 回调里直接重开扫） */
static esp_timer_handle_t s_scan_tmo = NULL;
static int s_retry_left = 0;
/* 主动断链（推图完成）标记，免得 DISCONNECT 把 DONE 改成 ERROR */
static bool s_finishing = false;

static bool s_stack_owned = false;   /* 协议栈是不是价签这边起的 */

static void fail_cleanup(const char *msg);
static void retry_cb(void *arg);
static void start_scan(void);

esl_state_t esl_ble_state(void) { return s_state; }
const char *esl_ble_status(void) { return (const char *) s_status; }

static void set_status(esl_state_t st, const char *msg)
{
    s_state = st;
    snprintf((char *) s_status, sizeof(s_status), "%s", msg);
    ESP_LOGI(TAG, "%s", msg);
}

/* ================================================================ 像素 */

static void buf_clear(void)
{
    memset(s_img, 0xFF, PLANE);
    memset(s_img + PLANE, 0x00, PLANE);
}

static inline int col_of(int x) { return ESL_W - 1 - x; }

static inline void put_px(int x, int y, bool red)
{
    if (x < 0 || x >= ESL_W || y < 0 || y >= ESL_H) {
        return;
    }
    int idx = col_of(x) * ROW_BYTES + (y >> 3);
    uint8_t bit = (uint8_t) (0x80 >> (y & 7));
    if (red) {
        s_img[PLANE + idx] |= bit;
    } else {
        s_img[idx] &= (uint8_t) ~bit;
    }
}

static inline void put_white(int x, int y)
{
    if (x < 0 || x >= ESL_W || y < 0 || y >= ESL_H) {
        return;
    }
    int idx = col_of(x) * ROW_BYTES + (y >> 3);
    uint8_t bit = (uint8_t) (0x80 >> (y & 7));
    s_img[idx] |= bit;
    s_img[PLANE + idx] &= (uint8_t) ~bit;
}

static void fill_rect(int x0, int y0, int x1, int y1, int ink)
{
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (ink == INK_WHITE) {
                put_white(x, y);
            } else {
                put_px(x, y, ink == INK_RED);
            }
        }
    }
}

static void fill_circle(int cx, int cy, int r, int ink)
{
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r) {
                if (ink == INK_WHITE) {
                    put_white(x, y);
                } else {
                    put_px(x, y, ink == INK_RED);
                }
            }
        }
    }
}

static void line(int x0, int y0, int x1, int y1, int w, int ink)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int n = (dx > dy ? dx : dy) + 1;
    for (int i = 0; i < n; i++) {
        int x = x0 + (x1 - x0) * i / (n > 1 ? n - 1 : 1);
        int y = y0 + (y1 - y0) * i / (n > 1 ? n - 1 : 1);
        for (int oy = 0; oy < w; oy++) {
            for (int ox = 0; ox < w; ox++) {
                if (ink == INK_WHITE) {
                    put_white(x + ox, y + oy);
                } else {
                    put_px(x + ox, y + oy, ink == INK_RED);
                }
            }
        }
    }
}

/* ================================================================ 文字 */

/* 取字符串前 maxc 个字符（UTF-8 安全）。日历格只有 26px，12px 字最多塞两个 */
static void utf8_head(const char *s, char *out, size_t cap, int maxc)
{
    size_t n = 0;
    int c = 0;
    const unsigned char *p = (const unsigned char *) s;
    while (*p && c < maxc) {
        int len = (*p < 0x80) ? 1
                  : (((*p & 0xE0) == 0xC0) ? 2 : (((*p & 0xF0) == 0xE0) ? 3 : 4));
        if (n + (size_t) len + 1 > cap) {
            break;
        }
        memcpy(out + n, p, (size_t) len);
        n += (size_t) len;
        p += len;
        c++;
    }
    out[n] = '\0';
}

static uint16_t next_cp(const char **p)
{
    const unsigned char *s = (const unsigned char *) *p;
    uint16_t cp;
    if (s[0] < 0x80) {
        cp = s[0];
        *p += 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        cp = (uint16_t) (((s[0] & 0x1F) << 6) | (s[1] & 0x3F));
        *p += 2;
    } else if ((s[0] & 0xF0) == 0xE0) {
        cp = (uint16_t) (((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F));
        *p += 3;
    } else {
        *p += 1;
        cp = '?';
    }
    return cp;
}

static const uint8_t *glyph16(uint16_t cp)
{
    for (int i = 0; i < FONT16_COUNT; i++) {
        if (font16x16[i].cp == cp) {
            return font16x16[i].bmp;
        }
    }
    return NULL;
}

static const uint8_t *glyph12(uint16_t cp)
{
    for (int i = 0; i < FONT12_COUNT; i++) {
        if (font12x12[i].cp == cp) {
            return font12x12[i].bmp;
        }
    }
    return NULL;
}

static inline void ink_at(int x, int y, int ink)
{
    if (ink == INK_WHITE) {
        put_white(x, y);
    } else {
        put_px(x, y, ink == INK_RED);
    }
}

static int draw8(int x, int y, uint8_t ch, int ink)
{
    if (ch < 0x20 || ch > 0x7E) {
        ch = '?';
    }
    const uint8_t *g = font8x16[ch - 0x20];
    for (int r = 0; r < 16; r++) {
        uint8_t row = g[r];
        for (int c = 0; c < 8; c++) {
            if (row & (0x80 >> c)) {
                ink_at(x + c, y + r, ink);
            }
        }
    }
    return 8;
}

/* 小字行专用：8x12 的 ASCII，和 font12x12 的汉字一样高 */
static int draw8s(int x, int y, uint8_t ch, int ink)
{
    if (ch < 0x20 || ch > 0x7E) {
        ch = '?';
    }
    const uint8_t *g = font8x12[ch - 0x20];
    for (int r = 0; r < 12; r++) {
        uint8_t row = g[r];
        for (int c = 0; c < 8; c++) {
            if (row & (0x80 >> c)) {
                ink_at(x + c, y + r, ink);
            }
        }
    }
    return 8;
}

static int draw16(int x, int y, uint16_t cp, int ink)
{
    const uint8_t *g = glyph16(cp);
    if (g == NULL) {
        return draw8(x, y, '?', ink) * 2;
    }
    for (int r = 0; r < 16; r++) {
        uint16_t row = ((uint16_t) g[r * 2] << 8) | g[r * 2 + 1];
        for (int c = 0; c < 16; c++) {
            if (row & (0x8000 >> c)) {
                ink_at(x + c, y + r, ink);
            }
        }
    }
    return 16;
}

static int draw12(int x, int y, uint16_t cp, int ink)
{
    const uint8_t *g = glyph12(cp);
    if (g == NULL) {
        return draw8(x, y, '?', ink);
    }
    for (int r = 0; r < 12; r++) {
        uint16_t row = ((uint16_t) g[r * 2] << 8) | g[r * 2 + 1];
        for (int c = 0; c < 12; c++) {
            if (row & (0x8000 >> c)) {
                ink_at(x + c, y + r, ink);
            }
        }
    }
    return 12;
}

/* 放大绘制：16x16 汉字点阵按 scale 倍放大（temp 用 2 倍数码牌效果） */
static int draw16_big(int x, int y, uint16_t cp, int ink, int scale)
{
    const uint8_t *g = glyph16(cp);
    if (g == NULL) {
        return 16 * scale;
    }
    for (int r = 0; r < 16; r++) {
        uint16_t row = ((uint16_t) g[r * 2] << 8) | g[r * 2 + 1];
        for (int c = 0; c < 16; c++) {
            if (row & (0x8000 >> c)) {
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        ink_at(x + c * scale + dx, y + r * scale + dy, ink);
                    }
                }
            }
        }
    }
    return 16 * scale;
}

/* 放大版文本（ASCII 用 8x16 字模，汉字用 16x16） */
static int text_big(const char *s, int x, int y, int ink, int scale)
{
    const char *p = s;
    while (*p) {
        uint16_t cp = next_cp(&p);
        if (cp < 0x80) {
            uint8_t ch = (uint8_t) cp;
            if (ch < 0x20 || ch > 0x7E) {
                ch = '?';
            }
            const uint8_t *g = font8x16[ch - 0x20];
            for (int r = 0; r < 16; r++) {
                uint8_t row = g[r];
                for (int c = 0; c < 8; c++) {
                    if (row & (0x80 >> c)) {
                        for (int dy = 0; dy < scale; dy++) {
                            for (int dx = 0; dx < scale; dx++) {
                                ink_at(x + c * scale + dx, y + r * scale + dy, ink);
                            }
                        }
                    }
                }
            }
            x += 8 * scale;
        } else {
            x += draw16_big(x, y, cp, ink, scale);
        }
    }
    return x;
}

static int text_w(const char *s)
{
    int w = 0;
    const char *p = s;
    while (*p) {
        uint16_t cp = next_cp(&p);
        w += (cp < 0x80) ? 8 : 16;
    }
    return w;
}

static int text(const char *s, int x, int y, int ink)
{
    const char *p = s;
    while (*p) {
        uint16_t cp = next_cp(&p);
        x += (cp < 0x80) ? draw8(x, y, (uint8_t) cp, ink)
                         : draw16(x, y, cp, ink);
    }
    return x;
}

static void text_right(const char *s, int xr, int y, int ink)
{
    text(s, xr - text_w(s), y, ink);
}

static int text12_w(const char *s)
{
    int w = 0;
    const char *p = s;
    while (*p) {
        uint16_t cp = next_cp(&p);
        w += (cp < 0x80) ? 8 : 12;
    }
    return w;
}

static void text12(const char *s, int x, int y, int ink)
{
    const char *p = s;
    while (*p) {
        uint16_t cp = next_cp(&p);
        x += (cp < 0x80) ? draw8s(x, y, (uint8_t) cp, ink)
                         : draw12(x, y, cp, ink);
    }
}

/* ================================================================ 小图标 */

static void icon_wifi(int x, int y, bool on, bool red)
{
    int ink = red ? INK_RED : INK_BLACK;
    /* 三段弧：用半圆近似 */
    for (int k = 0; k < 3; k++) {
        int r = 4 + k * 4;
        for (int deg = 210; deg <= 330; deg += 4) {
            float a = (float) deg * 3.14159265f / 180.0f;
            int px = x + 8 + (int) (cosf(a) * r);
            int py = y + 13 + (int) (sinf(a) * r);
            put_px(px, py, red);
        }
        (void) ink;
    }
    fill_circle(x + 8, y + 12, 1, red ? INK_RED : INK_BLACK);
    if (!on) {
        line(x + 2, y + 2, x + 14, y + 14, 2, red ? INK_RED : INK_BLACK);
    }
}

static void icon_check(int x, int y, int ink)
{
    line(x + 2, y + 8, x + 6, y + 12, 2, ink);
    line(x + 6, y + 12, x + 13, y + 3, 2, ink);
}

static void icon_cross(int x, int y, int ink)
{
    line(x + 3, y + 3, x + 12, y + 12, 2, ink);
    line(x + 12, y + 3, x + 3, y + 12, 2, ink);
}



static void icon_battery(int x, int y, int pct, bool red, int w, int h)
{
    int ink = red ? INK_RED : INK_BLACK;
    /* 外框 */
    line(x, y, x + w, y, 1, ink);
    line(x, y + h, x + w, y + h, 1, ink);
    line(x, y, x, y + h, 1, ink);
    line(x + w, y, x + w, y + h, 1, ink);
    /* 正极 */
    fill_rect(x + w + 1, y + h / 3, x + w + 2, y + h - h / 3, ink);
    if (pct < 0) {
        return;
    }
    if (pct > 100) {
        pct = 100;
    }
    int inner = w - 4;
    int fw = inner * pct / 100;
    if (fw > 0) {
        fill_rect(x + 2, y + 2, x + 1 + fw, y + h - 2, ink);
    }
}

/* ================================================================ 大天气图标 */

static void big_sun(int x, int y, int s, bool red)
{
    int ink = red ? INK_RED : INK_BLACK;
    int r = s / 3;
    int cx = x + s / 2, cy = y + s / 2;
    fill_circle(cx, cy, r, ink);
    for (int i = 0; i < 8; i++) {
        float a = (float) i * 3.14159265f / 4.0f;
        int x0 = cx + (int) (cosf(a) * (r + 3));
        int y0 = cy + (int) (sinf(a) * (r + 3));
        int x1 = cx + (int) (cosf(a) * (r + 9));
        int y1 = cy + (int) (sinf(a) * (r + 9));
        line(x0, y0, x1, y1, 2, ink);
    }
}

static void big_cloud(int x, int y, int s, int ink)
{
    fill_circle(x + s / 4, y + s / 2, s / 5, ink);
    fill_circle(x + s / 2, y + s / 3, s / 4, ink);
    fill_circle(x + 3 * s / 4, y + s / 2, s / 5, ink);
    fill_rect(x + s / 4, y + s / 2, x + 3 * s / 4, y + 3 * s / 5, ink);
}

static void big_sun_cloud(int x, int y, int s, bool red)
{
    big_sun(x, y - s / 8, s * 7 / 10, false);
    big_cloud(x + s / 6, y + s / 4, s * 3 / 4, red ? INK_RED : INK_BLACK);
}

static void big_rain(int x, int y, int s, bool red, int drops)
{
    big_cloud(x, y, s, INK_BLACK);
    int ink = red ? INK_RED : INK_BLACK;
    for (int i = 0; i < drops; i++) {
        int x0 = x + s / 4 + i * (s / 4);
        line(x0 + 3, y + s - s / 3, x0 - 2, y + s - 3, 2, ink);
    }
}

static void big_thunder(int x, int y, int s, bool red)
{
    big_cloud(x, y, s, INK_BLACK);
    int ink = red ? INK_RED : INK_BLACK;
    int cx = x + s / 2;
    line(cx + 4, y + s / 2, cx - 5, y + s - 4, 3, ink);
    line(cx - 5, y + s - 4, cx + 6, y + s / 2 + 6, 3, ink);
}

static void big_snow(int x, int y, int s, bool red)
{
    big_cloud(x, y, s, INK_BLACK);
    int ink = red ? INK_RED : INK_BLACK;
    for (int i = 0; i < 3; i++) {
        int cx = x + s / 4 + i * (s / 4);
        int cy = y + s - 6;
        line(cx - 3, cy, cx + 3, cy, 2, ink);
        line(cx, cy - 3, cx, cy + 3, 2, ink);
    }
}

static void big_fog(int x, int y, int s, bool red)
{
    big_cloud(x, y, s, INK_BLACK);
    int ink = red ? INK_RED : INK_BLACK;
    for (int i = 0; i < 3; i++) {
        int yy = y + s - s / 3 + i * (s / 10);
        line(x + s / 5 + (i % 2) * 3, yy, x + s - s / 5, yy, 2, ink);
    }
}

static void big_wind(int x, int y, int s, bool red)
{
    int ink = red ? INK_RED : INK_BLACK;
    for (int i = 0; i < 3; i++) {
        int yy = y + s / 3 + i * (s / 5);
        int w = s - 6 - i * 6;
        if (w < 6) {
            w = 6;
        }
        line(x + 3, yy, x + 3 + w, yy, 2, ink);
    }
    fill_circle(x + s - 8, y + s / 2, 4, ink);
}

static bool is_bad_weather(const char *desc)
{
    return desc && (strstr(desc, "雨") || strstr(desc, "雪") || strstr(desc, "雷")
                    || strstr(desc, "雾") || strstr(desc, "霾")
                    || strstr(desc, "风") || strstr(desc, "沙")
                    || strstr(desc, "尘") || strstr(desc, "暴"));
}

static void big_weather(int x, int y, int s, const char *desc)
{
    bool bad = is_bad_weather(desc);
    if (desc == NULL || *desc == 0) {
        big_sun(x, y, s, false);
    } else if (strstr(desc, "雷")) {
        big_thunder(x, y, s, true);
    } else if (strstr(desc, "雪")) {
        big_snow(x, y, s, true);
    } else if (strstr(desc, "暴") || strstr(desc, "大雨")) {
        big_rain(x, y, s, true, 3);
    } else if (strstr(desc, "雨")) {
        big_rain(x, y, s, true, 2);
    } else if (strstr(desc, "雾") || strstr(desc, "霾") || strstr(desc, "沙")
               || strstr(desc, "尘")) {
        big_fog(x, y, s, true);
    } else if (strstr(desc, "风")) {
        big_wind(x, y, s, true);
    } else if (strstr(desc, "阴")) {
        big_cloud(x, y, s, bad ? INK_RED : INK_BLACK);
    } else if (strstr(desc, "云")) {
        big_sun_cloud(x, y, s, false);
    } else {
        big_sun(x, y, s, false);
    }
}

/* ================================================================ 谚语 */

static const struct {
    const char *name;
    const char *prov;
} PROV[] = {
    { "立冬", "立冬补冬" }, { "小雪", "小雪封地" }, { "大雪", "大雪兆丰年" },
    { "冬至", "冬至吃饺子" }, { "元旦", "一元复始" }, { "小寒", "小寒大寒" },
    { "大寒", "大寒见三白" }, { "立春", "立春百草回" }, { "春节", "一年之计" },
    { "雨水", "雨水有雨" }, { "惊蛰", "惊蛰不耕田" },
    { "霜降", "霜降见霜" }, { "寒露", "寒露不露身" }, { "秋分", "秋分不生田" },
    { "清明", "清明前后 种瓜点豆" }, { "端午", "端午不插秧" },
    { "中秋", "八月十五月正圆" }, { "重阳", "重阳登高" },
    { "国庆", "举国同庆" }, { "劳动", "劳动最光荣" },
};

static const char *proverb_for(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(PROV) / sizeof(PROV[0]); i++) {
        if (strstr(name, PROV[i].name)) {
            return PROV[i].prov;
        }
    }
    return name;
}

/* ================================================================ 信息页 */

static void date_add(int *py, int *pm, int *pd, int delta)
{
    int y = *py, m = *pm, d = *pd;
    while (delta > 0) {
        d++;
        if (d > cal_days_in_month(y, m)) {
            d = 1;
            m++;
            if (m > 12) { m = 1; y++; }
        }
        delta--;
    }
    while (delta < 0) {
        d--;
        if (d < 1) {
            m--;
            if (m < 1) { m = 12; y--; }
            d = cal_days_in_month(y, m);
        }
        delta++;
    }
    *py = y; *pm = m; *pd = d;
}

/* 墨水屏加粗：每个黑点向右扩一格，细笔画不容易丢 */
__attribute__((unused)) static void dilate_planes(void)
{
    for (int i = 0; i < PLANE; i++) {
        uint8_t v = s_img[i];
        s_img[i] = (uint8_t) (v & ((v >> 1) | 0x80));
    }
}

/* 只在指定行范围加粗（跳过中间的日期色带，避免白字糊掉） */
__attribute__((unused)) static void bold_y(int y0, int y1)
{
    for (int x = 0; x + 1 < ESL_W; x++) {
        for (int y = y0; y < y1 && y < ESL_H; y++) {
            int idx = x * ROW_BYTES + (y >> 3);
            uint8_t bit = (uint8_t) (0x80 >> (y & 7));
            if (!(s_img[idx] & bit)) {
                s_img[(x + 1) * ROW_BYTES + (y >> 3)] &= (uint8_t) ~bit;
            }
        }
    }
}

/* 只画右上角的电量（百分比 + 电池图标）。
   预渲染时用的是上一轮读到的电量，连上价签读到新值后调它刷新一下。 */
static void draw_battery_only(void)
{
    char b[16];
    int bat = s_epd_bat;
    bool low = (bat <= 100 && bat < 20);
    int ink_bat = low ? INK_RED : INK_BLACK;

    /* 清掉右上角这一小块（背景是白的）：横向 80 像素、纵向 14 像素 */
    for (int y = 0; y < 14; y++) {
        for (int x = ESL_W - 80; x < ESL_W; x++) {
            put_white(x, y);
        }
    }

    int x = ESL_W - 4;
    if (bat <= 100) {
        snprintf(b, sizeof(b), "%d%%", bat);
    } else {
        snprintf(b, sizeof(b), "--");
    }
    text_right(b, x, 0, ink_bat);
    x -= text_w(b) + 5;
    icon_battery(x - 24, 2, bat <= 100 ? bat : -1, low, 24, 11);
    s_img_bat = bat;
}


/* 调试：一眼能认出来的测试图案（粗边框 + 条纹 + 大字） */
void esl_ble_draw_test_pattern(void)
{
    buf_clear();
    /* 粗边框（四条 4px 的边） */
    fill_rect(0, 0, ESL_W - 1, 3, INK_BLACK);
    fill_rect(0, ESL_H - 4, ESL_W - 1, ESL_H - 1, INK_BLACK);
    fill_rect(0, 0, 3, ESL_H - 1, INK_BLACK);
    fill_rect(ESL_W - 4, 0, ESL_W - 1, ESL_H - 1, INK_BLACK);
    /* 左侧黑白条纹 */
    for (int i = 0; i < 8; i++) {
        fill_rect(12, 14 + i * 13, 70, 14 + i * 13 + 6, INK_BLACK);
    }
    /* 品牌 + 测试字样 */
    text("\u767e\u5408\u793e YING", 84, 22, INK_BLACK);
    text("TEST", 84, 52, INK_RED);
    /* 底部一行小字说明这是测试页 */
    text("ESL TEST PAGE", 84, 96, INK_BLACK);
}

/* ---------------- 墨水屏界面自定义 ---------------- */
#define ESL_OPT_CAL     0x01   /* 日历 */
#define ESL_OPT_WEATHER 0x02   /* 天气面板 */
#define ESL_OPT_SUN     0x04   /* 日出日落 */
#define ESL_OPT_BRAND   0x08   /* 品牌 / 版本号 */

static uint8_t s_layout = 0;          /* 0 标准（日历+天气） 1 大字时间 */
static uint8_t s_opts = 0x0F;         /* 默认全开 */
static char    s_custom[48] = "";     /* 自定义一行字 */

void esl_ble_set_ui(uint8_t layout, uint8_t opts, const char *text)
{
    s_layout = (layout > 1) ? 0 : layout;
    s_opts = opts;
    s_custom[0] = '\0';
    if (text) {
        snprintf(s_custom, sizeof(s_custom), "%s", text);
    }
    ESP_LOGI(TAG, "墨水屏界面：布局 %d，开关 0x%02X，自定义「%s」", s_layout, s_opts, s_custom);
}

uint8_t esl_ble_ui_layout(void)
{
    return s_layout;
}

uint8_t esl_ble_ui_opts(void)
{
    return s_opts;
}

const char *esl_ble_ui_text(void)
{
    return s_custom;
}

/* 大字日期（16px 字模放大 2 倍）
   价签一天只刷一次，显示时间会一直停在推送那一刻 → 改成日期更实在 */
static void draw_big_date(int y)
{
    time_weather_t tw;
    memset(&tw, 0, sizeof(tw));
    time_weather_get(&tw);
    char tb[16];
    if (tw.time_valid) {
        snprintf(tb, sizeof(tb), "%d/%d", tw.month, tw.day);
    } else {
        snprintf(tb, sizeof(tb), "--/--");
    }
    int w = 0;
    for (const char *p = tb; *p; p++) {
        w += 16;                     /* 放大 2 倍后 ASCII 16px 宽 */
    }
    text_big(tb, (ESL_W - w) / 2, y, INK_BLACK, 2);

    /* 日期下面补一行星期，信息更完整 */
    if (tw.time_valid) {
        static const char *WD[] = { "日", "一", "二", "三", "四", "五", "六" };
        char wd[16];
        snprintf(wd, sizeof(wd), "星期%s", WD[(tw.weekday % 7 + 7) % 7]);
        text12(wd, (ESL_W - text12_w(wd)) / 2, y + 34, INK_BLACK);
    }
}

static void draw_info(void)
{
    buf_clear();


    time_weather_t tw;
    memset(&tw, 0, sizeof(tw));
    time_weather_get(&tw);

    char detail[64] = { 0 };
    app_state_t st = app_state_get(detail, sizeof(detail));
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (st == APP_ST_ONLINE) {
        s_last_authed_ms = now_ms;                 /* 记下来 */
    }
    /* 最近 10 分钟内认证过就算"已认证"，避免刚掉线时显示成"认证中" */
    bool authed = (st == APP_ST_ONLINE) ||
                  (s_last_authed_ms > 0 && (now_ms - s_last_authed_ms) <= AUTHED_WINDOW_MS);

    char buf[96];

    /* ---------------- 状态栏 ---------------- */
    if (authed) {
        icon_wifi(2, 0, true, false);
        text("已认证", 22, 0, INK_BLACK);
        icon_check(22 + text_w("已认证") + 4, 0, INK_BLACK);
    } else {
        icon_wifi(2, 0, false, true);
        text("认证中", 22, 0, INK_RED);
        icon_cross(22 + text_w("认证中") + 4, 0, INK_RED);
    }

    /* 右侧从右往左：百分比 → 电池 → 湿度 → 温度 → 天气文字 */
    int x = ESL_W - 4;
    bool low = (s_epd_bat <= 100 && s_epd_bat < 20);
    int ink_bat = low ? INK_RED : INK_BLACK;

    if (s_epd_bat <= 100) {
        snprintf(buf, sizeof(buf), "%d%%", s_epd_bat);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    text_right(buf, x, 0, ink_bat);
    x -= text_w(buf) + 5;
    icon_battery(x - 24, 2, s_epd_bat <= 100 ? s_epd_bat : -1, low, 24, 11);
    /* 和电池之间留 28px：原来 12px 时第三个字会被电池压住 */
    x -= 24 + 28;

    /* 电池左边：品牌「百合社」（可关） */
    if (s_opts & ESL_OPT_BRAND) {
        const char *BRAND = "\u767e\u5408\u793e";
        int bw = text_w(BRAND);
        text(BRAND, x - bw, 0, INK_BLACK);
        x -= bw + 8;
    }

    /* 天气不再挤在状态栏，统一放到右下角的天气面板里 */

    /* ---------------- 日期色带 ---------------- */
    const int bar_y0 = 19, bar_y1 = 38;

    cal_day_t cdt;
    const char *today_name = NULL;
    if (tw.time_valid && cal_lookup(tw.year, tw.month, tw.day, &cdt)) {
        today_name = cdt.name;
    }

    int wdi = tw.time_valid ? ((tw.weekday % 7 + 7) % 7) : 0;

    if (tw.time_valid) {
        static const char *WD_EN[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        snprintf(buf, sizeof(buf), "%s %d/%d", WD_EN[wdi], tw.month, tw.day);
    } else {
        snprintf(buf, sizeof(buf), "时间未同步");
    }

    fill_rect(0, bar_y0, ESL_W - 1, bar_y1, today_name ? INK_RED : INK_BLACK);

    /* 色带右侧：固件版本 + 作者（白字小号，空间充足不会被裁） */
    {
        const esp_app_desc_t *ad = esp_app_get_description();
        char vb[48];
        snprintf(vb, sizeof(vb), "v%s YING", (ad && ad->version[0]) ? ad->version : "?");
        int vw = text12_w(vb);
        text12(vb, ESL_W - 16 - vw, bar_y0 + 4, INK_WHITE);   /* 右边留 16px，防面板边缘死区 */
    }
    if (today_name) {
        /* 节日当天：日期靠左，谚语靠右 */
        text(buf, 8, bar_y0 + 2, INK_WHITE);
        const char *pv = proverb_for(today_name);
        if (pv) {
            text_right(pv, ESL_W - 8, bar_y0 + 2, INK_WHITE);
        }
    } else {
        /* 平时：靠左（右侧留给版本号） */
        text(buf, 6, bar_y0 + 2, INK_WHITE);
    }

    /* 大字时间布局：时间顶替日历区 */
    if (s_layout == 1) {
        draw_big_date(40);
        if (s_custom[0]) {
            int cw = text12_w(s_custom);
            text12(s_custom, (ESL_W - cw) / 2, 88, INK_BLACK);
        }
        if (s_opts & ESL_OPT_WEATHER) {
            if ((s_opts & ESL_OPT_WEATHER) && tw.weather_valid) {
                snprintf(buf, sizeof(buf), "%dC HUM %d", tw.temp_c, tw.humidity);
                int w = text12_w(buf);
                text12(buf, (ESL_W - w) / 2, 104, INK_BLACK);
            }
        }
        if ((s_opts & ESL_OPT_SUN) && tw.daily_valid && tw.sunrise[0]) {
            snprintf(buf, sizeof(buf), "日出%s 日落%s", tw.sunrise, tw.sunset);
            int w = text12_w(buf);
            text12(buf, (ESL_W - w) / 2, 116, INK_BLACK);
        }
        return;
    }


    /* ---------------- 日历 ---------------- */
    const int cal_x = 2, col_w = 24, y0 = 42;
    if (s_opts & ESL_OPT_CAL) {

    static const char *WD_AB[] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    for (int i = 0; i < 7; i++) {
        const char *c = WD_AB[i];
        int w = text_w(c);
        text(c, cal_x + i * col_w + (col_w - w) / 2, y0,
             (i == 0 || i == 6) ? INK_RED : INK_BLACK);
    }

    if (tw.time_valid) {
        for (int week = 0; week < 3; week++) {
            int yb = y0 + 22 + week * 22 + 2;
            int delta0 = (week - 1) * 7 - wdi;
            for (int d = 0; d < 7; d++) {
                int yy = tw.year, mm = tw.month, dd = tw.day;
                date_add(&yy, &mm, &dd, delta0 + d);

                bool today = (delta0 + d) == 0;
                bool weekend = (d == 0 || d == 6);
                cal_day_t cd;
                bool fest = cal_lookup(yy, mm, dd, &cd) && cd.name != NULL;

                int cx = cal_x + d * col_w;
                if (today && fest) {
                    /* 节日当天：整格换成节日名（12px 塞进本格）。
                       格子只有 26px，最多放两个字，"万圣夜" 就显示成 "万圣"，
                       完整名字在顶部色条右侧（没谚语时显示名字本身） */
                    fill_rect(cx - 1, yb - 1, cx + col_w - 3, yb + 16, INK_BLACK);
                    char nm[12];
                    utf8_head(cd.name, nm, sizeof(nm), 2);
                    int nw = text12_w(nm);
                    text12(nm, cx + (col_w - nw) / 2, yb + 1, INK_WHITE);
                } else if (today) {
                    fill_rect(cx - 1, yb - 1, cx + col_w - 3, yb + 16, INK_BLACK);
                    snprintf(buf, sizeof(buf), "%d", dd);
                    int nw = text_w(buf);
                    text(buf, cx + (col_w - nw) / 2, yb, INK_WHITE);
                } else {
                    snprintf(buf, sizeof(buf), "%d", dd);
                    int nw = text_w(buf);
                    int ink = (fest || weekend) ? INK_RED : INK_BLACK;
                    text(buf, cx + (col_w - nw) / 2, yb, ink);
                }
            }
        }
    }

    }

    /* ---------------- 右下角天气面板 ----------------
       竖分隔线 + 图标(32) + 温度(16px) + 天气(12px) + 湿度/明天(12px)
       注意：字库只有 ASCII + 汉字，没有 ° 符号，温度写成 "14C" */
    {
        const int px0 = 172;              /* 面板左边界（日历 7 列 x24 到 170） */
        const int py0 = 42;
        const int pw = ESL_W - px0;   /* 124 */       /* 面板宽度 124 */

        /* 细竖线，把日历和天气分开 */
        for (int y = py0; y < ESL_H - 2; y++) {
            put_px(px0 - 1, y, false);
        }

        if (tw.weather_valid) {
            /* A 方案（天气卡片）
               ┌ 5 C            ☁ ┐   大温度(2 倍数码牌) + 图标 40px
               │ 小雨        HUM 89 │   天气词(红) + 湿度右对齐
               ├───────────────────┤   细分隔线
               │ 日出7:09  日落17:18│   12px 一行
               └───────────────────┘ */
            const int isz = 40;
            big_weather(px0 + pw - isz - 2, py0 + 2, isz, tw.desc);

            ESP_LOGI(TAG, "面板文字: desc=%s temp=%d hum=%d 日出=%s 日落=%s",
                     tw.desc ? tw.desc : "(null)", tw.temp_c, tw.humidity,
                     tw.sunrise, tw.sunset);

            /* 大温度：16px 字模放大 2 倍 → 32px 高，10 和负数也放得下 */
            {
                char tb[8];
                snprintf(tb, sizeof(tb), "%d", tw.temp_c);
                int bw = 0;
                for (const char *q = tb; *q; q++) {
                    bw += 16;                    /* ASCII 放大 2 倍后 16px/字 */
                }
                text_big(tb, px0 + 4, py0 + 2, INK_BLACK, 2);
                text("C", px0 + 4 + bw + 2, py0 + 18, INK_BLACK);
            }

            /* 天气词（16px，坏天气红色），左下方 */
            if (tw.desc) {
                char d5[24];
                int maxc = 4;
                const int left_w = pw - 40;
                utf8_head(tw.desc, d5, sizeof(d5), maxc);
                while (maxc > 1 && text_w(d5) > left_w) {
                    maxc--;
                    utf8_head(tw.desc, d5, sizeof(d5), maxc);
                }
                int ink = is_bad_weather(tw.desc) ? INK_RED : INK_BLACK;
                text(d5, px0 + 6, py0 + 44, ink);
            }

            /* 湿度右对齐 */
            {
                char hb[16];
                snprintf(hb, sizeof(hb), "HUM %d", tw.humidity);
                int hw = text12_w(hb);
                text12(hb, px0 + pw - 4 - hw, py0 + 48, INK_BLACK);
            }

            /* 细分隔线 */
            for (int x = px0 + 4; x <= px0 + pw - 4; x++) {
                put_px(x, py0 + 64, false);
            }

            /* 日出 / 日落：一行横排（可用 SET esl_opts 关掉）
               12px 字库里 ASCII 是 8px 宽，两个"日X小时:分"正好 120px，
               面板 124px 放得下；万一放不下（时间带前导 0 等）自动换成单字标签 */
            if (s_opts & ESL_OPT_SUN) {
            {
                char s1[24], s2[24];
                int sh = 0, sm = 0, eh = 0, em = 0;
                bool ok1 = tw.daily_valid && tw.sunrise[0] != '\0';
                bool ok2 = tw.daily_valid && tw.sunset[0] != '\0';
                if (ok1) {
                    sscanf(tw.sunrise, "%d:%d", &sh, &sm);
                }
                if (ok2) {
                    sscanf(tw.sunset, "%d:%d", &eh, &em);
                }
                if (ok1) {
                    snprintf(s1, sizeof(s1), "日出%d:%02d", sh, sm);
                } else {
                    snprintf(s1, sizeof(s1), "日出--:--");
                }
                if (ok2) {
                    snprintf(s2, sizeof(s2), "日落%d:%02d", eh, em);
                } else {
                    snprintf(s2, sizeof(s2), "日落--:--");
                }
                int w1 = text12_w(s1), w2 = text12_w(s2);
                if (w1 + w2 + 2 > pw) {
                    /* 还是太宽：标签缩成一个字，保证不撞边 */
                    if (ok1) {
                        snprintf(s1, sizeof(s1), "升%d:%02d", sh, sm);
                    } else {
                        snprintf(s1, sizeof(s1), "升--:--");
                    }
                    if (ok2) {
                        snprintf(s2, sizeof(s2), "落%d:%02d", eh, em);
                    } else {
                        snprintf(s2, sizeof(s2), "落--:--");
                    }
                    w1 = text12_w(s1);
                    w2 = text12_w(s2);
                }
                int x1 = px0 + (pw - (w1 + w2 + 2)) / 2;
                if (x1 < px0 + 1) {
                    x1 = px0 + 1;
                }
                text12(s1, x1, py0 + 70, INK_BLACK);
                text12(s2, x1 + w1 + 2, py0 + 70, INK_BLACK);
            }
            }
        } else {
            /* 没天气数据：面板中间写一行提示，不画框（框看起来像乱码） */
            const char *t = "暂无天气";
            int tw2 = text12_w(t);
            text12(t, px0 + (pw - tw2) / 2, py0 + 34, INK_BLACK);
            const char *t2 = "联网后更新";
            int t2w = text12_w(t2);
            text12(t2, px0 + (pw - t2w) / 2, py0 + 54, INK_BLACK);
        }
    }
}

/* 几何测试卡（排查方向/颜色） */
static void draw_test_card(void)
{
    buf_clear();
    fill_rect(0, 0, ESL_W - 1, ESL_H - 1, INK_BLACK);
    fill_rect(1, 1, ESL_W - 2, ESL_H - 2, INK_WHITE);
    fill_rect(8, 8, 140, 44, INK_WHITE);
    fill_rect(14, 14, 134, 38, INK_RED);
    for (int i = 8; i <= 140; i += 4) {
        put_px(i, 8, false);
        put_px(i, 44, false);
    }
    fill_rect(12, 56, 44, 88, INK_BLACK);
    fill_rect(ESL_W - 56, ESL_H - 48, ESL_W - 16, ESL_H - 12, INK_RED);
    for (int i = 0; i < 18; i++) {
        int x = 160 + i * 7;
        fill_rect(x, 56, x + 2, 100, INK_BLACK);
    }
}

/* ================================================================ 发送 */

static void compute_partial(void);
static void send_next(void);

static int on_write(uint16_t conn, const struct ble_gatt_error *e,
                    struct ble_gatt_attr *a, void *arg)
{
    if (e && e->status != 0) {
        ESP_LOGW(TAG, "写失败 %d", e->status);
        fail_cleanup("写失败");
        return 0;
    }
    send_next();
    return 0;
}

static void issue(uint16_t chr, const uint8_t *data, uint16_t len)
{
    if (ble_gattc_write_flat(s_conn, chr, data, len, on_write, NULL) != 0) {
        fail_cleanup("写请求失败");
    }
}

/* 统一失败收尾：停超时、停扫描、断链、清进度，保证状态一定能落地 */
static void fail_cleanup(const char *msg)
{
    if (s_tmo) {
        esp_timer_stop(s_tmo);
    }
    if (s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        s_finishing = true;
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    s_phase = 0;
    s_off = 0;

    /* 价签的 BLE 偶尔会漏答（MTU 交换或者第一次图片写 30 秒没回）。
       这种失败整轮重试通常能成功，别一次就把整次推送判死 */
    if (msg && s_retry_left > 0 && s_state != ESL_DONE && ble_hs_synced()) {
        s_retry_left--;
        ESP_LOGW(TAG, "%s，3 秒后整轮重试（还剩 %d 次）", msg, s_retry_left);
        set_status(ESL_CONNECTING, "重试中");
        if (s_retry_tmo) {
            esp_timer_stop(s_retry_tmo);
            esp_timer_start_once(s_retry_tmo, 3LL * 1000 * 1000);
        }
        return;
    }

    if (msg && s_state != ESL_DONE) {
        set_status(ESL_ERROR, msg);
    }
}

/* 5 秒后重新扫描整轮重来 */
static void retry_cb(void *arg)
{
    (void) arg;

    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        /* 断链还没完成，2 秒后再看 */
        if (s_retry_tmo) {
            esp_timer_start_once(s_retry_tmo, 2LL * 1000 * 1000);
        }
        return;
    }

    ESP_LOGW(TAG, "重试：重新扫描价签");
    if (s_tmo) {
        esp_timer_stop(s_tmo);
        esp_timer_start_once(s_tmo, 40LL * 1000 * 1000);
    }
    start_scan();
}

static void tmo_cb(void *arg)
{
    (void) arg;
    ESP_LOGW(TAG, "整次推送超时，强制收尾");
    fail_cleanup("推送超时");
}

static void send_next(void)
{
    /* BLE_HS_CONN_HANDLE_NONE 是 0xFFFF 而不是 0，不能写 if (!s_conn) */
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    switch (s_phase) {
    case 0:
        /* 小程序连上价签后的第一件事就是发 F1（读设备信息），
           实测少了这个握手，价签对后面的图片写不理不睬 */
        set_status(ESL_PUSHING, "推图：握手");
        {
            uint8_t f1 = 0xF1;
            s_phase = 3;
            issue(s_ctl_chr, &f1, 1);
        }
        return;
    case 1:
        {
            time_t now = time(NULL);
            uint32_t ts = (now > 1600000000) ? (uint32_t) now + 28800 : 0;
            uint8_t tb[5] = { 0xDD, (uint8_t) (ts >> 24), (uint8_t) (ts >> 16),
                              (uint8_t) (ts >> 8), (uint8_t) ts };
            s_phase = 2;
            issue(s_ctl_chr, tb, 5);
        }
        return;
    case 2:
        /* 原来这里会先发一个 E2（刷新）。实测这一步会让价签立刻整屏刷新，
           刷新期间它不处理后面的图片写，30 秒后 ATT 超时，整次推送失败。
           真正让价签出图的 E2 在 case 8 已经发过一次，这里不再重复 */
        s_phase = 3;
        send_next();
        return;
    case 3:
        set_status(ESL_PUSHING, "进入图片模式");
        {
            uint8_t em[2] = { 0xE1, 0x00 };
            s_phase = 30;
            issue(s_ctl_chr, em, 2);
        }
        return;
    case 30:
        set_status(ESL_PUSHING, "推图：传输");
        {
            uint8_t st2[2] = { 0x00, 0x00 };
            s_phase = 4;
            issue(s_img_chr, st2, 2);
        }
        return;
    case 4:
        {
            uint8_t pr[3] = { 0x02, 0x00, 0x00 };
            s_phase = 5;
            s_off = s_partial ? s_pmin : 0;
            issue(s_img_chr, pr, 3);
        }
        return;
    case 5:
    case 6:
        {
            int plane = (s_phase == 5) ? 0 : 1;
            uint8_t c = (plane == 0) ? 0xFF : 0x00;
            const uint8_t *src = s_img + plane * PLANE;
            int limit = s_partial ? s_pend : PLANE;
            int n = (limit - s_off) < s_chunk ? (limit - s_off) : s_chunk;
            if (n <= 0) {
                s_phase++;
                s_off = s_partial ? s_pmin : 0;
                send_next();
                return;
            }
            /* 局部刷新时：这一块和上次推出去的内容一模一样就跳过，不发 */
            if (s_partial && s_last_img != NULL) {
                const uint8_t *oldp = s_last_img + plane * PLANE + s_off;
                if (memcmp(src + s_off, oldp, (size_t) n) == 0) {
                    s_off += n;
                    if (s_off >= limit) {
                        s_phase++;
                        s_off = s_partial ? s_pmin : 0;
                    }
                    send_next();          /* 跳过这一块，直接处理下一块 */
                    return;
                }
            }
            s_pkt[0] = 0x03;
            s_pkt[1] = c;
            s_pkt[2] = (uint8_t) (s_off >> 8);
            s_pkt[3] = (uint8_t) s_off;
            memcpy(s_pkt + 4, src + s_off, (size_t) n);
            s_off += n;
            if (s_off >= limit) {
                s_phase++;
                s_off = s_partial ? s_pmin : 0;
            }
            issue(s_img_chr, s_pkt, (uint16_t) (n + 4));
        }
        return;
    case 7:
        {
            /* 厂商 webtool：01 01 = 全屏更新，01 00 = 局部更新 */
            uint8_t cm[2] = { 0x01, (uint8_t) (s_partial ? 0x00 : 0x01) };
            s_phase = 9;
            ESP_LOGI(TAG, "%s", s_partial ? "推图：局部刷新（只刷变化区）" : "推图：整屏刷新");
            issue(s_img_chr, cm, 2);
        }
        return;
    case 8:
        s_phase = 9;
        send_next();
        return;
    case 9:
        /* 记下这次推出去的画面，下次内容一样就不用再刷 */
        if (s_last_img == NULL) {
            s_last_img = heap_caps_malloc(PLANE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (s_last_img != NULL && s_img != NULL) {
            memcpy(s_last_img, s_img, (size_t) (PLANE * 2));
            s_last_img_valid = true;
        }

        /* 刷新指令已发完：这次推送到此为止，主动断链并把状态落地。
           以前这里只把 s_phase 改成 10 就 return，没有后续 GATT 操作，
           就再也不会有 on_write 回调，状态会永远停在「刷新中」 */
        s_phase = 0;
        if (s_tmo) {
            esp_timer_stop(s_tmo);
        }
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            s_finishing = true;
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        set_status(ESL_DONE, "推图完成");
        return;
    default:
        set_status(ESL_DONE, "推图完成");
        return;
    }
}

/* ================================================================ 发现 */

static int on_bat_read(uint16_t conn, const struct ble_gatt_error *e,
                       struct ble_gatt_attr *attr, void *arg)
{
    if (e && e->status == 0 && attr && attr->om
        && OS_MBUF_PKTLEN(attr->om) >= 1) {
        uint8_t v = 0;
        os_mbuf_copydata(attr->om, 0, 1, &v);
        s_epd_bat = v;
        ESP_LOGI(TAG, "价签电量 %u%%", v);
    } else {
        s_epd_bat = 0xFF;
    }
    /* 优先用预渲染好的画面；没有或太旧才现画 */
    {
        int64_t now2 = esp_timer_get_time() / 1000;
        bool fresh = (s_img_built_ms > 0) && (s_img_built_info == s_want_info) &&
                     (now2 - s_img_built_ms <= IMG_FRESH_MS);
        if (!fresh) {
            ESP_LOGW(TAG, "没有新鲜的预渲染画面，现场画一张");
            if (s_want_info) {
                draw_info();
            } else {
                draw_test_card();
            }
            s_img_built_ms = now2;
            s_img_built_info = s_want_info;
        } else {
            ESP_LOGI(TAG, "用预渲染画面（%.0f 秒前生成）", (now2 - s_img_built_ms) / 1000.0);
            /* 画面里的电量可能是上一轮的：这次读到了新值就只重画那一小块 */
            if (s_want_info && s_img_bat != (int) s_epd_bat) {
                /* 局部发送已禁用，实际是整屏推送，日志要写实 */
                ESP_LOGI(TAG, "电量变化（%d%% → %d%%），重画电量区域后整屏推送",
                         s_img_bat, (int) s_epd_bat);
                draw_battery_only();
                compute_partial();      /* 电量区域变了，局部范围要跟着更新 */
            }
        }
    }

    /* 全页膨胀会让色带里的白字糊掉，先关掉；
       需要加粗时只对白底黑字的部分做 */
    /* 按小程序的实际流程走：连上后先发 F1 握手（小程序在订阅通知后立刻发 F1，
       价签会回一段带 MAC 的通知），然后再 0000 → 020000 → 数据 → 0101。
       E0（设面板）/DD（对时）/E2（刷新）不在推图流程里，先发它们会让价签
       进入刷新/面板切换，后面的图片写 30 秒没人回。 */
    s_phase = 0;
    send_next();
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *e,
                  const struct ble_gatt_chr *chr, void *arg)
{
    if (e->status == 0 && chr) {
        if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
            ESP_LOGI(TAG, "  特征 16位 0x%04x 句柄 %u", chr->uuid.u16.value, chr->val_handle);
        } else {
            const ble_uuid128_t *u128 = (const ble_uuid128_t *) &chr->uuid;
            char hx[3 * 16 + 1];
            for (int k = 0; k < 16; k++) snprintf(hx + k * 3, 4, "%02x ", u128->value[k]);
            hx[48] = 0;
            int c1 = ble_uuid_cmp(&chr->uuid.u, &IMG_UUID.u);
            int c2 = ble_uuid_cmp(&chr->uuid.u, &CTL_UUID.u);
            ESP_LOGI(TAG, "  特征 128位 句柄 %u  UUID=%s", chr->val_handle, hx);
            ESP_LOGI(TAG, "    type: chr=%d img=%d  cmp_img=%d cmp_ctl=%d",
                     chr->uuid.u.type, IMG_UUID.u.type, c1, c2);
        }
        /* 直接比 16 字节：ble_uuid_cmp 在这块价签上会误判 */
        const uint8_t *uval = ((const ble_uuid128_t *) &chr->uuid)->value;
        if (chr->uuid.u.type == BLE_UUID_TYPE_128
            && memcmp(uval, IMG_UUID.value, 16) == 0) {
            s_img_chr = chr->val_handle;
        } else if (chr->uuid.u.type == BLE_UUID_TYPE_128
                   && memcmp(uval, CTL_UUID.value, 16) == 0) {
            s_ctl_chr = chr->val_handle;
        } else if (chr->uuid.u.type == BLE_UUID_TYPE_16
                   && chr->uuid.u16.value == 0x1F1F) {
            /* 控制特征在本机上是 16 位形式（00001f1f-... 的短写法） */
            s_ctl_chr = chr->val_handle;
        } else if (chr->uuid.u.type == BLE_UUID_TYPE_16
                   && chr->uuid.u16.value == 0x2A19) {
            s_bat_chr = chr->val_handle;
        }
    } else if (e->status == BLE_HS_EDONE) {
        if (!s_img_chr || !s_ctl_chr) {
            fail_cleanup("没找到价签特征");
            return 0;
        }
        ESP_LOGI(TAG, "特征 img=%u ctl=%u bat=%u", s_img_chr, s_ctl_chr, s_bat_chr);
        s_epd_bat = 0xFF;
        if (s_bat_chr) {
            if (ble_gattc_read(conn, s_bat_chr, on_bat_read, NULL) != 0) {
                /* 读不到电量不影响推图，直接画 */
                on_bat_read(conn, NULL, NULL, NULL);
            }
        } else {
            on_bat_read(conn, NULL, NULL, NULL);
        }
    } else if (e->status != 0) {
        /* 发现过程中的其它 GATT 错误：以前会被吞掉，然后停在"连接中" */
        fail_cleanup("特征发现失败");
    }
    return 0;
}

__attribute__((unused)) static int svc_cb(uint16_t conn, const struct ble_gatt_error *e,
                  const struct ble_gatt_svc *svc, void *arg);

/* MTU 协商完成后才能发现 128 位 UUID 的服务 */
static int esl_mtu_cb(uint16_t conn, const struct ble_gatt_error *e, uint16_t mtu, void *arg)
{
    int st = e ? e->status : 0;
    ESP_LOGI(TAG, "MTU 协商：%u（err %d）", mtu, st);
    if (st != 0) {
        /* 价签没回 MTU：通常是对端没准备好（手机还连着它 / 价签刚醒）。
           分片固定 132 字节，MTU 没谈成后面的写必然失败，
           所以直接收尾，别在一条已经不可用的链路上继续发现特征 */
        fail_cleanup("MTU 协商失败（手机还连着价签？）");
        return 0;
    }
    {
        int c = (int) mtu - 7;
        if (c < 20) c = 20;
        if (c > CHUNK) c = CHUNK;
        s_chunk = c;
        ESP_LOGI(TAG, "分片 %d 字节（MTU %u）", s_chunk, mtu);
    }
    set_status(ESL_CONNECTING, "发现特征");
    /* 这个价签的"按组发现服务"在句柄 15 之后会中断，
       直接遍历全部特征值更可靠 */
    if (ble_gattc_disc_all_chrs(conn, 1, 0xFFFF, chr_cb, NULL) != 0) {
        fail_cleanup("特征发现启动失败");
    }
    return 0;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *e,
                  const struct ble_gatt_svc *svc, void *arg)
{
    if (e->status == 0 && svc) {
        if (svc->uuid.u.type == BLE_UUID_TYPE_16) {
            ESP_LOGI(TAG, "服务 16位 UUID=0x%04x 句柄 %u~%u", svc->uuid.u16.value,
                     svc->start_handle, svc->end_handle);
        } else {
            ESP_LOGI(TAG, "服务 128位 句柄 %u~%u", svc->start_handle, svc->end_handle);
        }
        ble_gattc_disc_all_chrs(conn, svc->start_handle, svc->end_handle,
                                chr_cb, NULL);
        return 0;
    }
    if (e->status == BLE_HS_EDONE) {
        if (!s_img_chr || !s_ctl_chr) {
            set_status(ESL_ERROR, "没找到价签特征");
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        ESP_LOGI(TAG, "特征 img=%u ctl=%u bat=%u", s_img_chr, s_ctl_chr, s_bat_chr);
        s_epd_bat = 0xFF;
        if (s_bat_chr) {
            ble_gattc_read(conn, s_bat_chr, on_bat_read, NULL);
        } else {
            on_bat_read(conn, NULL, NULL, NULL);
        }
    }
    return 0;
}

static int gap_event(struct ble_gap_event *ev, void *arg);

/* 懒创建扫描重试定时器 */
static void scan_tmo_cb(void *arg)
{
    (void) arg;
    if (s_state == ESL_SCANNING) {
        start_scan();
    }
}

static void ensure_scan_tmo(void)
{
    if (s_scan_tmo != NULL) {
        return;
    }
    const esp_timer_create_args_t a = {
        .callback = scan_tmo_cb,
        .name = "esl_scan",
    };
    if (esp_timer_create(&a, &s_scan_tmo) != ESP_OK) {
        ESP_LOGW(TAG, "扫描重试定时器创建失败");
    }
}

static void start_scan(void)
{
    ensure_scan_tmo();
    struct ble_gap_disc_params p;
    memset(&p, 0, sizeof(p));
    p.filter_duplicates = 1;
    /* 射频优先给 WiFi：扫描占空比降到 30%（100ms 间隔里只扫 30ms），
       并且只扫 15 秒，找不到就让用户按一下价签再来 */
    /* 100% 占空比连续扫描：itvl = window = 50ms。
       原来 100ms/30ms（30%）实测要重试两次、多等 90 秒才搜到价签；
       推图一天一次、扫描只需一两秒，优先保证"一搜就有"。 */
    p.itvl = 80;         /* 50ms（单位 0.625ms） */
    p.window = 80;       /* 50ms，占空比 100% */
    if (s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
    }
    set_status(ESL_SCANNING, "扫描价签中");
    if (ble_gap_disc(s_own_addr_type, 8000, &p, gap_event, NULL) != 0) {
        fail_cleanup("扫描失败");
        return;
    }
    s_scanning = true;
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *d = &ev->disc;
        if (d->length_data == 0) {
            return 0;
        }
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) {
            return 0;
        }
        /* 认价签：广播名以 NRF 开头，或者广播里带这个价签的服务 UUID。
           名字被 E4 关掉以后就只能靠 UUID（再不行靠已知 MAC 兜底） */
        static const uint8_t kSvcUuid[16] = {
            0x38, 0x9a, 0x7d, 0x21, 0xd3, 0x83, 0x4e, 0x04,
            0xba, 0xa3, 0xa9, 0xeb, 0x10, 0x7b, 0x18, 0x13,
        };  /* 13187b10-eba9-a3ba-044e-83d3217d9a38，小端 */
        /* 这台价签的 MAC（AA:BB:CC:DD:EE:FF），隐藏蓝牙名后靠它兜底 */
        static const uint8_t kMac[6] = { 0, 0, 0, 0, 0, 0 };   /* 改成你的价签 MAC（小端） */

        bool match = false;
        if (f.name != NULL && f.name_len >= 3 && memcmp(f.name, "NRF", 3) == 0) {
            match = true;
        } else if (f.uuids128 != NULL) {
            for (int i = 0; i < f.num_uuids128 && !match; i++) {
                if (memcmp(f.uuids128[i].value, kSvcUuid, 16) == 0) {
                    match = true;
                }
            }
        }
        if (!match && memcmp(d->addr.val, kMac, 6) == 0) {
            match = true;
        }
        if (!match) {
            return 0;
        }

        char nm[32] = { 0 };
        int n = (f.name_len < 31) ? f.name_len : 31;
        if (f.name != NULL) {
            memcpy(nm, f.name, (size_t) n);
        } else {
            strlcpy(nm, "(无名)", sizeof(nm));
        }
        ESP_LOGI(TAG, "发现 %s (%02x:%02x:%02x:%02x:%02x:%02x, %d dBm)", nm,
                 d->addr.val[5], d->addr.val[4], d->addr.val[3],
                 d->addr.val[2], d->addr.val[1], d->addr.val[0], d->rssi);
        if (s_scanning) {
            ble_gap_disc_cancel();
            s_scanning = false;
        }
        set_status(ESL_CONNECTING, "连接价签");
        if (ble_gap_connect(s_own_addr_type, &d->addr, 4000, NULL, gap_event, NULL) != 0) {
            fail_cleanup("连接发起失败");
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        /* 8 秒扫描到期还没搜到：立刻重来，别干等 40 秒总超时。
           价签的广播间隔可能比较长，连续重扫才能尽快撞上它。 */
        s_scanning = false;
        if (s_scan_tmo != NULL && s_state == ESL_SCANNING) {
            ESP_LOGW(TAG, "扫描到期没搜到，100ms 后重新扫描");
            esp_timer_stop(s_scan_tmo);
            esp_timer_start_once(s_scan_tmo, 100LL * 1000);
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_retry = 0;
            ESP_LOGI(TAG, "已连接 %u（先协商 MTU）", s_conn);
            set_status(ESL_CONNECTING, "协商 MTU");
            ble_gattc_exchange_mtu(s_conn, esl_mtu_cb, NULL);
        } else if (s_retry < 5) {
            s_retry++;
            /* 连接超时通常是价签已经睡回去了，重连同一个地址没用，
               必须重新扫描等它下一次广播。4 秒一轮，比原来 15 秒快得多。 */
            ESP_LOGW(TAG, "连接失败第 %d 次，重新扫描（不盲目重连）", s_retry);
            set_status(ESL_SCANNING, "重新扫描");
            if (s_scan_tmo != NULL) {
                esp_timer_stop(s_scan_tmo);
                esp_timer_start_once(s_scan_tmo, 100LL * 1000);
            }
            start_scan();
        } else {
            s_retry = 0;
            if (s_tmo) {
                esp_timer_stop(s_tmo);
            }
            set_status(ESL_ERROR, "连接失败（已重试 5 次）");
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "断开 %d", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_scanning = false;
        if (!s_finishing
            && (s_state == ESL_SCANNING || s_state == ESL_CONNECTING
                || s_state == ESL_PUSHING)) {
            /* 中途掉线（价签关机/走远）：必须落地，否则永远停在扫描/推图中 */
            s_phase = 0;
            s_off = 0;
            if (s_tmo) {
                esp_timer_stop(s_tmo);
            }
            set_status(ESL_ERROR, "连接断开");
        }
        s_finishing = false;
        return 0;
    default:
        return 0;
    }
}

static void on_sync(void)
{
    /* 默认 MTU 23 装不下 128 位 UUID 的服务声明，
       价签的厂商服务会因此发现不到 —— 必须先把 MTU 提上去 */
    ble_att_set_preferred_mtu(247);
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    ESP_LOGI(TAG, "BLE 就绪");
    start_scan();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "BLE reset %d", reason); }

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* 算这次画面和上次推出去的画面差多少，决定用局部刷新还是整屏刷新。
   注意：必须在画面**画完之后**调用，否则算出来的是旧范围，价签会留残影 */
static void compute_partial(void)
{
    s_partial = false;
    s_pmin = 0;
    s_pend = PLANE;
    if (!s_last_img_valid || s_last_img == NULL || s_img == NULL) {
        return;
    }
    if (memcmp(s_img, s_last_img, (size_t) (PLANE * 2)) == 0) {
        return;
    }
    int lo = PLANE, hi = -1;
    for (int i = 0; i < PLANE; i++) {
        if (s_img[i] != s_last_img[i] || s_img[PLANE + i] != s_last_img[PLANE + i]) {
            if (i < lo) lo = i;
            if (i > hi) hi = i;
        }
    }
    if (hi < 0 || lo > hi) {
        return;
    }
    /* 一个字节覆盖 8 列像素：范围对齐到 8，避免边缘残影 */
    lo = (lo / 8) * 8;
    hi = ((hi / 8) + 1) * 8;
    if (hi > PLANE) hi = PLANE;
    /* 不启用局部发送：三色墨水屏要求每次写完整图像，
       只发中间一段会让屏上数据错位，整屏变成噪点（实测过）。
       这里只记录变化范围用于日志，实际仍然整屏发送。 */
    ESP_LOGI(TAG, "画面变化 %d/%d 字节（仍整屏发送，局部会让屏幕变花）",
             hi - lo, PLANE);
}

static esp_err_t start(bool info)
{
    /* 单飞：已经在扫描/连接/推图就拒绝，避免两个推送互相踩 */
    esl_state_t cur = s_state;
    if (cur == ESL_SCANNING || cur == ESL_CONNECTING || cur == ESL_PUSHING) {
        ESP_LOGW(TAG, "上一次推送还没结束（%s），拒绝重复进入", s_status);
        return ESP_ERR_INVALID_STATE;
    }

    s_want_info = info;
    /* 每次开始先把上一次的残局清干净，旧句柄绝不能带进来 */
    s_phase = 0;
    s_off = 0;
    s_retry = 0;
    s_finishing = false;
    s_img_chr = s_ctl_chr = s_bat_chr = 0;

    if (s_img == NULL) {
        s_img = heap_caps_malloc(PLANE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_img == NULL) {
            s_img = heap_caps_malloc(PLANE * 2, MALLOC_CAP_8BIT);   /* PSRAM 不行就退回内部 */
        }
        if (s_img == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* 整次推送的总超时（40 秒）：回调丢了也不会永远停在"扫描中/推图中" */
    if (s_tmo == NULL) {
        const esp_timer_create_args_t targs = {
            .callback = tmo_cb,
            .name = "esl_tmo",
        };
        if (esp_timer_create(&targs, &s_tmo) != ESP_OK) {
            s_tmo = NULL;
        }
    }
    if (s_retry_tmo == NULL) {
        const esp_timer_create_args_t rargs = {
            .callback = retry_cb,
            .name = "esl_retry",
        };
        if (esp_timer_create(&rargs, &s_retry_tmo) != ESP_OK) {
            s_retry_tmo = NULL;
        }
    }
    if (s_retry_tmo) {
        esp_timer_stop(s_retry_tmo);
    }
    s_chunk = CHUNK;
    s_retry_left = 2;          /* 一次推送最多整轮重试 2 次（共 3 次机会） */
    if (s_tmo) {
        esp_timer_stop(s_tmo);
        esp_timer_start_once(s_tmo, 40LL * 1000 * 1000);
    }

    /* 关键：在启动蓝牙之前把画面画好。
       这时候没开 BLE，内存充足、也不占蓝牙带宽；推送时只发数据 */
    {
        int64_t tnow = esp_timer_get_time() / 1000;
        bool need = (s_img_built_ms == 0) || (s_img_built_info != info) ||
                    (tnow - s_img_built_ms > IMG_FRESH_MS);
        if (need) {
            if (info) {
                draw_info();
            } else {
                draw_test_card();
            }
            s_img_built_ms = tnow;
            s_img_built_info = info;
            s_img_bat = s_epd_bat;
            ESP_LOGI(TAG, "推送前已画好画面（%s）", info ? "信息页" : "测试卡");
        } else {
            ESP_LOGI(TAG, "复用 %.0f 秒前的画面", (tnow - s_img_built_ms) / 1000.0);
        }
    }

    /* 画面已经确定了，现在才能算局部刷新的范围 */
    compute_partial();

    /* 先把状态立起来再初始化：调用方看到的就是"在跑"，不会误判已结束 */
    set_status(ESL_SCANNING, "准备蓝牙");

    esp_err_t err = nimble_port_init();
    if (err == ESP_OK) {
        /* 协议栈是自己起来的：注册回调并启动 host 任务 */
        s_stack_owned = true;
        ble_hs_cfg.sync_cb = on_sync;
        ble_hs_cfg.reset_cb = on_reset;
        nimble_port_freertos_init(host_task);
        return ESP_OK;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        /* 已经被别的组件（风扇）初始化过：绝不能再启一个 host 任务，
           否则两个任务抢同一个协议栈会崩。直接复用现有栈扫描。 */
        if (!ble_hs_synced()) {
            ESP_LOGW(TAG, "蓝牙协议栈还没就绪，稍后再试");
            fail_cleanup("蓝牙协议栈未就绪");
            return ESP_ERR_INVALID_STATE;
        }
        set_status(ESL_SCANNING, "扫描价签中");
        start_scan();
        return ESP_OK;
    }

    fail_cleanup("蓝牙初始化失败");
    return err;
}

/* 主循环发现"已认证"时调用，记下最近一次认证时刻
   （墨水屏取最近 10 分钟内的记录，不用实时值） */
void esl_ble_note_authed(void)
{
    s_last_authed_ms = esp_timer_get_time() / 1000;
}

/* 推送前预渲染画面：info=信息页，false=测试卡 */
esp_err_t esl_ble_prepare(bool info)
{
    /* 推送过程中正在从这里读数据发出去，绝不能同时改缓冲区 */
    esl_state_t st = s_state;
    if (st == ESL_SCANNING || st == ESL_CONNECTING || st == ESL_PUSHING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_img == NULL) {
        s_img = heap_caps_malloc(PLANE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_img == NULL) {
            s_img = heap_caps_malloc(PLANE * 2, MALLOC_CAP_8BIT);   /* PSRAM 不行就退回内部 */
        }
        if (s_img == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (info) {
        draw_info();
    } else {
        draw_test_card();
    }
    s_img_built_ms = esp_timer_get_time() / 1000;
    s_img_built_info = info;
    s_img_bat = s_epd_bat;
    return ESP_OK;
}

/* 推价签前先确认网络在线：BLE 会抢 2.4G 射频，
   认证/重连的关键时刻推图很容易把 WiFi 顶掉 */
/* 调试用：把当前墨水屏画面缓冲交出去（PLANE*2 字节，黑白两个平面） */
const uint8_t *esl_ble_image(size_t *len)
{
    if (len) {
        *len = (s_img != NULL) ? (size_t) (PLANE * 2) : 0;
    }
    return s_img;
}

/* manual=true：用户按了键，绕过"网络未就绪"检查
   （推价签走蓝牙，本来就不需要联网） */
static esp_err_t push_common(bool info, bool manual)
{
    /* 内容没变就不推：整屏刷新要 40 秒，白刷一次既费电又占射频。
       但手动推送（按键 BOOT / 串口 ESL）例外 —— 用户就是想强制刷一次，
       跳过的话屏上毫无变化，看起来像按键失灵。 */
    if (!manual && s_last_img_valid && s_last_img != NULL && s_img != NULL &&
        memcmp(s_img, s_last_img, (size_t) (PLANE * 2)) == 0) {
        set_status(ESL_DONE, "无变化");
        ESP_LOGI(TAG, "画面和上次一样，跳过推送");
        return ESP_OK;
    }
    if (!esl_net_ready) {
        if (!manual) {
            set_status(ESL_ERROR, "网络未就绪");
            ESP_LOGW(TAG, "网络未就绪，跳过自动推送");
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGW(TAG, "网络未就绪，但这是按键手动推，照推");
    }
    return start(info);
}

esp_err_t esl_ble_push_info(void) { return push_common(true, false); }
esp_err_t esl_ble_push_test(void) { return push_common(false, false); }

/* 按键手动推送：不受网络状态限制 */
esp_err_t esl_ble_push_manual(bool info) { return push_common(info, true); }

/* 推送进度 0~100（给界面显示用） */
int esl_ble_progress(void)
{
    switch (s_state) {
    case ESL_PUSHING:
        if (s_phase >= 7) {
            return 99;                      /* 数据发完，正在刷新 */
        }
        {
            int done = s_off + ((s_phase >= 6) ? PLANE : 0);
            int total = PLANE * 2;
            int p = done * 100 / (total ? total : 1);
            return p < 1 ? 1 : (p > 98 ? 98 : p);
        }
    case ESL_DONE:
        return 100;
    default:
        return 0;
    }
}

bool esl_ble_stack_up(void) { return s_stack_owned; }

/* 价签结束、空闲 15 秒后由 main 调用，把 ~60KB 内部 RAM 还给系统 */
void esl_ble_release(void)
{
    if (!s_stack_owned) {
        return;
    }
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    nimble_port_stop();
    nimble_port_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    s_stack_owned = false;
    s_state = ESL_IDLE;
    ESP_LOGW(TAG, "释放蓝牙栈：内部空闲 %uK → %uK",
             (unsigned) (before / 1024),
             (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}
