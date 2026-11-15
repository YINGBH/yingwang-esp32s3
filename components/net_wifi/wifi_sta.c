#include "wifi_sta.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/ip_addr.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi";
static void apply_dns_only(void);   /* 前置声明 */
static uint8_t s_mac[6];
static bool s_rate_1m = true;     /* 管理帧锁 1Mbps */
static bool s_mac_set = false;

#define BIT_CONNECTED  BIT0
#define BIT_FAIL       BIT1

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static char s_static_ip[32];                 /* 非空则用静态 IP，不走 DHCP */
void apply_static_ip__(void);               /* 前置声明 */
static int s_retry;
static int s_fail_streak;                  /* 连续失败次数（只用于日志） */
static volatile bool s_connecting;
static esp_timer_handle_t s_recon_tmo;     /* 退避重连定时器 */
static char s_ssid[33];
static volatile bool s_connected;
static bool s_wifi_inited = false;


/* 只设置 DNS（不碰 IP，避免再次触发 GOT_IP 事件）。
   顺序：公共 DNS 优先，网关兜底。带重入保护。 */
/* ---------------- DNS 候选与轮换 ----------------
   顺序：路由器（网关）→ 阿里 → 腾讯 → 114 → 百度。
   网关 DNS 失效时自动往下换，不再一根筋重设同一个。 */
static const char *kDnsCand[] = {
    NULL,               /* 0 = 网关，运行时取 */
    "223.5.5.5",        /* 阿里 */
    "119.29.29.29",     /* 腾讯 */
    "114.114.114.114",  /* 114 */
    "180.76.76.76",     /* 百度 */
};
#define DNS_CAND_N ((int) (sizeof(kDnsCand) / sizeof(kDnsCand[0])))
static int s_dns_rot = 0;

const char *wifi_sta_dns_name(void)
{
    if (s_dns_rot == 0) {
        return "路由器";
    }
    return kDnsCand[s_dns_rot];
}

/* 按 s_dns_rot 设置 MAIN / BACKUP / FALLBACK */
static void dns_apply_rot(void)
{
    if (s_netif == NULL) {
        return;
    }
    esp_netif_ip_info_t ipi;
    bool got_gw = (esp_netif_get_ip_info(s_netif, &ipi) == ESP_OK && ipi.gw.addr != 0);

    esp_netif_dns_info_t d = { 0 };
    d.ip.type = ESP_IPADDR_TYPE_V4;

    /* 主 DNS */
    if (s_dns_rot == 0) {
        if (!got_gw) {
            s_dns_rot = 1;                  /* 没网关就直接用阿里 */
        } else {
            d.ip.u_addr.ip4.addr = ipi.gw.addr;
        }
    }
    if (s_dns_rot != 0) {
        d.ip.u_addr.ip4.addr = esp_ip4addr_aton(kDnsCand[s_dns_rot]);
    }
    esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &d);

    /* 备用 1、备用 2：主 DNS 之后的两个候选 */
    for (int k = 1; k <= 2; k++) {
        int idx = (s_dns_rot + k) % DNS_CAND_N;
        if (idx == 0) {
            if (!got_gw) {
                continue;
            }
            d.ip.u_addr.ip4.addr = ipi.gw.addr;
        } else {
            d.ip.u_addr.ip4.addr = esp_ip4addr_aton(kDnsCand[idx]);
        }
        esp_netif_set_dns_info(s_netif,
                               (k == 1) ? ESP_NETIF_DNS_BACKUP : ESP_NETIF_DNS_FALLBACK, &d);
    }
}

/* 换下一个候选 DNS（主循环在解析失败时调用） */
void wifi_sta_dns_rotate(void)
{
    s_dns_rot = (s_dns_rot + 1) % DNS_CAND_N;
    dns_apply_rot();
    ESP_LOGW(TAG, "DNS 换到：%s", wifi_sta_dns_name());
}

void wifi_sta_dns_set(int idx)
{
    if (idx < 0 || idx >= DNS_CAND_N) {
        return;
    }
    s_dns_rot = idx;
    dns_apply_rot();
}

