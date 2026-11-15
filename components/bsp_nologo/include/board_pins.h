/*
 * Nologo 星智 1.54 寸 TFT（Wi-Fi 版）引脚定义
 *
 * 来源：xiaozhi-esp32 v2.4.2
 *   main/boards/nologo/xingzhi-cube-1.54tft-wifi/config.h
 *   main/boards/nologo/xingzhi-cube-1.54tft-wifi/xingzhi-cube-1.54tft-wifi.cc
 *
 * 这块板子是电池供电的，GPIO21 是电源自锁：开机后必须把它拉高，
 * 松手之后才不会掉电。忘记这一步的表现是"按一下就断电"。
 */
#ifndef _BOARD_PINS_H_
#define _BOARD_PINS_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>

/* ---- 屏幕：ST7789 240x240，走 SPI3 ---- */
#define BOARD_LCD_SPI_HOST   SPI3_HOST
#define BOARD_LCD_MOSI       GPIO_NUM_10
#define BOARD_LCD_SCLK       GPIO_NUM_9
#define BOARD_LCD_DC         GPIO_NUM_8
#define BOARD_LCD_CS         GPIO_NUM_14
#define BOARD_LCD_RST        GPIO_NUM_18
#define BOARD_LCD_BL         GPIO_NUM_13
#define BOARD_LCD_WIDTH      240
#define BOARD_LCD_HEIGHT     240
#define BOARD_LCD_BL_INVERT  false

/* ---- 按键：低电平按下 ---- */
#define BOARD_BTN_BOOT       GPIO_NUM_0
#define BOARD_BTN_UP         GPIO_NUM_40
#define BOARD_BTN_DOWN       GPIO_NUM_39

/* ---- 电源 ---- */
/* 电源自锁：拉高维持供电，拉低即断电（可用于软关机） */
#define BOARD_POWER_HOLD     GPIO_NUM_21
/* 充电检测：高电平 = 正在充电 */
#define BOARD_CHARGE_DETECT  GPIO_NUM_38

/* ---- 音频：纯 I2S，没有 codec 芯片 ---- */
#define BOARD_MIC_WS         GPIO_NUM_4
#define BOARD_MIC_SCK        GPIO_NUM_5
#define BOARD_MIC_DIN        GPIO_NUM_6
#define BOARD_SPK_DOUT       GPIO_NUM_7
#define BOARD_SPK_BCLK       GPIO_NUM_15
#define BOARD_SPK_LRCK       GPIO_NUM_16

#endif  // _BOARD_PINS_H_