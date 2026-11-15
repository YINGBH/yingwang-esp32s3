#ifndef _BSP_POWER_H_
#define _BSP_POWER_H_

#include <stdbool.h>

/**
 * 上电自锁。必须尽早调用（display 之前），
 * 否则电池供电时松手就断电。
 */
void bsp_power_hold(void);

/** 释放自锁 = 软关机，调用后几十毫秒内断电 */
void bsp_power_off(void);

/** 是否正在充电 */
bool bsp_power_is_charging(void);

#endif  // _BSP_POWER_H_