static void apply_dns_only(void)
{
    static volatile bool busy = false;
    if (busy || s_netif == NULL) {
        return;
    }
    busy = true;
    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    /* 网关优先：同一网络下手机用路由器 DNS 解析正常，
       板子用公共 DNS 却反复 getaddrinfo 202。公共 DNS 只做兜底。 */
    esp_netif_ip_info_t ipi;
    bool got_gw = (esp_netif_get_ip_info(s_netif, &ipi) == ESP_OK && ipi.gw.addr != 0);
    if (got_gw) {
        dns.ip.u_addr.ip4.addr = ipi.gw.addr;
        esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("223.5.5.5");
    busy = false;

    /* 统一交给轮换逻辑设置（会处理"没有网关"的情况） */
    dns_apply_rot();
    ESP_LOGI(TAG, "DNS = %s", wifi_sta_dns_name());
}

static void recon_cb(void *arg)
{
    (void) arg;
    if (!s_connected) {
        s_connecting = true;
        esp_err_t e = esp_wifi_connect();
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "重连失败: %s", esp_err_to_name(e));
        }
    }
}

static void ensure_recon_timer(void)
{
    if (s_recon_tmo != NULL) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = recon_cb,
        .name = "wifi_recon",
    };
    if (esp_timer_create(&args, &s_recon_tmo) == ESP_OK) {
        ESP_LOGI(TAG, "退避重连定时器已创建（5/10/20/30/60 秒）");
    } else {
        ESP_LOGE(TAG, "退避定时器创建失败，将用固定间隔重连");
    }
}

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* 一律走退避定时器，避免和正在进行的连接撞车 */
        if (s_recon_tmo) {
            esp_timer_stop(s_recon_tmo);
            esp_timer_start_once(s_recon_tmo, 1000);
        } else {
            /* 管理帧先用 1Mbps：拥挤信道上认证/关联更容易成功 */
            if (s_rate_1m) {
                esp_err_t re = esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_1M_L);
                ESP_LOGW(TAG, "锁定 1Mbps 发送: %s", esp_err_to_name(re));
            }
            /* 必须在 connect 之前改 MAC，否则会返回 ESP_ERR_WIFI_STATE */
            if (s_mac_set) {
                esp_err_t me = esp_wifi_set_mac(WIFI_IF_STA, s_mac);
                ESP_LOGW(TAG, "应用伪装 MAC: %s", esp_err_to_name(me));
            }
            /* 让 WiFi 尽量保持连接的三件套 */
            esp_wifi_set_ps(WIFI_PS_NONE);                 /* 关省电：不错过 AP 的包 */
            esp_wifi_set_protocol(WIFI_IF_STA,
                                  WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
            esp_wifi_set_inactive_time(WIFI_IF_STA, 60);   /* 不活跃 60 秒才断（默认 10） */
            ESP_LOGI(TAG, "已关闭省电、只启用 11bgn、不活跃超时 60 秒");

            s_connecting = true;
            esp_wifi_connect();
        }

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *) data;
        s_connected = false;
        s_connecting = false;
        s_retry++;
        s_fail_streak++;
        int reason = e ? e->reason : -1;
        ESP_LOGW(TAG, "断开（reason=%d），第 %d 次重连", reason, s_retry);

        /* 按用户要求：**不再重启 WiFi 驱动**。
           原来连续 40 次失败会 esp_wifi_stop/start，但重启会重新走关联，
           反而可能再次撞上 AP 的限流；退避（5→60 秒）已经足够让 AP 恢复。
           这里只打一条日志，然后照常退避重连。 */
        if (s_fail_streak > 0 && (s_fail_streak % 20) == 0) {
            ESP_LOGW(TAG, "已连续失败 %d 次，继续按退避重连（不重启驱动）", s_fail_streak);
        }

        /* 退避：5s → 10s → 20s → 30s → 60s 封顶。
           原来 3.4 秒一次太密：华为路由器对反复关联失败的设备会临时限流，
           越敲越被拒。放慢节奏让 AP 把坏状态过期掉。 */
        static const uint32_t steps[] = { 5000, 10000, 20000, 30000, 60000 };
        int idx = s_retry <= 1 ? 0 : (s_retry >= 5 ? 4 : s_retry - 1);
        uint32_t delay_ms = steps[idx];
        if (s_recon_tmo != NULL) {
            esp_timer_stop(s_recon_tmo);
            esp_timer_start_once(s_recon_tmo, (uint64_t) delay_ms * 1000);
        } else {
            esp_wifi_connect();
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *) data;
        ESP_LOGI(TAG, "拿到 IP: " IPSTR, IP2STR(&e->ip_info.ip));
        /* 拿到 IP 后再确认一次关闭省电：开机那次设置常被驱动忽略 */
        esp_wifi_set_ps(WIFI_PS_NONE);
        /* 重连后把 DNS 再设一遍（只设 DNS，不动 IP：
           动 IP 会再次触发 GOT_IP，形成无限递归）。 */
        apply_dns_only();
        s_retry = 0;
        s_fail_streak = 0;
        s_connecting = false;
        if (s_recon_tmo) {
            esp_timer_stop(s_recon_tmo);
        }
        s_connected = true;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

