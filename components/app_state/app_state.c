/*
 * 全局运行状态
 *
 * 只放两样东西：
 *   - 当前状态（已认证/登录中/失败…）和一行详情，屏幕和串口共享
 *   - 动作请求（重试登录、重连 WiFi、重启、重画屏幕）
 *
 * 所有读写都过一把互斥锁：主循环在写、串口任务在读，detail 是 48 字节的
 * 字符串，不加锁可能读到半截中文。
 */
#include "app_state.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock = NULL;
static app_state_t s_state = APP_ST_BOOT;
static char s_detail[48] = "";
static uint32_t s_actions = 0;
static int64_t s_boot_us = 0;

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

void app_state_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    s_boot_us = esp_timer_get_time();
    s_state = APP_ST_BOOT;
    s_detail[0] = '\0';
    s_actions = 0;
}

void app_state_set(app_state_t st, const char *detail)
{
    lock();
    s_state = st;
    if (detail != NULL) {
        snprintf(s_detail, sizeof(s_detail), "%s", detail);
    }
    unlock();
}

app_state_t app_state_get(char *detail, size_t cap)
{
    app_state_t st;
    lock();
    st = s_state;
    if (detail && cap > 0) {
        snprintf(detail, cap, "%s", s_detail);
    }
    unlock();
    return st;
}

const char *app_state_name(app_state_t st)
{
    switch (st) {
    case APP_ST_BOOT:   return "启动";
    case APP_ST_WIFI:   return "连接中";
    case APP_ST_PROBE:  return "检测中";
    case APP_ST_LOGIN:  return "登录中";
    case APP_ST_ONLINE: return "已认证";
    case APP_ST_NOWIFI: return "未连接";
    case APP_ST_ERROR:  return "失败";
    default:            return "就绪";
    }
}

const char *app_state_color(app_state_t st)
{
    switch (st) {
    case APP_ST_ONLINE: return "green";
    case APP_ST_WIFI:
    case APP_ST_PROBE:  return "yellow";
    case APP_ST_LOGIN:  return "orange";
    case APP_ST_NOWIFI:
    case APP_ST_ERROR:  return "red";
    default:            return "gray";
    }
}

uint32_t app_state_uptime_s(void)
{
    if (s_boot_us == 0) {
        return 0;
    }
    return (uint32_t) ((esp_timer_get_time() - s_boot_us) / 1000000);
}

void app_state_request(uint32_t actions)
{
    lock();
    s_actions |= actions;
    unlock();
}

uint32_t app_state_take_actions(void)
{
    uint32_t a;
    lock();
    a = s_actions;
    s_actions = 0;
    unlock();
    return a;
}
