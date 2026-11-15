/*
 * USB 串口命令接口（给网页版上位机用）
 *
 * 走 USB-Serial/JTAG 的控制台串口，一行一条命令，应答行以 '@' 开头，
 * 网页那边只认 @ 开头的行，其它行（I/W/E 日志）直接忽略。
 *
 *   PING                  -> @{"ok":true}
 *   GET                   -> @{"ok":true,"state":...,"cfg":{...}}
 *   SET key=value         -> @{"ok":true}
 *       key: ssid pass user pwd city lat lon brightness screen_off
 *   RECONNECT             -> 重连 WiFi
 *   REBOOT                -> 重启
 */
#ifndef _USB_CONSOLE_H_
#define _USB_CONSOLE_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void usb_console_init(void);

/** 由 main 实现：切到第 idx 个界面（调试/产线截图用） */
void ui_goto(int idx);

/** 由 main 实现：暂停/恢复界面刷新（截图用） */
void ui_freeze(bool on);

/** 由 main 实现：模拟按一次键（0=BOOT 1=UP 2=DOWN） */
void ui_press(int which);

/** 切换计时页模式：0=倒计时 1=秒表 */
void ui_timer_mode(int sw);

/** 测试用：触发一次环境很吵提醒 */
void ui_noise_test(void);

/** 调试：画出开机 LOGO（on=false 恢复界面） */
void ui_show_logo(bool on);

#ifdef __cplusplus
}
#endif

#endif  // _USB_CONSOLE_H_