esp_err_t wifi_sta_start(const char *ssid, const char *password)
{

    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_ssid, ssid, sizeof(s_ssid));

    s_events = xEventGroupCreate();
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
 esp_netif_set_hostname(s_netif, "esp32-portal");
    apply_static_ip__();   /* 有静态 IP 就先配上，DHCP 不再参与 */ /* 路由器设备列表里显示为 YING */

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    s_wifi_inited = true;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL, NULL));

    wifi_config_t cfg = { 0 };
    strlcpy((char *) cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (password) {
        strlcpy((char *) cfg.sta.password, password, sizeof(cfg.sta.password));
    }
    cfg.sta.threshold.authmode = password && password[0]
                                     ? WIFI_AUTH_WPA2_PSK
                                     : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ensure_recon_timer();      /* 必须先建好，否则断开时没有退避 */
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "开始连接 %s", ssid);
    return ESP_OK;
}

bool wifi_sta_connected(void)
{
    return s_connected;
}

int8_t wifi_sta_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

const char *wifi_sta_ssid(void)
{
    return s_ssid;
}

const char *wifi_sta_ip(void)
{
    static char s_ip[16] = {0};
    s_ip[0] = 0;

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        return s_ip;
    }

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        return s_ip;
    }
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&info.ip));
    return s_ip;
}

/**
 * 用新的 SSID/密码重连。
 *
 * wifi_sta_start() 里做了 esp_wifi_init，只能调用一次（第二次会断言失败）。
 * 网页后台改了 WiFi 密码后走这个函数：只改配置、断开重连，不动驱动。
 */
esp_err_t wifi_sta_reload(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_wifi_inited) {
        return wifi_sta_start(ssid, password);
    }

    strlcpy(s_ssid, ssid, sizeof(s_ssid));

    wifi_config_t cfg = { 0 };
    strlcpy((char *) cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (password) {
        strlcpy((char *) cfg.sta.password, password, sizeof(cfg.sta.password));
    }
    cfg.sta.threshold.authmode = (password && password[0])
                                     ? WIFI_AUTH_WPA2_PSK
                                     : WIFI_AUTH_OPEN;

    s_connected = false;

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "断开失败: %s", esp_err_to_name(err));
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置新配置失败: %s", esp_err_to_name(err));
        return err;
    }

    ensure_recon_timer();
    s_connecting = true;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "重连失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "配置已更新，正在连接 %s", ssid);
    return ESP_OK;
}

/**
 * 扫一下目标 SSID 在不在附近，返回信号强度（dBm）；不在返回 -127。
 * 用来避免"目标热点根本没信号还一直重连"的空转。
 */
int8_t wifi_sta_target_rssi(const char *ssid)
{
    if (ssid == NULL || ssid[0] == 0 || !s_wifi_inited) {
        return -127;
    }
    wifi_scan_config_t cfg = { 0 };
    cfg.show_hidden = true;
    esp_err_t err = esp_wifi_scan_start(&cfg, true);      /* true = 阻塞扫描 */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "扫描失败 %s", esp_err_to_name(err));
        return -127;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        return -127;
    }
    if (n > 32) {
        n = 32;
    }
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        return -127;
    }
    int8_t best = -127;
    uint16_t got = n;
    if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
        for (uint16_t i = 0; i < got; i++) {
            if (strcmp((const char *) recs[i].ssid, ssid) == 0
                && recs[i].rssi > best) {
                best = recs[i].rssi;
            }
        }
    }
    free(recs);
    return best;
}

