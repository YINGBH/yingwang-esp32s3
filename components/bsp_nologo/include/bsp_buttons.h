#ifndef _BSP_BUTTONS_H_
#define _BSP_BUTTONS_H_

#include <stdbool.h>

typedef enum {
    BSP_BTN_BOOT = 0,
    BSP_BTN_UP,
    BSP_BTN_DOWN,
    BSP_BTN_COUNT,
} bsp_button_t;

typedef void (*bsp_button_cb_t)(bsp_button_t btn, void *user);

/** 初始化三个按键（低电平有效，内部上拉） */
void bsp_buttons_init(void);

/** 启动扫描任务 */
void bsp_buttons_start(void);

/** 注册单击回调（同一个按键只保留最后一个） */
void bsp_buttons_on_click(bsp_button_cb_t cb, void *user);

/** 当前是否按下 */
bool bsp_buttons_pressed(bsp_button_t btn);

#endif  // _BSP_BUTTONS_H_