/*
 * ESP32-S3 当 BLE 主设备，驱动盒马墨水价签（HINK-E029A14-A1, 296x128, 三色）
 */
#ifndef ESL_BLE_H
#define ESL_BLE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESL_IDLE = 0,
    ESL_SCANNING,
    ESL_CONNECTING,
    ESL_PUSHING,
    ESL_DONE,
    ESL_ERROR,
} esl_state_t;

/** 启动一次"扫描 → 连接 → 推信息页 → 断开" */
esp_err_t esl_ble_push_info(void);

/** 调试：当前墨水屏画面缓冲（PLANE*2 字节，黑/红两个平面） */
const uint8_t *esl_ble_image(size_t *len);

/** main 更新：网络是否就绪（已认证才允许推价签） */
void esl_ble_set_net_ready(bool ready);

/** 预渲染画面（推送前调用，避免在蓝牙会话里实时计算） */
esp_err_t esl_ble_prepare(bool info);

/** 记录一次"认证成功"：墨水屏状态取最近 10 分钟内的记录 */
void esl_ble_note_authed(void);

/** 按键手动推送：不受网络未就绪限制 */
esp_err_t esl_ble_push_manual(bool info);

/** 推几何测试卡（排查方向/颜色用） */
esp_err_t esl_ble_push_test(void);

esl_state_t esl_ble_state(void);

/** 推送进度 0~100 */
int esl_ble_progress(void);

/** 状态中文描述 */
const char *esl_ble_status(void);

/** 蓝牙栈是不是价签这边起的（风扇没动过栈时就是 true） */
bool esl_ble_stack_up(void);

/** 价签结束、空闲后释放蓝牙栈，把 ~60KB 内部 RAM 还给系统 */
void esl_ble_release(void);

#ifdef __cplusplus
}
#endif

#endif
/** 墨水屏界面自定义：布局 0 标准 / 1 大字时间；opts 见 ESL_OPT_* */
void esl_ble_set_ui(uint8_t layout, uint8_t opts, const char *text);

/* ---- 给其它组件用：按需初始化 BLE 协议栈 / 查链路是否忙 ---- */
esp_err_t esl_ble_stack_ensure(void);
void esl_ble_abort(void);   /* 中止当前扫描/连接/推图 */
esp_err_t esl_ble_tracker_scan(uint32_t ms);  /* 被动扫追踪标签 */
void esl_ble_stack_hold(bool on);  /* 占用协议栈，禁止被释放 */
bool esl_ble_link_busy(void);



uint8_t esl_ble_ui_layout(void);
uint8_t esl_ble_ui_opts(void);
const char *esl_ble_ui_text(void);