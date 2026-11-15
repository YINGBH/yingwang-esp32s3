/*
 * WiFi STA 连接管理：连上后自动重连，掉线不会卡死
 */
#ifndef WIFI_STA_H
#define WIFI_STA_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化并开始连接，非阻塞。 */
esp_err_t wifi_sta_start(const char *ssid, const char *password);
void wifi_sta_set_static_ip(const char *ip);
esp_err_t wifi_sta_set_mac(const char *macstr);

/** 管理帧是否锁 1Mbps（抗干扰）；默认 true */
void wifi_sta_set_rate_1m(bool on);

/** 强制断开再连（手动重新认证） */
void wifi_sta_force_reconnect(void);
/** 硬重启：disconnect → stop → start → connect */
void wifi_sta_hard_restart(void);
/** 夜间休眠：true=打开 WiFi，false=关掉 */
void wifi_sta_power(bool on);

/** 打印当前 DNS 服务器（诊断） */
void wifi_sta_dump_dns(void);

/** 重新应用 DNS 服务器 */
void wifi_sta_reapply_dns(void);
/** 换下一个候选 DNS（路由器→阿里→腾讯→114→百度） */
void wifi_sta_dns_rotate(void);
/** 当前正在用的 DNS 名称 */
const char *wifi_sta_dns_name(void);
/** 直接指定候选序号（0=路由器） */
void wifi_sta_dns_set(int idx);

/** 是否已拿到 IP。 */
/**
 * 换 WiFi 配置后重连（网页后台改密码时用）。
 * 不会重新初始化 WiFi 驱动，可以反复调用。
 */
esp_err_t wifi_sta_reload(const char *ssid, const char *password);

bool wifi_sta_connected(void);

/** 信号强度，未连接返回 0。 */
int8_t wifi_sta_rssi(void);

/** 已连接时返回当前 SSID，否则返回空串。 */
const char *wifi_sta_ssid(void);

/** 已连接时返回本机 IP 字符串，否则返回空串。 */
const char *wifi_sta_ip(void);

/** 目标热点在不在附近（dBm，不在返回 -127） */
int8_t wifi_sta_target_rssi(const char *ssid);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_STA_H */