void apply_static_ip__(void)
{
    if (s_static_ip[0] == '\0' || s_netif == NULL) {
        return;
    }
    /* 支持 "192.168.1.240" 或 "192.168.1.240/24"（默认 /24） */
    char iponly[40];
    int prefix = 24;
    const char *slash = strchr(s_static_ip, '/');
    if (slash != NULL) {
        size_t n = (size_t) (slash - s_static_ip);
        if (n >= sizeof(iponly)) {
            n = sizeof(iponly) - 1;
        }
        memcpy(iponly, s_static_ip, n);
        iponly[n] = '\0';
        int p = atoi(slash + 1);
        if (p >= 8 && p <= 30) {
            prefix = p;
        } else {
            ESP_LOGW(TAG, "前缀 /%d 不合理，按 /24 处理", p);
        }
    } else {
        snprintf(iponly, sizeof(iponly), "%s", s_static_ip);
    }

    esp_netif_ip_info_t info = { 0 };
    if (esp_netif_str_to_ip4(iponly, &info.ip) != ESP_OK) {
        ESP_LOGW(TAG, "静态 IP 格式不对: %s", s_static_ip);
        return;
    }
    uint32_t mask = (prefix >= 32) ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32 - prefix));
    uint32_t ip = info.ip.addr;
    info.netmask.addr = htonl(mask);
    info.gw.addr = (ip & htonl(mask)) | htonl(1U);

    esp_netif_dhcpc_stop(s_netif);
    if (esp_netif_set_ip_info(s_netif, &info) != ESP_OK) {
        ESP_LOGW(TAG, "设置静态 IP 失败，回退 DHCP");
        esp_netif_dhcpc_start(s_netif);
        return;
    }
    /* DNS：网关优先（内网域名要靠它），再挂两个公共 DNS 兜底。
       之前只设了网关，路由器 DNS 一卡就所有域名解析失败
       （日志里 getaddrinfo() returns 202）。 */
    apply_dns_only();
    ESP_LOGI(TAG, "DNS: 网关 + 223.5.5.5 + 119.29.29.29");
    char ipbuf[16], gwbuf[16], maskbuf[16];
    esp_ip4addr_ntoa(&info.ip, ipbuf, sizeof(ipbuf));
    esp_ip4addr_ntoa(&info.gw, gwbuf, sizeof(gwbuf));
    esp_ip4addr_ntoa(&info.netmask, maskbuf, sizeof(maskbuf));
    ESP_LOGI(TAG, "使用静态 IP %s 掩码 %s 网关 %s", ipbuf, maskbuf, gwbuf);
}

void wifi_sta_set_static_ip(const char *ip)
{
    snprintf(s_static_ip, sizeof(s_static_ip), "%s", ip ? ip : "");
    ESP_LOGI(TAG, "静态 IP: %s", s_static_ip[0] ? s_static_ip : "(DHCP)");
}

/* 强制断开再连：状态页连按 3 次 BOOT 时用，
   走完整的 认证 → 关联 → DHCP/静态 IP → 门户探测 流程 */
/* 夜间休眠：直接把 WiFi 关掉/打开。
   路由器夜里关机时，板子会一直 reason=201 重连（实测两小时几百次），
   既费电又刷日志，所以干脆关掉射频，早上再开。 */
void wifi_sta_power(bool on)
{
    if (on) {
        ESP_LOGW(TAG, "夜间结束：打开 WiFi");
        esp_err_t e = esp_wifi_start();
        if (e != ESP_OK && e != ESP_ERR_WIFI_INIT_STATE) {
            ESP_LOGW(TAG, "esp_wifi_start: %s", esp_err_to_name(e));
        }
        s_connecting = true;
        s_connected = false;
        esp_wifi_connect();
    } else {
        ESP_LOGW(TAG, "进入夜间：关闭 WiFi");
        s_connected = false;
        s_connecting = false;
        if (s_recon_tmo) {
            esp_timer_stop(s_recon_tmo);
        }
        esp_wifi_stop();
    }
}

