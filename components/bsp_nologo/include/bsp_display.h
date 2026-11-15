#ifndef _BSP_DISPLAY_H_
#define _BSP_DISPLAY_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* RGB565：高字节先传，和 ST7789 的默认一致 */
#define RGB565(r, g, b) ((uint16_t) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* 常用颜色 */
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_ORANGE  0xFD20
#define COLOR_CYAN    0x07FF
#define COLOR_GRAY    0x8410
#define COLOR_DARK    0x2104
#define COLOR_BG      0x1082

/* ---------------- 主题 ----------------
   参考 ESPHome/LVGL 的做法：界面只引用语义角色，不写死色值，
   切换主题时整套替换（bg/fg/dim/accent/ok/warn/info/panel 共 8 个）。*/
typedef struct {
    uint16_t bg;        /* 背景 */
    uint16_t fg;        /* 主文字 */
    uint16_t dim;       /* 次要文字 */
    uint16_t accent;    /* 标题 / 高亮 */
    uint16_t ok;        /* 正常 / 已认证 */
    uint16_t warn;      /* 警告 / 错误 */
    uint16_t info;      /* 数值 / 信息 */
    uint16_t panel;     /* 面板、分隔线、色块 */
} bsp_theme_t;

extern bsp_theme_t g_theme;

#define TH_BG     (g_theme.bg)
#define TH_FG     (g_theme.fg)
#define TH_DIM    (g_theme.dim)
#define TH_ACCENT (g_theme.accent)
#define TH_OK     (g_theme.ok)
#define TH_WARN   (g_theme.warn)
#define TH_INFO   (g_theme.info)
#define TH_PANEL  (g_theme.panel)

#define BSP_THEME_COUNT 11   /* 5 基础 + 6 二次元 */

/** 主题名（idx 越界返回 NULL） */
const char *bsp_theme_name(int idx);

/** 载入内置主题（idx 越界不做事） */
void bsp_theme_load_preset(int idx);

/** 自定义：只给背景色和强调色，其余按背景明暗自动推导（浅底配深字） */
void bsp_theme_custom(uint16_t bg, uint16_t accent);

/** 当前主题序号（自定义返回 -1） */
int bsp_theme_current(void);

/**
 * 有背景图时保证文字可读。
 *
 * 浅色主题（暖白）是"深字浅底"，但如果用户又传了一张深色背景图，
 * 字就糊在图上。这个函数在有背景图时把主文字/次要文字强制成亮色，
 * 强调色和状态色保持不变 —— 主题的"性格"还在，字也不会看不见。
 */
void bsp_theme_ensure_readable(bool has_background);

/* ---------------- 装饰层（二次元定制） ----------------
   边框 / 角落小图标 / 角标文字，都跟随主题 accent 色。
   内容画完之后调用，盖在整页上面。*/
#define BSP_DECO_BORDER_NONE  0
#define BSP_DECO_BORDER_THIN  1   /* 1px 细线 */
#define BSP_DECO_BORDER_ROUND 2   /* 圆角框 */
#define BSP_DECO_BORDER_DOUBLE 3  /* 双线 */

#define BSP_DECO_ICON_NONE   0
#define BSP_DECO_ICON_STAR   1
#define BSP_DECO_ICON_HEART  2
#define BSP_DECO_ICON_NOTE   3
#define BSP_DECO_ICON_SAKURA 4
#define BSP_DECO_ICON_MOON   5

/** 设置装饰（border/icon 越界自动夹到合法范围；tag 可为空字符串） */
void bsp_deco_set(uint8_t border, uint8_t icon, const char *tag);

/** 按当前设置画装饰层 */
void bsp_deco_draw(void);

/** 初始化 SPI + ST7789 + 背光 PWM。失败会打日志并返回错误码 */
esp_err_t bsp_display_init(void);

/** 设置背光亮度 0-100 */
void bsp_display_backlight(int percent);

/**
 * 设置屏幕方向。
 * @param deg 0 / 90 / 180 / 270（其他值按 0 处理）
 * 屏幕是 240x240 正方形，旋转不改变分辨率，UI 坐标不用动。
 */
void bsp_display_set_rotation(int deg);

/** 读取当前方向 */
int bsp_display_get_rotation(void);

/** 微调显存偏移（旋转后画面偏了用它修） */
void bsp_display_set_gap_adj(int x, int y);

uint16_t bsp_display_width(void);
uint16_t bsp_display_height(void);

/** 全屏填充 */
void bsp_display_clear(uint16_t color);
/** 把帧缓冲刷到屏幕（所有绘制完成后调用一次） */
void bsp_display_flush(void);

/** 填充矩形（自动裁到屏幕范围内） */
void bsp_display_fill_rect(int x, int y, int w, int h, uint16_t color);

/** 画边框 */
void bsp_display_rect(int x, int y, int w, int h, uint16_t color);

/**
 * 显示一行 UTF-8 文本（支持 ASCII 和字库里收录的汉字）。
 *
 * @param x, y    左上角坐标
 * @param scale   放大倍数，1 = 8x16 / 16x16 原始大小
 * @param bg      背景色；传 -1 表示只画字不改背景
 * @return        画完后的 x 坐标，方便接着画
 */
int bsp_display_text(int x, int y, const char *utf8, uint16_t fg,
                     int bg, uint8_t scale);

/** 文本宽度（像素），用于居中 */
int bsp_display_text_width(const char *utf8, uint8_t scale);

/** 直接送一块 RGB565 数据到屏幕（背景图用） */
void bsp_display_blit(int x, int y, int w, int h, const uint16_t *pixels);

/** 画一张 RGB888 图片（可选放大和反色），用于显示验证码 */
void bsp_display_image(int x, int y, int w, int h, const uint8_t *rgb888,
                       uint8_t scale, bool invert);

/** 取帧缓冲（240x240 RGB565，小端），用于调试截图 */
const uint16_t *bsp_display_framebuffer(void);

/** 水平居中显示 */
void bsp_display_text_center(int y, const char *utf8, uint16_t fg,
                             int bg, uint8_t scale);

#endif  // _BSP_DISPLAY_H_