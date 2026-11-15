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