/* 硬重启 WiFi：stop → start → connect。
   比 force_reconnect 彻底：会把驱动状态、扫描缓存、BSSID 关联全部清掉。
   早上路由器刚起来、一直对着同一个 AP 重试连不上时用这个。 */
void wifi_sta_hard_restart(void)
{
    ESP_LOGW(TAG, "硬重启 WiFi：disconnect → stop → start → connect");
    if (s_recon_tmo) {
        esp_timer_stop(s_recon_tmo);
    }
    s_connected = false;
    s_connecting = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_wifi_start();

    /* esp_wifi_start() 是异步的：驱动还没把 STA 拉到 START 状态时
       esp_wifi_connect() 会返回 ESP_ERR_WIFI_CONN。这里重试几次等它就绪。 */
    s_retry = 0;
    s_fail_streak = 0;
    s_connecting = true;
    esp_err_t e = ESP_ERR_WIFI_CONN;
    for (int i = 0; i < 10; i++) {
        vTaskDelay(pdMS_TO_TICKS(300));
        e = esp_wifi_connect();
        if (e != ESP_ERR_WIFI_CONN) {
            break;
        }
    }
    ESP_LOGW(TAG, "硬重启完成，重连发起: %s", esp_err_to_name(e));
}

void wifi_sta_force_reconnect(void)
{
    ESP_LOGW(TAG, "强制重连（手动重新认证）");
    s_retry = 0;
    s_fail_streak = 0;
    s_connected = false;
    if (s_recon_tmo) {
        esp_timer_stop(s_recon_tmo);
    }
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    s_connecting = true;
    esp_err_t e = esp_wifi_connect();
    ESP_LOGW(TAG, "强制重连发起: %s", esp_err_to_name(e));
}

void wifi_sta_set_rate_1m(bool on)
{
    s_rate_1m = on;
    ESP_LOGW(TAG, "管理帧速率策略: %s", on ? "锁 1Mbps（抗干扰）" : "自动");
}

esp_err_t wifi_sta_set_mac(const char *macstr)
{
    if (macstr == NULL || macstr[0] == '\0' ||
        strcmp(macstr, "reset") == 0 || strcmp(macstr, "0") == 0) {
        s_mac_set = false;
        memset(s_mac, 0, sizeof(s_mac));
        ESP_LOGI(TAG, "MAC 伪装已关闭，使用芯片真实 MAC");
        return ESP_OK;
    }
    unsigned v[6];
    if (sscanf(macstr, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        ESP_LOGW(TAG, "MAC 格式不对: %s", macstr);
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF) {
            ESP_LOGW(TAG, "MAC 某一段超出 0xFF: %s", macstr);
            return ESP_ERR_INVALID_ARG;
        }
        s_mac[i] = (uint8_t) v[i];
    }
    s_mac_set = true;
    ESP_LOGI(TAG, "MAC 伪装: %02X:%02X:%02X:%02X:%02X:%02X",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    return ESP_OK;
}


/* 打印当前 lwIP 里的 DNS 服务器（诊断用） */
void wifi_sta_dump_dns(void)
{
    if (s_netif == NULL) {
        ESP_LOGW(TAG, "netif 还没创建");
        return;
    }
    static const char *names[] = { "MAIN", "BACKUP", "FALLBACK" };
    for (int i = 0; i < 3; i++) {
        esp_netif_dns_info_t d;
        memset(&d, 0, sizeof(d));
        if (esp_netif_get_dns_info(s_netif, (esp_netif_dns_type_t) i, &d) == ESP_OK) {
            char b[20] = { 0 };
            esp_ip4addr_ntoa(&d.ip.u_addr.ip4, b, sizeof(b));
            ESP_LOGI(TAG, "DNS %-8s = %s", names[i], b);
        }
    }
}


/* 供主循环调用：重新灌一遍 DNS 服务器 */
void wifi_sta_reapply_dns(void)
{
    apply_dns_only();
}
