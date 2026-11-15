/**
 * 联通校园宽带门户客户端实现
 *
 * 关键点（都是 PC 端踩过坑之后总结出来的）：
 *  - 必须先 open_index，否则服务器不知道账号要授权给谁，会回"用户名或密码错误"
 *  - 密码是 RSA-512 PKCS#1 v1.5，再套两层 base64
 *  - 验证码要先取 ID 再取图片，提交时 verifyHidden 带 ID
 */

#include "esp_attr.h"
#include "portal_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pngle.h"
#include "captcha_infer.h"
#include "pwd_enc.h"

static const char *TAG = "portal";

/* 验证码回调：带屏的板子用来看一眼识别得对不对 */
static portal_captcha_cb_t s_captcha_cb = NULL;
EXT_RAM_BSS_ATTR static uint8_t s_last_png[8192];   /* 最近一次验证码 PNG（测试用） */
static size_t s_last_png_len = 0;
static float s_last_conf[CAPTCHA_CHARS];   /* 最近一次识别的置信度 */

/* 提交前要求每一位的置信度都不低于这个值，否则换一张图重取。
   实测（300 张人工标注）：8 个错例的最低位置信度全部 < 0.87，
   正确样本中位数 0.997。门限设 0.90 时保留集准确率 100%，
   代价只是多取约 8% 的图（取图免费，不消耗提交次数）。 */
#define CAPTCHA_CONF_MIN 0.90f
#define CAPTCHA_REROLL_MAX 6
static void *s_captcha_user = NULL;

void portal_set_captcha_callback(portal_captcha_cb_t cb, void *user)
{
    s_captcha_cb = cb;
    s_captcha_user = user;
}

#define PORTAL_BASE      "http://YOUR_PORTAL_IP:8090"
/* 门户登录表单里的固定值（实测配置）：
   AC_IP 取跳转 URL 里的 wlanacip，wlanAcName 留空，ssid 用你连的校园网 SSID */

/* 站点参数运行时从配置读（网页后台 / SET portal_base= 改），
   下面三个宏只是「谁都没设置」时的兜底默认值 */
#define PORTAL_DEF_AC_IP "YOUR_AC_IP"
#define PORTAL_DEF_SSID  "YOUR_CAMPUS_SSID"
#define PORTAL_BASE_MAX  128
#define PORTAL_UA        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/131.0.0.0"
#define HTTP_TIMEOUT_MS  4000   /* 原 12 秒：4 个站点最坏 48 秒，屏幕一直卡"检测中" */

#define BODY_CAP         4096
#define LOCATION_CAP     256
#define COOKIE_CAP       384

/* 认证状态的唯一标尺：能正常打开 cn.bing.com 就算已认证，
   被跳到门户/打不开就说明需要认证 */
#define PROBE_URL   "http://cn.bing.com/"
/* 二次确认：门户经常把 cn.bing.com 放白名单（给系统联网检测用），
   只看 Bing 会误判"已认证"。再探一个不会进白名单的国内站点。 */
#define PROBE_URL2  "http://www.nmc.cn/"
/* 这两个是"必然会被 AC 拦截"的地址：只要没认证，AC 会回 302 跳到门户 */
#define PROBE_URL3  "http://connectivitycheck.platform.hicloud.com/generate_204"
#define PROBE_URL4  "http://www.msftconnecttest.com/connecttest.txt"
/* 多目标探测：校园网经常单独拦某个域名，任意一个通就算在线 */
#define PROBE_URL5  "http://www.baidu.com/"
#define PROBE_URL6  "http://www.qq.com/"

static portal_wlan_t s_wlan;
static char s_my_ip[16];           /* main 联网后告诉我们当前 IP */

/* 运行时站点参数：由 main / 串口 SET 通过 portal_set_site() 写入。
   空串表示这一项用编译期默认值，这样旧配置和没配置过的板子行为不变。 */
static char s_base[PORTAL_BASE_MAX];
static char s_site_ac_ip[48];
static char s_site_ssid[48];

const char *portal_base_url(void)
{
    return (s_base[0] != '\0') ? s_base : PORTAL_BASE;
}

static const char *site_ac_ip(void)
{
    return (s_site_ac_ip[0] != '\0') ? s_site_ac_ip : PORTAL_DEF_AC_IP;
}

static const char *site_ssid(void)
{
    return (s_site_ssid[0] != '\0') ? s_site_ssid : PORTAL_DEF_SSID;
}

static void portal_defaults(void);   /* 定义在后面 */

void portal_set_site(const char *base, const char *ac_ip, const char *ssid)
{
    if (base != NULL) {
        strlcpy(s_base, base, sizeof(s_base));
    }
    if (ac_ip != NULL) {
        strlcpy(s_site_ac_ip, ac_ip, sizeof(s_site_ac_ip));
    }
    if (ssid != NULL) {
        strlcpy(s_site_ssid, ssid, sizeof(s_site_ssid));
    }
    /* 门户参数跟着站点走：清掉旧学校抓到的 AC/IP，下次探测重新拿 */
    if (base != NULL && base[0] != '\0') {
        s_wlan.wlan_ac_ip[0] = '\0';
        s_wlan.wlan_ac_name[0] = '\0';
    }
    ESP_LOGI(TAG, "站点参数: base=%s ac=%s ssid=%s",
             portal_base_url(), site_ac_ip(), site_ssid());
    /* 用新的默认值补一次空的 AC/SSID，免得刚改完地址、探测又没拿到参数时是空字段 */
    portal_defaults();
}

#define PORTAL_NVS_NS  "portal"
#define PORTAL_NVS_KEY "wlan"

static void portal_save_wlan(void)
{
    nvs_handle_t h;
    if (nvs_open(PORTAL_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, PORTAL_NVS_KEY, &s_wlan, sizeof(s_wlan));
    nvs_close(h);
}

static void portal_defaults(void)
{
    if (s_wlan.wlan_ac_ip[0] == '\0') {
        strlcpy(s_wlan.wlan_ac_ip, site_ac_ip(), sizeof(s_wlan.wlan_ac_ip));
    }
    if (s_wlan.ssid[0] == '\0') {
        strlcpy(s_wlan.ssid, site_ssid(), sizeof(s_wlan.ssid));
    }
}

static void portal_load_wlan(void)
{
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    nvs_handle_t h;
    if (nvs_open(PORTAL_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    portal_wlan_t tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, PORTAL_NVS_KEY, &tmp, &len) == ESP_OK && len == sizeof(tmp)) {
        s_wlan = tmp;
        ESP_LOGI(TAG, "载入上次门户参数: user_ip=%s ac_ip=%s",
                 s_wlan.wlan_user_ip, s_wlan.wlan_ac_ip);
    }
    nvs_close(h);
    portal_defaults();

    /* 用户显式配置的站点参数优先于上次抓到的缓存：
       否则换了学校、旧学校缓存里的 ac_ip / ssid 会一直把新配置盖住 */
    if (s_site_ac_ip[0] != '\0') {
        strlcpy(s_wlan.wlan_ac_ip, s_site_ac_ip, sizeof(s_wlan.wlan_ac_ip));
        s_wlan.wlan_ac_name[0] = '\0';
    }
    if (s_site_ssid[0] != '\0') {
        strlcpy(s_wlan.ssid, s_site_ssid, sizeof(s_wlan.ssid));
    }
}

void portal_client_set_my_ip(const char *ip)
{
    snprintf(s_my_ip, sizeof(s_my_ip), "%s", ip ? ip : "");
}
static char s_cookie[COOKIE_CAP];

/* 最后一次登录响应的原文（PORTALHTML 命令导出用） */
EXT_RAM_BSS_ATTR static char s_last_reply[4096];
static size_t s_last_reply_len = 0;

/* ------------------------------------------------------------------ */
/* HTTP 基础                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *body;
    size_t body_cap;
    size_t body_len;
    char   location[LOCATION_CAP];
    bool   truncated;
    int    status;
} http_buf_t;

/* 探测模式：body 够了就主动断开，不把整页拉下来（Bing 首页几十 KB） */
static bool s_abort_when_full = false;

/* 最近一次请求是否因为 DNS 解析失败（getaddrinfo 202）而失败 */
static volatile bool s_dns_failed = false;
static volatile bool s_login_busy = false;   /* 登录进行中，测试命令别插队 */

static void store_cookie(const char *raw)
{
    /* 只需要 name=value，忽略 Path/Expires 等属性 */
    const char *semi = strchr(raw, ';');
    size_t n = semi ? (size_t) (semi - raw) : strlen(raw);
    if (n == 0 || n >= COOKIE_CAP || n > 200) {
        return;
    }
    /* 拆出名字（等号左边） */
    const char *eq = memchr(raw, '=', n);
    if (eq == NULL || eq == raw) {
        return;
    }
    size_t name_len = (size_t) (eq - raw);

    /* 同名 cookie 要替换，不能追加。
       门户每次响应都下发新的 JSESSIONID，如果一直追加，
       服务器会取第一个（已失效）的会话，验证码永远对不上。 */
    char *p = s_cookie;
    while (p != NULL && *p != '\0') {
        /* 跳过前导空白 */
        while (*p == ' ') {
            p++;
        }
        if (strncmp(p, raw, name_len) == 0 && p[name_len] == '=') {
            /* 命中同名：把这一段替换掉 */
            char *seg_end = strchr(p, ';');
            size_t tail_len = 0;
            const char *tail = NULL;
            if (seg_end != NULL) {
                tail = seg_end;
                while (*tail == ';' || *tail == ' ') {
                    tail++;
                }
                tail_len = strlen(tail);
            }
            char rest[COOKIE_CAP];
            if (tail != NULL && tail_len > 0) {
                memcpy(rest, tail, tail_len + 1);
            } else {
                rest[0] = '\0';
            }
            size_t prefix = (size_t) (p - s_cookie);
            if (prefix > 0 && s_cookie[prefix - 1] == ' ') {
                prefix--;
            }
            if (prefix > 0 && s_cookie[prefix - 1] == ';') {
                prefix--;
            }
            if (prefix > 0 && s_cookie[prefix - 1] == ' ') {
                prefix--;
            }
            s_cookie[prefix] = '\0';
            /* 追加新值 */
            if (prefix > 0) {
                strlcat(s_cookie, "; ", COOKIE_CAP);
            }
            strlcat(s_cookie, raw, COOKIE_CAP);
            if (rest[0] != '\0') {
                strlcat(s_cookie, "; ", COOKIE_CAP);
                strlcat(s_cookie, rest, COOKIE_CAP);
            }
            ESP_LOGI(TAG, "Cookie 更新: %.*s", (int) name_len, raw);
            return;
        }
        /* 找下一个 ; */
        char *next = strchr(p, ';');
        if (next == NULL) {
            break;
        }
        p = next + 1;
    }

    /* 没有同名 → 追加 */
    if (s_cookie[0] != '\0') {
        size_t used = strlen(s_cookie);
        if (used + n + 3 >= COOKIE_CAP) {
            return;
        }
        s_cookie[used]     = ';';
        s_cookie[used + 1] = ' ';
        memcpy(s_cookie + used + 2, raw, n);
        s_cookie[used + 2 + n] = '\0';
    } else {
        memcpy(s_cookie, raw, n);
        s_cookie[n] = '\0';
    }
}

static esp_err_t http_event(esp_http_client_event_t *e)
{
    http_buf_t *b = (http_buf_t *) e->user_data;

    switch (e->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (b && e->header_key && e->header_value) {
            if (strcasecmp(e->header_key, "Location") == 0) {
                strlcpy(b->location, e->header_value, sizeof(b->location));
            } else if (strcasecmp(e->header_key, "Set-Cookie") == 0) {
                store_cookie(e->header_value);
            }
        }
        break;

    case HTTP_EVENT_ON_DATA:
        if (b && b->body && e->data && e->data_len > 0) {
            if (b->body_len + (size_t) e->data_len < b->body_cap) {
                memcpy(b->body + b->body_len, e->data, (size_t) e->data_len);
                b->body_len += (size_t) e->data_len;
                b->body[b->body_len] = '\0';
            } else {
                b->truncated = true;
                b->status = esp_http_client_get_status_code(e->client);
                if (s_abort_when_full) {
                    return ESP_FAIL;      /* 够判断了，别再往下读 */
                }
            }
        }
        break;

    default:
        break;
    }
    return ESP_OK;
}

/**
 * 通用请求。
 *
 * @param no_redirect  true 时不跟随 302（用来抓 Location）
 * @return HTTP 状态码；<0 表示网络错误
 */
static int http_request(const char *url, esp_http_client_method_t method,
                        const char *post_data, size_t post_len,
                        bool no_redirect,
                        char *body, size_t body_cap, size_t *body_len,
                        char *location, size_t location_cap)
{
    http_buf_t ctx = {
        .body      = body,
        .body_cap  = body_cap,
        .body_len  = 0,
        .truncated = false,
    };
    ctx.location[0] = '\0';

    esp_http_client_config_t cfg = {
        .url                   = url,
        .method                = method,
        .event_handler         = http_event,
        .user_data             = &ctx,
        .timeout_ms            = HTTP_TIMEOUT_MS,
        .disable_auto_redirect = no_redirect,
        .buffer_size           = 1024,
        .buffer_size_tx        = 1024,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        return -1;
    }

    esp_http_client_set_header(cli, "User-Agent", PORTAL_UA);
    esp_http_client_set_header(cli, "Accept", "*/*");
    if (s_cookie[0] != '\0') {
        esp_http_client_set_header(cli, "Cookie", s_cookie);
    }
    if (method == HTTP_METHOD_POST) {
        esp_http_client_set_header(cli, "Content-Type",
                                   "application/x-www-form-urlencoded");
        char referer[PORTAL_BASE_MAX + 16];
    snprintf(referer, sizeof(referer), "%s/index.wlan", portal_base_url());
    esp_http_client_set_header(cli, "Referer", referer);
        esp_http_client_set_post_field(cli, post_data, (int) post_len);
    }

    esp_err_t err = esp_http_client_perform(cli);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(cli)
                                 : (ctx.status > 0 ? ctx.status : -1);
    esp_http_client_cleanup(cli);

    if (body_len) {
        *body_len = ctx.body_len;
    }
    if (location && location_cap > 0) {
        strlcpy(location, ctx.location, location_cap);
    }
    if (ctx.truncated) {
        ESP_LOGW(TAG, "响应被截断（缓冲区 %u 字节）", (unsigned) body_cap);
    }
    return status;
}

/* ------------------------------------------------------------------ */
/* 参数解析                                                           */
/* ------------------------------------------------------------------ */

/**
 * 从 URL 查询串里取参数（大小写不敏感）。
 * 例：param_get(url, "wlanuserip", buf, sizeof buf)
 */
static bool param_get(const char *url, const char *key, char *out, size_t cap)
{
    if (!url || !key || !out || cap == 0) {
        return false;
    }
    char lower_url[LOCATION_CAP * 2];
    char lower_key[32];
    strlcpy(lower_url, url, sizeof(lower_url));
    strlcpy(lower_key, key, sizeof(lower_key));
    for (char *p = lower_url; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p += 32;
    }
    for (char *p = lower_key; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p += 32;
    }

    const char *hit = strstr(lower_url, lower_key);
    while (hit) {
        /* 必须紧跟在 ? 或 & 之后 */
        if (hit == lower_url || *(hit - 1) == '?' || *(hit - 1) == '&') {
            const char *v = hit + strlen(lower_key);
            if (*v == '=') {
                v++;
                size_t n = 0;
                while (v[n] && v[n] != '&' && n + 1 < cap) {
                    n++;
                }
                /* 关键：值要从**原始 url**里取（保持大小写），
                   小写副本只用来定位键名。以前从小写副本复制，
                   导致 SSID 被提交成小写，验证码校验一直失败。 */
                const char *v_orig = url + (v - lower_url);
                memcpy(out, v_orig, n);
                out[n] = '\0';
                return true;
            }
        }
        hit = strstr(hit + 1, lower_key);
    }
    return false;
}

static void wlan_clear(portal_wlan_t *w)
{
    memset(w, 0, sizeof(*w));
}

static bool wlan_fill_from_url(const char *url, portal_wlan_t *w)
{
    char tmp[48];
    bool any = false;

    if (param_get(url, "wlanacname", tmp, sizeof(tmp))) {
        strlcpy(w->wlan_ac_name, tmp, sizeof(w->wlan_ac_name));
        any = true;
    }
    if (param_get(url, "wlanacip", tmp, sizeof(tmp))) {
        strlcpy(w->wlan_ac_ip, tmp, sizeof(w->wlan_ac_ip));
        any = true;
    }
    if (param_get(url, "wlanuserip", tmp, sizeof(tmp))) {
        strlcpy(w->wlan_user_ip, tmp, sizeof(w->wlan_user_ip));
        any = true;
    }
    if (param_get(url, "ssid", tmp, sizeof(tmp))) {
        strlcpy(w->ssid, tmp, sizeof(w->ssid));
        any = true;
    }
    return any && w->wlan_user_ip[0] != '\0';
}

/* ------------------------------------------------------------------ */
/* 探测                                                               */
/* ------------------------------------------------------------------ */

/* 页面里带 bing 才算真的到了 Bing：门户劫持回来的是自己的登录页，
   不会出现这个字样 */
static bool looks_like_bing(const char *body, size_t len)
{
    if (body == NULL || len < 4) {
        return false;
    }
    for (size_t i = 0; i + 4 <= len; i++) {
        if ((body[i] == 'b' || body[i] == 'B') &&
            (body[i + 1] == 'i' || body[i + 1] == 'I') &&
            (body[i + 2] == 'n' || body[i + 2] == 'N') &&
            (body[i + 3] == 'g' || body[i + 3] == 'G')) {
            return true;
        }
    }
    return false;
}

/* 把跳转地址里的门户参数捞出来（AC 地址可能变，能解析就更新） */
static void take_portal_params(const char *url, portal_wlan_t *out)
{
    /* 门户也可能换域名/换 IP：只要 URL 里带门户特征就认 */
    if (strstr(url, "YOUR_PORTAL_IP") == NULL &&
        strstr(url, "index.wlan") == NULL &&
        strstr(url, "wlanuserip") == NULL &&
        strstr(url, "wlanacip") == NULL) {
        return;
    }
    portal_wlan_t w;
    wlan_clear(&w);
    if (wlan_fill_from_url(url, &w)) {
        s_wlan = w;
        portal_save_wlan();       /* 存起来：下次门户不重定向也能登录 */
        if (out) {
            *out = w;
        }
        ESP_LOGI(TAG, "抓到 AC 参数: ip=%s ac=%s ssid=%s",
                 w.wlan_user_ip, w.wlan_ac_ip, w.ssid);
    } else {
        ESP_LOGW(TAG, "跳转到门户但没解析出参数：%s", url);
        if (out) {
            *out = s_wlan;
        }
    }
}

/* 主动触发门户弹窗：请求一个不在白名单里的地址，
   AC 会回 302 跳到门户登录页；从 Location 里把 AC 参数抓下来。
   实测：只有"没网"的时候门户才会弹，所以必须主动撞一下。 */
static bool trigger_portal_redirect(portal_wlan_t *out)
{
    static const char *urls[] = { PROBE_URL3, PROBE_URL4 };
    static char body[512];

    for (int i = 0; i < 2; i++) {
        char loc[LOCATION_CAP] = { 0 };
        size_t blen = 0;
        s_abort_when_full = true;
        int st = http_request(urls[i], HTTP_METHOD_GET, NULL, 0,
                              true, body, sizeof(body), &blen, loc, sizeof(loc));
        s_abort_when_full = false;

        if (loc[0] != '\0' && (strstr(loc, "wlan") != NULL ||
                                strstr(loc, "YOUR_PORTAL_IP") != NULL)) {
            ESP_LOGW(TAG, "触发门户跳转成功: %s", loc);
            take_portal_params(loc, out);
            return true;
        }
        if (st >= 200 && st < 300 && blen > 0) {
            return false;          /* 真能上网，不是门户 */
        }
        ESP_LOGW(TAG, "触发跳转失败（%s -> %d）", urls[i], st);
    }
    return false;
}

/* Bing 通了不代表真的通：再探第二个站点。
   被跳走/被拦/内容不对 → 认为需要认证，并把跳转里的门户参数抓下来 */
static portal_probe_t verify_online(portal_wlan_t *out)
{
    EXT_RAM_BSS_ATTR static char body2[1024];
    char loc2[LOCATION_CAP];
    size_t blen2 = 0;

    s_abort_when_full = true;
    int st2 = http_request(PROBE_URL2, HTTP_METHOD_GET, NULL, 0,
                           true, body2, sizeof(body2), &blen2,
                           loc2, sizeof(loc2));
    s_abort_when_full = false;

    if (st2 >= 200 && st2 < 300 && blen2 > 0) {
        /* 真的是正常网页（不是门户页）就算在线 */
        if (strstr(loc2, "nmc") != NULL || loc2[0] == '\0') {
            return PORTAL_ONLINE;
        }
        ESP_LOGW(TAG, "二次探测被跳到 %s，按门户处理", loc2);
        take_portal_params(loc2, out);
        return PORTAL_NEED_LOGIN;
    }

    if (st2 >= 300 && st2 < 400) {
        ESP_LOGW(TAG, "二次探测被重定向到 %s，按门户处理", loc2);
        take_portal_params(loc2, out);
        return PORTAL_NEED_LOGIN;
    }

    /* 连不上或者被拦：Bing 是白名单，实际没上网 */
    ESP_LOGW(TAG, "二次探测失败(%d)，判定需要认证", st2);
    if (out) {
        *out = s_wlan;
    }
    return PORTAL_NEED_LOGIN;
}

/* 多目标探测：254/200 且内容不是门户页 → 在线。
   返回 1=在线，0=都要认证/都不通，-1=遇到门户跳转（参数已抓） */
/* 联网探测：串行，端点顺序 华为 → 小米 → 百度 → 腾讯
 *   204 = 在线；200 且含门户特征 / 302 跳门户 = 需要认证
 * 不做并发：每条都要新建 http client，反复分配会让内部堆碎片化
 * （实测最大连续块 38K → 31K），收益不明显。
 */

/* ---------------- 探测专用 Keep-Alive 客户端 ----------------
   复用同一条 TCP 连接，避免每 15 秒新建连接产生 TIME_WAIT。
   socket 占用从 4 个降到 1 个，且不随探测频率增长。 */
static esp_http_client_handle_t s_ka_cli = NULL;
static char s_ka_url[160] = { 0 };
static http_buf_t s_ka_ctx;              /* 单线程使用，静态即可 */

static esp_err_t ka_event(esp_http_client_event_t *e)
{
    http_event(e);                       /* 复用原来的数据收集逻辑 */
    return ESP_OK;
}

static esp_err_t ka_ensure_client(const char *url)
{
    if (s_ka_cli != NULL && strcmp(s_ka_url, url) == 0) {
        return ESP_OK;
    }
    if (s_ka_cli != NULL) {
        esp_http_client_cleanup(s_ka_cli);
        s_ka_cli = NULL;
    }
    esp_http_client_config_t cfg = {
        .url                 = url,
        .method              = HTTP_METHOD_GET,
        .event_handler       = ka_event,
        .user_data           = &s_ka_ctx,
        .timeout_ms          = HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .buffer_size         = 1024,
        .buffer_size_tx      = 1024,
        .keep_alive_enable   = true,     /* 关键：保持连接 */
        .keep_alive_idle     = 40,       /* 空闲 40 秒内不关（探测 15 秒一次，够用） */
        .keep_alive_interval = 10,
        .keep_alive_count    = 3,
    };
    s_ka_cli = esp_http_client_init(&cfg);
    if (s_ka_cli == NULL) {
        return ESP_FAIL;
    }
    strlcpy(s_ka_url, url, sizeof(s_ka_url));
    ESP_LOGI(TAG, "探测连接已建立（Keep-Alive）: %s", url);
    return ESP_OK;
}

/* 用 Keep-Alive 连接探测一个地址：返回 HTTP 状态码，<0 失败 */
static int probe_keepalive(const char *url)
{
    if (ka_ensure_client(url) != ESP_OK) {
        return -1;
    }
    s_ka_ctx.body = NULL;
    s_ka_ctx.body_cap = 0;
    s_ka_ctx.body_len = 0;
    s_ka_ctx.truncated = false;
    s_ka_ctx.location[0] = '\0';

    esp_err_t err = esp_http_client_perform(s_ka_cli);
    if (err != ESP_OK) {
        /* 连接可能已被对端关闭，销毁后下次重建 */
        esp_http_client_cleanup(s_ka_cli);
        s_ka_cli = NULL;
        s_ka_url[0] = '\0';
        return -1;
    }
    return esp_http_client_get_status_code(s_ka_cli);
}

/* 主循环退出时（或长时间不用）释放 */
void portal_keepalive_close(void)
{
    if (s_ka_cli != NULL) {
        esp_http_client_cleanup(s_ka_cli);
        s_ka_cli = NULL;
        s_ka_url[0] = '\0';
    }
}

static int probe_multi_online(portal_wlan_t *out)
{
    static const char *urls[] = {
        "http://connectivitycheck.platform.hicloud.com/generate_204",   /* 华为 */
        "http://connect.rom.miui.com/generate_204",                     /* 小米 */
        "http://www.baidu.com/",                                        /* 兜底 */
        "http://www.qq.com/",                                           /* 兜底 */
    };
    EXT_RAM_BSS_ATTR static char body[1024];
    const int64_t t0 = esp_timer_get_time() / 1000;
    bool got_any_response = false;      /* 是否收到过任何 HTTP 状态码 */

    /* 优先用 Keep-Alive 连接探测第一个站点：
       复用同一条 TCP 连接，不产生 TIME_WAIT。
       这一步覆盖了 90% 以上的正常情况。 */
    {
        int kst = probe_keepalive(urls[0]);
        if (kst == 204) {
            ESP_LOGI(TAG, "%s 通（HTTP 204，Keep-Alive），判定在线（耗时 %lld ms）",
                     urls[0], (long long) (esp_timer_get_time() / 1000 - t0));
            return 1;
        }
        if (kst == 200) {
            /* generate_204 端点正常只回 204。回 200 基本就是被门户劫持了
               （把登录页原样返回）。这里不能判"在线"，否则会永远不去认证；
               交给下面的多站点探测，按内容再判一次。 */
            ESP_LOGW(TAG, "%s 回 200（疑似被劫持），改走多站点探测", urls[0]);
        }
        /* 其它状态码/失败 → 走原来的多站点兜底逻辑 */
        ESP_LOGW(TAG, "Keep-Alive 探测返回 %d，改用多站点探测", kst);
    }

    for (int i = 0; i < 4; i++) {
        char loc[LOCATION_CAP] = { 0 };
        size_t blen = 0;
        s_abort_when_full = true;
        int st = http_request(urls[i], HTTP_METHOD_GET, NULL, 0,
                              true, body, sizeof(body), &blen, loc, sizeof(loc));
        s_abort_when_full = false;

        /* 204 = 标准联网检测通过。
           前两个是 generate_204 端点：它们回 200 说明被门户劫持了，
           不能只看状态码就判"在线"。 */
        if (st == 204 || st == 200) {
            bool hijacked = (blen > 0 &&
                             (strstr(body, "wlanuserip") != NULL ||
                              strstr(body, "portalLogin") != NULL));
            if (st == 200 && i < 2) {
                hijacked = true;
            }
            if (hijacked) {
                ESP_LOGW(TAG, "%s 返回门户页，需要认证", urls[i]);
                take_portal_params(urls[i], out);
                return -1;
            }
            ESP_LOGI(TAG, "%s 通（HTTP %d），判定在线（耗时 %lld ms）",
                     urls[i], st, (long long) (esp_timer_get_time() / 1000 - t0));
            return 1;
        }
        /* 302 跳到门户 */
        if (st >= 300 && st < 400 && loc[0] != '\0' &&
            (strstr(loc, "wlan") != NULL || strstr(loc, "YOUR_PORTAL_IP") != NULL)) {
            ESP_LOGW(TAG, "%s 被跳到 %s", urls[i], loc);
            take_portal_params(loc, out);
            return -1;
        }
        if (st >= 0) {
            got_any_response = true;    /* 有响应（哪怕是 4xx/5xx）
                                           说明链路是通的，只是内容不对 */
        }
        ESP_LOGW(TAG, "%s 不通（%d）", urls[i], st);
    }
    /* 一个响应都没拿到 = 链路/路由问题，不是门户。
       返回 -2 让上层报"网络不可达"，而不是傻乎乎去登录 */
    return got_any_response ? 0 : -2;
}


/* 探测站点全部失败时（最常见原因：DNS 解析不了），
   直接用门户 IP 问一次。登录流程本身完全不需要 DNS，
   不能因为 DNS 挂了就永远不认证。 */
static portal_probe_t probe_portal_by_ip(portal_wlan_t *out)
{
    portal_wlan_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    if (!trigger_portal_redirect(&fresh) || fresh.wlan_user_ip[0] == '\0') {
        return PORTAL_UNREACHABLE;
    }
    if (out) {
        *out = fresh;
    }
    ESP_LOGW(TAG, "DNS 不可用，但门户 IP 直接可达 —— 判定为需要登录");
    return PORTAL_NEED_LOGIN;
}


/* 登录前刷新门户参数。
   强制认证时没有 302 跳转，门户首页给的 wlanUserIp 是空的，
   直接提交必然失败。这里按优先级取真实参数：
     ① 之前 302 跳转里抓到并缓存的（含真实公网 IP）
     ② 现在主动撞一次门户跳转
   拿不到就返回 false，调用方不要提交空 IP。 */

/* 问一个 IP 回显服务，拿本机出口公网 IP。
   门户登录时会比对 wlanUserIp 是否等于真实来源 IP，
   缓存里的旧 IP 会导致"用户名或密码错误"。 */
bool portal_get_public_ip(char *out, size_t cap)
{
    static const char *urls[] = {
        "http://ip.3322.net",
        "http://ifconfig.me/ip",
        "http://api.ipify.org",
    };
    for (int i = 0; i < (int) (sizeof(urls) / sizeof(urls[0])); i++) {
        char body[64];
        size_t len = 0;
        int st = http_request(urls[i], HTTP_METHOD_GET, NULL, 0, false,
                              body, sizeof(body) - 1, &len, NULL, 0);
        if (st != 200 || len < 7) {
            continue;
        }
        body[len] = '\0';
        /* 去掉空白，校验 IPv4 形状 */
        char ip[32];
        size_t n = 0;
        for (size_t k = 0; k < len && n < sizeof(ip) - 1; k++) {
            char ch = body[k];
            if ((ch >= '0' && ch <= '9') || ch == '.') {
                ip[n++] = ch;
            }
        }
        ip[n] = '\0';
        int dots = 0;
        for (size_t k = 0; k < n; k++) {
            if (ip[k] == '.') {
                dots++;
            }
        }
        if (dots == 3 && n >= 7 && n <= 15) {
            strlcpy(out, ip, cap);
            ESP_LOGI(TAG, "取到本机出口 IP: %s（来自 %s）", ip, urls[i]);
            return true;
        }
    }
    return false;
}

bool portal_refresh_params(portal_wlan_t *out)
{
    if (out == NULL) {
        return false;
    }
    portal_load_wlan();

    /* ① 先主动撞一次 302：未认证时能拿到最新的真实参数 */
    portal_wlan_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    if (trigger_portal_redirect(&fresh) && fresh.wlan_user_ip[0] != '\0') {
        *out = fresh;
        ESP_LOGI(TAG, "撞 302 拿到最新参数: user_ip=%s ac_ip=%s",
                 out->wlan_user_ip, out->wlan_ac_ip);
        return true;
    }

    /* ② 已经在线时没有 302，就用缓存里的 AC 信息 + 实测的出口 IP。
       门户会比对 user_ip 与真实来源 IP，缓存 IP 过期就会被判"密码错误"。 */
    portal_load_wlan();
    if (s_wlan.wlan_ac_ip[0] != '\0') {
        *out = s_wlan;
        char pub[32] = { 0 };
        if (portal_get_public_ip(pub, sizeof(pub))) {
            strlcpy(out->wlan_user_ip, pub, sizeof(out->wlan_user_ip));
            ESP_LOGI(TAG, "用实测出口 IP 登录: user_ip=%s ac_ip=%s",
                     out->wlan_user_ip, out->wlan_ac_ip);
            return true;
        }
    }

    /* ③ 实在拿不到就退回缓存 */
    if (s_wlan.wlan_user_ip[0] != '\0' && s_wlan.wlan_ac_ip[0] != '\0') {
        *out = s_wlan;
        ESP_LOGW(TAG, "退回缓存参数（IP 可能已过期）: user_ip=%s ac_ip=%s",
                 out->wlan_user_ip, out->wlan_ac_ip);
        return true;
    }

    ESP_LOGE(TAG, "拿不到 wlanUserIp，放弃本次登录（避免提交空 IP 白烧验证码）");
    return false;
}

portal_probe_t portal_probe(portal_wlan_t *out)
{
    portal_load_wlan();

    /* 先用多个站点快速判定到底有没有网（校园网常单独拦 bing） */
    {
        portal_wlan_t tmp2;
        memset(&tmp2, 0, sizeof(tmp2));
        int r = probe_multi_online(&tmp2);
        if (r > 0) {
            return PORTAL_ONLINE;
        }
        if (r == -2) {
            /* 探测站点全都拿不到响应。注意：这通常是 DNS 挂了，
               而门户登录全程走 IP，根本不需要 DNS ——
               所以先用门户 IP 直接问一次，别直接放弃。 */
            ESP_LOGW(TAG, "探测站点全部无响应（多半是 DNS），改用门户 IP 直连判断");
            return probe_portal_by_ip(out);
        }
        if (r < 0) {
            if (out) {
                *out = tmp2;
                if (out->wlan_user_ip[0] == '\0' && s_my_ip[0] != '\0') {
                    strlcpy(out->wlan_user_ip, s_my_ip, sizeof(out->wlan_user_ip));
                }
            }
            return PORTAL_NEED_LOGIN;
        }
        /* r == 0：都不通，继续走原来的 bing 探测（可能是纯离线） */
    }
    EXT_RAM_BSS_ATTR static char body[1024];
    char location[LOCATION_CAP];
    size_t blen = 0;

    s_abort_when_full = true;
    int status = http_request(PROBE_URL, HTTP_METHOD_GET, NULL, 0,
                              true, body, sizeof(body), &blen,
                              location, sizeof(location));
    s_abort_when_full = false;

    if (status < 0) {
        s_dns_failed = true;    /* 连域名都解不出，主循环会重设 DNS */
        /* 连 cn.bing.com 都到不了：先主动撞一次，逼 AC 把门户地址吐出来 */
        ESP_LOGW(TAG, "cn.bing.com 连不上，主动触发门户跳转");
        portal_load_wlan();
        portal_wlan_t fresh;
        memset(&fresh, 0, sizeof(fresh));
        if (trigger_portal_redirect(&fresh) && fresh.wlan_user_ip[0] != '\0') {
            if (out) {
                *out = fresh;
            }
            return PORTAL_NEED_LOGIN;
        }
        /* 连 bing 都没响应，也没从门户跳转里抓到真实参数：
           大概率是网络不通，不要用默认 AC 地址伪造一个门户 */
        ESP_LOGW(TAG, "bing 无响应且抓不到门户参数，判定网络不可达");
        return PORTAL_UNREACHABLE;
    }

    if (status >= 300 && status < 400) {
        /* 跳到 bing 自己的 https 说明路是通的；
           跳到任何别的地址都当成门户（门户换域名也不会误判成在线） */
        if (strstr(location, "bing.com") != NULL) {
            return verify_online(out);
        }
        take_portal_params(location, out);
        ESP_LOGW(TAG, "cn.bing.com 被跳到 %s，按门户处理", location);
        return PORTAL_NEED_LOGIN;
    }

    if (status == 200) {
        if (looks_like_bing(body, blen)) {
            return verify_online(out);
        }
        /* 200 但内容不是 Bing：被门户劫持了 */
        ESP_LOGW(TAG, "cn.bing.com 返回 200 但不是 Bing 内容，按门户处理");
        if (out) {
            *out = s_wlan;
        }
        return PORTAL_NEED_LOGIN;
    }

    ESP_LOGW(TAG, "cn.bing.com 返回 %d，按需要认证处理", status);
    if (out) {
        *out = s_wlan;
        portal_defaults();
        if (out->wlan_ac_ip[0] == '\0') {
            strlcpy(out->wlan_ac_ip, site_ac_ip(), sizeof(out->wlan_ac_ip));
        }
        if (out->ssid[0] == '\0') {
            strlcpy(out->ssid, site_ssid(), sizeof(out->ssid));
        }
        /* 门户没重定向时抓不到参数：user_ip 用当前 IP 顶上 */
        if (s_my_ip[0] != '\0') {
            strlcpy(out->wlan_user_ip, s_my_ip, sizeof(out->wlan_user_ip));
        }
        if (out->wlan_user_ip[0] != '\0') {
            ESP_LOGI(TAG, "改用缓存参数: user_ip=%s ac_ip=%s",
                     out->wlan_user_ip, out->wlan_ac_ip);
        }
    }
    return PORTAL_NEED_LOGIN;
}

const portal_wlan_t *portal_last_wlan(void)
{
    return &s_wlan;
}

/* ------------------------------------------------------------------ */
/* 首页绑定                                                           */
/* ------------------------------------------------------------------ */

int portal_open_index(const portal_wlan_t *w)
{
    if (!w) {
        w = &s_wlan;
    }
    /* 最坏情况：门户地址 127 + 四个 47 字节参数 + 固定分隔符 ≈ 375，
       url 必须留够，否则 -Wformat-truncation 直接报错 */
    char url[PORTAL_BASE_MAX + 256];
    snprintf(url, sizeof(url),
             "%s/index.wlan?wlanuserip=%s&wlanacname=%s&wlanacip=%s&ssid=%s",
             portal_base_url(),
             w->wlan_user_ip, w->wlan_ac_name, w->wlan_ac_ip, w->ssid);

    EXT_RAM_BSS_ATTR static char body[BODY_CAP];   /* static：4KB 不能放栈上，主任务栈会爆 */
    size_t len = 0;
    int status = http_request(url, HTTP_METHOD_GET, NULL, 0, false,
                              body, sizeof(body), &len, NULL, 0);
    ESP_LOGI(TAG, "open_index -> %d (%u 字节)", status, (unsigned) len);
    return (status == 200) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* 验证码                                                             */
/* ------------------------------------------------------------------ */

/* PNG 解码目标：62x25 的 RGB 缓冲 */
typedef struct {
    uint8_t *rgb;
    uint32_t w;
    uint32_t h;
    size_t   cap;
} png_target_t;

static void png_on_init(pngle_t *pngle, uint32_t w, uint32_t h)
{
    png_target_t *t = (png_target_t *) pngle_get_user_data(pngle);
    t->w = w;
    t->h = h;
    ESP_LOGI(TAG, "验证码图片 %ux%u", (unsigned) w, (unsigned) h);
}

static void png_on_draw(pngle_t *pngle, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, const uint8_t rgba[4])
{
    png_target_t *t = (png_target_t *) pngle_get_user_data(pngle);
    for (uint32_t dy = 0; dy < h; dy++) {
        uint32_t py = y + dy;
        if (py >= t->h) {
            continue;
        }
        for (uint32_t dx = 0; dx < w; dx++) {
            uint32_t px = x + dx;
            if (px >= t->w) {
                continue;
            }
            size_t off = ((size_t) py * t->w + px) * 3;
            if (off + 2 < t->cap) {
                t->rgb[off + 0] = rgba[0];
                t->rgb[off + 1] = rgba[1];
                t->rgb[off + 2] = rgba[2];
            }
        }
    }
}

/** 从 JSON 里抠出 "key":"value" */
static bool json_get(const char *json, const char *key, char *out, size_t cap)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return false;
    }
    p += strlen(pattern);
    while (*p == ' ' || *p == ':' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1 < cap) {
        n++;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return n > 0;
}

/**
 * 取一张验证码，返回 ID、识别结果。
 * 图片解码后用本地模型识别。
 */
static bool fetch_captcha(char *code_id, size_t id_cap,
                          char *out_code, size_t out_cap)
{
    ESP_LOGI(TAG, "generateVerifyCode 请求中…");
    /* 1) 拿 ID（4KB 缓冲放静态区，别从调用任务栈上要：串口任务栈会被打爆） */
    EXT_RAM_BSS_ATTR static char body[BODY_CAP];
    size_t len = 0;
    char code_url[PORTAL_BASE_MAX + 40];
    snprintf(code_url, sizeof(code_url), "%s/generateVerifyCode.wlan", portal_base_url());
    int status = http_request(code_url,
                              HTTP_METHOD_POST, "", 0, false,
                              body, sizeof(body), &len, NULL, 0);
    ESP_LOGI(TAG, "POST 返回 status=%d len=%u", status, (unsigned) len);
    if (status != 200 || len == 0) {
        ESP_LOGW(TAG, "generateVerifyCode 失败（%d）", status);
        return false;
    }

    char img_url[256];
    if (!json_get(body, "verifyCode", code_id, id_cap)) {
        ESP_LOGW(TAG, "没解析出 verifyCode：%.80s", body);
        return false;
    }
    if (!json_get(body, "url", img_url, sizeof(img_url))) {
        /* 有些版本只给 code，路径要自己拼 */
        snprintf(img_url, sizeof(img_url), "/imgVerifyCode.wlan?code=%s", code_id);
    }

    /* 2) 下载图片 */
    uint8_t *png = s_last_png;
    char url[384];
    if (img_url[0] == '/') {
        snprintf(url, sizeof(url), "%s%s", portal_base_url(), img_url);
    } else {
        strlcpy(url, img_url, sizeof(url));
    }

    size_t png_len = 0;
    status = http_request(url, HTTP_METHOD_GET, NULL, 0, false,
                          (char *) png, sizeof(s_last_png), &png_len, NULL, 0);
    s_last_png_len = png_len;
    ESP_LOGI(TAG, "IMG 返回 status=%d len=%u url=%.60s", status, (unsigned) png_len, url);
    if (status != 200 || png_len < 64) {
        ESP_LOGW(TAG, "下载验证码图片失败（%d, %u 字节）", status, (unsigned) png_len);
        return false;
    }

    /* 3) 解码 PNG */
    static uint8_t rgb[CAPTCHA_IMG_W * CAPTCHA_IMG_H * 3];
    png_target_t target = {
        .rgb = rgb,
        .w   = CAPTCHA_IMG_W,
        .h   = CAPTCHA_IMG_H,
        .cap = sizeof(rgb),
    };
    memset(rgb, 0, sizeof(rgb));

    ESP_LOGI(TAG, "PNG 头: %02X %02X %02X %02X，大小 %u", png[0], png[1], png[2], png[3], (unsigned) png_len);

    pngle_t *pngle = pngle_new();
    if (!pngle) {
        ESP_LOGW(TAG, "pngle_new 失败（内存不足？）");
        return false;
    }
    pngle_set_user_data(pngle, &target);
    pngle_set_init_callback(pngle, png_on_init);
    pngle_set_draw_callback(pngle, png_on_draw);

    int fed_total = 0;
    size_t off = 0;
    while (off < png_len) {
        int n = pngle_feed(pngle, png + off, png_len - off);
        if (n < 0) {
            ESP_LOGW(TAG, "PNG 解码失败：%s", pngle_error(pngle));
            pngle_destroy(pngle);
            return false;
        }
        if (n == 0) {
            break;          /* 还需要更多数据，但已经喂完了 */
        }
        off += (size_t) n;
        fed_total += n;
    }
    uint32_t w = pngle_get_width(pngle);
    uint32_t h = pngle_get_height(pngle);
    pngle_destroy(pngle);

    ESP_LOGI(TAG, "PNG %ux%u，喂入 %d 字节", (unsigned) w, (unsigned) h, fed_total);
    if (fed_total == 0 || off < png_len) {
        ESP_LOGW(TAG, "PNG 没解完（喂入 %u/%u 字节）",
                 (unsigned) off, (unsigned) png_len);
    }

    if (w != CAPTCHA_IMG_W || h != CAPTCHA_IMG_H) {
        ESP_LOGW(TAG, "图片尺寸不对：%ux%u（期望 %dx%d）",
                 (unsigned) w, (unsigned) h, CAPTCHA_IMG_W, CAPTCHA_IMG_H);
        return false;
    }

    /* 4) 本地识别 */
    char code[CAPTCHA_CHARS + 1] = {0};
    if (captcha_recognize_ex(rgb, code, s_last_conf) != 0) {
        ESP_LOGW(TAG, "识别失败");
        return false;
    }
    strlcpy(out_code, code, out_cap);
    ESP_LOGI(TAG, "验证码: %s（置信度 %.2f %.2f %.2f %.2f）", code,
             s_last_conf[0], s_last_conf[1], s_last_conf[2], s_last_conf[3]);


    /* 交给界面：带屏时可以画出来，并且允许人放弃这张重取 */
    if (s_captcha_cb != NULL) {
        portal_captcha_t cap = {
            .rgb = rgb,
            .w = CAPTCHA_IMG_W,
            .h = CAPTCHA_IMG_H,
            .code = code,
        };
        if (!s_captcha_cb(&cap, s_captcha_user)) {
            ESP_LOGW(TAG, "这张验证码被放弃，重新取一张");
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 表单提交                                                           */
/* ------------------------------------------------------------------ */

static void url_append(char *dst, size_t cap, const char *key, const char *val)
{
    size_t used = strlen(dst);
    if (used + 1 >= cap) {
        return;
    }
    int n = snprintf(dst + used, cap - used, "%s=", key);
    if (n < 0 || (size_t) n >= cap - used) {
        /* snprintf 返回的是"本应写入"的长度：截断了就别往下算，
           否则 used 会越过 cap，后面的 dst[used] 直接越界写 */
        dst[used] = '\0';
        return;
    }
    used += (size_t) n;

    for (const char *p = val; *p && used + 4 < cap; p++) {
        unsigned char c = (unsigned char) *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[used++] = (char) c;
        } else {
            int m = snprintf(dst + used, cap - used, "%%%02X", c);
            if (m < 0) break;
            used += (size_t) m;
        }
    }
    dst[used] = '\0';

    if (used + 2 < cap) {
        dst[used++] = '&';
        dst[used] = '\0';
    }
}

/** 从返回页面里抠出提示文字 text("...") */
static void extract_message(const char *html, char *out, size_t cap)
{
    out[0] = '\0';
    const char *p = strstr(html, "text(");
    while (p) {
        const char *q = p + 5;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '"') {
            q++;
            size_t n = 0;
            while (q[n] && q[n] != '"' && n + 1 < cap) n++;
            memcpy(out, q, n);
            out[n] = '\0';
            return;
        }
        p = strstr(p + 1, "text(");
    }
}

/* 注销：POST /portalLogout.wlan?isCloseWindow=N&<ts>
   字段从门户的下线页面扒出来的：wlanacname / wlanacip / myTime /
   wlanuserip / ssid / logouttimes */
portal_login_t portal_logout(const portal_wlan_t *w, char *msg, size_t msg_len)
{
    if (msg && msg_len) {
        msg[0] = '\0';
    }
    if (w == NULL) {
        return PORTAL_LOGIN_NETERR;
    }

    EXT_RAM_BSS_ATTR static char form[BODY_CAP];
    form[0] = '\0';
    url_append(form, sizeof(form), "wlanacname", w->wlan_ac_name);
    url_append(form, sizeof(form), "wlanacip",  w->wlan_ac_ip);
    url_append(form, sizeof(form), "myTime",    "");
    url_append(form, sizeof(form), "wlanuserip", w->wlan_user_ip);
    url_append(form, sizeof(form), "ssid",      w->ssid);
    url_append(form, sizeof(form), "logouttimes", "1");
    size_t n = strlen(form);
    if (n > 0 && form[n - 1] == '&') {
        form[n - 1] = '\0';
    }

    char url[PORTAL_BASE_MAX + 64];
    snprintf(url, sizeof(url), "%s/portalLogout.wlan?isCloseWindow=N&%" PRIu64,
             portal_base_url(), (uint64_t) (esp_timer_get_time() / 1000));

    EXT_RAM_BSS_ATTR static char body[BODY_CAP];
    size_t len = 0;
    int status = http_request(url, HTTP_METHOD_POST, form, strlen(form), false,
                              body, sizeof(body), &len, NULL, 0);
    if (status < 0 || len == 0) {
        ESP_LOGW(TAG, "注销请求失败（%d）", status);
        return PORTAL_LOGIN_NETERR;
    }
    ESP_LOGI(TAG, "门户原始回复（%u 字节）: %.600s", (unsigned) len, body);
    char text[PORTAL_MAX_MSG];
    extract_message(body, text, sizeof(text));
    if (msg && msg_len) {
        snprintf(msg, msg_len, "%s", text);
    }
    ESP_LOGI(TAG, "注销结果: %s", text);

    if (strstr(body, "成功") != NULL || strstr(body, "success") != NULL) {
        return PORTAL_LOGIN_OK;
    }
    return PORTAL_LOGIN_FAILED;
}

static portal_login_t submit_login_ex(const char *user, const char *user_pwd_enc,
                                      const char *code, const char *code_id,
                                      const portal_wlan_t *w, bool force,
                                      char *msg, size_t msg_len)
{
    char form[BODY_CAP];
    form[0] = '\0';

    url_append(form, sizeof(form), "loginmode",  "static");
    url_append(form, sizeof(form), "passType",   "1");
    url_append(form, sizeof(form), "wlanAcName", w->wlan_ac_name);
    url_append(form, sizeof(form), "wlanAcIp",   w->wlan_ac_ip);
    url_append(form, sizeof(form), "wlanUserIp", w->wlan_user_ip);
    url_append(form, sizeof(form), "ssid",       w->ssid);
    url_append(form, sizeof(form), "userName",   user);
    url_append(form, sizeof(form), "userPwd",    user_pwd_enc);
    url_append(form, sizeof(form), "verifyCode", code);
    url_append(form, sizeof(form), "verifyHidden", code_id);
    if (force) {
        /* 门户回"已在线，是否继续操作？"时，带上这个参数就是点"继续" */
        url_append(form, sizeof(form), "forceLogin", "1");
    }

    /* 去掉末尾多余的 & */
    size_t n = strlen(form);
    if (n > 0 && form[n - 1] == '&') {
        form[n - 1] = '\0';
    }

    /* 调试：把表单打出来（密码打码），方便和 PC 端对比。
       注意：必须 static —— form 已经占了 4KB 栈，再放 4KB 会打爆 12KB 主任务栈
       （实测 "***ERROR*** A stack overflow in task main" 无限重启）。 */
    {
        EXT_RAM_BSS_ATTR static char dbg[BODY_CAP];
        strlcpy(dbg, form, sizeof(dbg));
        char *uf = strstr(dbg, "userPwd=");
        if (uf != NULL) {
            char *amp = strchr(uf, '&');
            size_t keep = 8;
            if (amp != NULL && (size_t) (amp - uf) > keep) {
                /* 保留前 8 个字符，其余替换成 * */
                for (char *q = uf + keep; q < amp; q++) {
                    *q = '*';
                }
            }
        }
        ESP_LOGI(TAG, "提交表单: %s（%u 字节）", dbg, (unsigned) strlen(form));
    }

    char url[PORTAL_BASE_MAX + 48];
    snprintf(url, sizeof(url), "%s/portalLogin.wlan?%" PRIu64,
             portal_base_url(), (uint64_t) (esp_timer_get_time() / 1000));

    EXT_RAM_BSS_ATTR static char body[BODY_CAP];   /* static：4KB 不能放栈上，主任务栈会爆 */
    size_t len = 0;
    int status = http_request(url, HTTP_METHOD_POST, form, strlen(form), false,
                              body, sizeof(body), &len, NULL, 0);
    if (status < 0 || len == 0) {
        ESP_LOGW(TAG, "提交登录失败（%d）", status);
        return PORTAL_LOGIN_NETERR;
    }
    if (status >= 400) {
        /* 5xx/403 的错误页里什么关键字都没有，不能当成功 */
        ESP_LOGW(TAG, "门户返回 HTTP %d", status);
        return PORTAL_LOGIN_NETERR;
    }
    ESP_LOGI(TAG, "登录原始回复（%u 字节）: %.900s", (unsigned) len, body);
    /* 存原文，供 PORTALHTML 导出 */
    {
        size_t cp = len < sizeof(s_last_reply) - 1 ? len : sizeof(s_last_reply) - 1;
        memcpy(s_last_reply, body, cp);
        s_last_reply[cp] = '\0';
        s_last_reply_len = cp;
    }
    /* 正文填满说明可能被截断：关键字也许落在缓冲区外面 */
    bool maybe_cut = (len >= BODY_CAP - 1);

    char text[PORTAL_MAX_MSG];
    extract_message(body, text, sizeof(text));
    if (msg && msg_len) {
        strlcpy(msg, text, msg_len);
    }
    ESP_LOGI(TAG, "服务器提示: %s", text[0] ? text : "(无)");

    if (strstr(body, "已在线") != NULL) {
        return PORTAL_LOGIN_ALREADY;
    }
    if (strstr(body, "验证码错误") != NULL ||
        strstr(body, "验证码不正确") != NULL ||
        strstr(body, "请输入验证码") != NULL) {
        return PORTAL_LOGIN_CAPTCHA;
    }
    if (strstr(body, "登录成功") != NULL || strstr(body, "成功登录") != NULL) {
        return PORTAL_LOGIN_OK;
    }
    if (strstr(body, "错误") != NULL || strstr(body, "失败") != NULL) {
        return PORTAL_LOGIN_FAILED;
    }
    if (maybe_cut) {
        /* 结果页超过 4KB 被截断，没看到结论就不能猜成功 */
        ESP_LOGW(TAG, "登录结果页被截断（%u 字节），无法判断", (unsigned) len);
        if (msg && msg_len) {
            strlcpy(msg, "结果页太大，无法判断", msg_len);
        }
        return PORTAL_LOGIN_NETERR;
    }
    /* 没有明显报错，当成成功 */
    return PORTAL_LOGIN_OK;
}



/* 从 HTML 里取出 <input name="X" ... value="Y"> 的值 */
static bool html_input_value(const char *html, const char *name, char *out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "name=\"%s\"", name);
    const char *p = strstr(html, pat);
    if (p == NULL) {
        return false;
    }
    const char *v = strstr(p, "value=\"");
    if (v == NULL) {
        return false;
    }
    v += 7;
    const char *e = strchr(v, '"');
    if (e == NULL) {
        return false;
    }
    size_t n = (size_t) (e - v);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, v, n);
    out[n] = '\0';
    return true;
}

/* 跟着门户的自动跳转表单走完最后一步 */
static portal_login_t follow_login_redirect(const char *html,
                                            char *msg, size_t msg_len)
{
    static const char *keys[] = {
        "synccitycheck", "AUTO_LOGIN", "wlanUserIp", "isFlowTime", "remainTime",
        "wlanAcName", "wlanAcIp", "userName", "ssid", "cookies",
        "encryUser", "authInfo", "SELF_SERVICE_URL", "passType", "autoLoginMode",
    };
    EXT_RAM_BSS_ATTR static char form[BODY_CAP];    /* static：4KB 不能放栈上 */
    form[0] = '\0';

    int n_field = (int) (sizeof(keys) / sizeof(keys[0]));
    for (int i = 0; i < n_field; i++) {
        char val[512];
        if (!html_input_value(html, keys[i], val, sizeof(val))) {
            continue;
        }
        url_append(form, sizeof(form), keys[i], val);
    }
    size_t n = strlen(form);
    if (n > 0 && form[n - 1] == '&') {
        form[n - 1] = '\0';
    }
    ESP_LOGI(TAG, "跳转表单: %.300s", form);

    EXT_RAM_BSS_ATTR static char body[BODY_CAP];
    size_t len = 0;
    char redirect_url[PORTAL_BASE_MAX + 40];
    snprintf(redirect_url, sizeof(redirect_url), "%s/portalLoginRedirect.wlan", portal_base_url());
    int status = http_request(redirect_url,
                              HTTP_METHOD_POST, form, strlen(form), false,
                              body, sizeof(body), &len, NULL, 0);
    ESP_LOGI(TAG, "跳转返回 status=%d len=%u", status, (unsigned) len);
    if (status < 0 || len == 0) {
        return PORTAL_LOGIN_NETERR;
    }
    if (len < sizeof(s_last_reply)) {
        memcpy(s_last_reply, body, len);
        s_last_reply[len] = '\0';
        s_last_reply_len = len;
    }
    char text[PORTAL_MAX_MSG];
    extract_message(body, text, sizeof(text));
    if (msg && msg_len) {
        strlcpy(msg, text, msg_len);
    }
    ESP_LOGI(TAG, "跳转提示: %s", text[0] ? text : "(无)");

    if (strstr(body, "成功") != NULL || strstr(body, "欢迎") != NULL ||
        strstr(body, "注销") != NULL) {
        return PORTAL_LOGIN_OK;
    }
    return PORTAL_LOGIN_OK;         /* 跳转完成一般就是成功，探测会再确认 */
}

/* ---------------- 强制登录（顶号） ----------------
   门户在"已在线，是否继续操作？"页面里给了一个现成表单：
       POST /portalForceLogin.wlan
       wlanAcName / wlanAcIp / wlanUserIp / ssid / passType
       userName / userPwd（明文）/ autoLogin / onlineInfo
   不需要验证码，也不用 RSA。按这个表单原样提交即可顶掉旧会话。 */
portal_login_t portal_force_login(const char *user, const char *password,
                                  const portal_wlan_t *w,
                                  char *msg, size_t msg_len)
{
    EXT_RAM_BSS_ATTR static char form[BODY_CAP];    /* static：4KB 不能放栈上 */
    form[0] = '\0';

    url_append(form, sizeof(form), "wlanAcName", w->wlan_ac_name);
    url_append(form, sizeof(form), "wlanAcIp",   w->wlan_ac_ip);
    url_append(form, sizeof(form), "wlanUserIp", w->wlan_user_ip);
    url_append(form, sizeof(form), "ssid",       w->ssid);
    url_append(form, sizeof(form), "passType",   "1");
    url_append(form, sizeof(form), "userName",   user);
    url_append(form, sizeof(form), "userPwd",    password);
    url_append(form, sizeof(form), "autoLogin",  "false");

    char info[128];
    snprintf(info, sizeof(info), "{\"acip\":\"%s\",\"userip\":\"%s\"}",
             w->wlan_ac_ip, w->wlan_user_ip);
    url_append(form, sizeof(form), "onlineInfo", info);

    size_t n = strlen(form);
    if (n > 0 && form[n - 1] == '&') {
        form[n - 1] = '\0';
    }

    {
        EXT_RAM_BSS_ATTR static char dbg[BODY_CAP];
        strlcpy(dbg, form, sizeof(dbg));
        char *uf = strstr(dbg, "userPwd=");
        if (uf != NULL) {
            char *amp = strchr(uf, '&');
            if (amp != NULL) {
                /* "userPwd=" 是 8 个字符，从第 9 个开始才是密码。
                   以前这里写的 uf + 14，会把密码前 6 个字符打进日志 */
                for (char *q = uf + 8; q < amp; q++) {
                    *q = '*';
                }
            }
        }
        ESP_LOGI(TAG, "强制登录表单: %s", dbg);
    }

    EXT_RAM_BSS_ATTR static char body[BODY_CAP];
    size_t len = 0;
    char force_url[PORTAL_BASE_MAX + 40];
    snprintf(force_url, sizeof(force_url), "%s/portalForceLogin.wlan", portal_base_url());
    int status = http_request(force_url,
                              HTTP_METHOD_POST, form, strlen(form), false,
                              body, sizeof(body), &len, NULL, 0);
    ESP_LOGI(TAG, "强制登录返回 status=%d len=%u", status, (unsigned) len);
    if (status < 0 || len == 0) {
        return PORTAL_LOGIN_NETERR;
    }
    if (len < sizeof(s_last_reply)) {
        memcpy(s_last_reply, body, len);
        s_last_reply[len] = '\0';
        s_last_reply_len = len;
    }
    ESP_LOGI(TAG, "强制登录原文（%u 字节）: %.500s", (unsigned) len, body);

    char text[PORTAL_MAX_MSG];
    extract_message(body, text, sizeof(text));
    if (msg && msg_len) {
        strlcpy(msg, text, msg_len);
    }
    ESP_LOGI(TAG, "强制登录提示: %s", text[0] ? text : "(无)");

    /* 门户回的是自动跳转表单（portalLoginRedirect.wlan）→ 这是"确认"后的
       正常流程，必须像浏览器那样跟着提交一次。 */
    if (strstr(body, "portalLoginRedirect.wlan") != NULL) {
        ESP_LOGI(TAG, "门户要求跳转确认，继续提交");
        return follow_login_redirect(body, msg, msg_len);
    }

    /* 严格判定：必须看到明确的成功标记才算成功。
       实测门户拒绝时会原样返回登录页（标题"中国联通校园宽带业务"，
       里面既没有"成功"也没有"失败"），按旧逻辑会误判成功。 */
    if (strstr(body, "认证成功") != NULL || strstr(body, "登录成功") != NULL ||
        strstr(body, "注销") != NULL || strstr(body, "已登录") != NULL) {
        return PORTAL_LOGIN_OK;
    }
    if (strstr(body, "已在线") != NULL) {
        return PORTAL_LOGIN_ALREADY;
    }
    ESP_LOGW(TAG, "强制登录被拒（返回的是登录页，%u 字节）", (unsigned) len);
    return PORTAL_LOGIN_FAILED;
}


static portal_login_t portal_login_inner(const char *user, const char *password,
                                         const portal_wlan_t *w,
                                         char *msg, size_t msg_len)
{
    if (!user || !password) {
        return PORTAL_LOGIN_FAILED;
    }
    if (!w) {
        w = &s_wlan;
    }

    char enc[PWD_ENC_MAX_LEN + 1];
    if (portal_encrypt_password(password, enc, sizeof(enc)) != 0) {
        ESP_LOGE(TAG, "密码加密失败");
        return PORTAL_LOGIN_FAILED;
    }

    portal_open_index(w);

    portal_login_t last = PORTAL_LOGIN_NETERR;
    bool want_force = false;      /* 下一轮提交是否带 forceLogin=1（顶号） */
    for (int attempt = 0; attempt < 4; attempt++) {
        char code_id[64] = {0};
        char code[CAPTCHA_CHARS + 1] = {0};

        /* 置信度不够就换一张：取图是免费的，只有提交才消耗那一次机会。
           这样把"模型认错"的概率从 2.7% 压到接近 0。 */
        bool got = false;
        for (int reroll = 0; reroll < CAPTCHA_REROLL_MAX; reroll++) {
            if (!fetch_captcha(code_id, sizeof(code_id), code, sizeof(code))) {
                vTaskDelay(pdMS_TO_TICKS(800));
                continue;
            }
            got = true;
            float cmin = s_last_conf[0];
            for (int i = 1; i < CAPTCHA_CHARS; i++) {
                if (s_last_conf[i] < cmin) {
                    cmin = s_last_conf[i];
                }
            }
            if (cmin >= CAPTCHA_CONF_MIN) {
                break;                    /* 够自信，提交 */
            }
            ESP_LOGW(TAG, "置信度 %.2f 偏低（第 %d 次换图），丢弃 %s 重取",
                     cmin, reroll + 1, code);
            if (reroll + 1 >= CAPTCHA_REROLL_MAX) {
                ESP_LOGW(TAG, "换了 %d 次仍不自信，就用这次的结果兜底",
                         CAPTCHA_REROLL_MAX);
            }
        }
        if (!got) {
            continue;
        }

        if (want_force) {
            ESP_LOGW(TAG, "再次提交（forceLogin=1，新验证码 %s）", code);
        }
        ESP_LOGI(TAG, "提交登录：code=%s id=%.16s", code, code_id);
        last = submit_login_ex(user, enc, code, code_id, w, want_force, msg, msg_len);

        /* 门户回"已在线，是否继续操作？"：再发一次带 forceLogin=1 的请求，
           相当于在网页上点"继续"，把别的设备的会话顶掉，
           确保这台板子的 IP 真正被授权（否则会出现"账号在线但本机没网"）。 */
        if (last == PORTAL_LOGIN_ALREADY) {
            /* 验证码是一次性的：第一次提交（回"已在线"）已把它消耗掉，
               所以顶号必须重新取图、重新识别，不能用旧的 code。 */
            ESP_LOGW(TAG, "账号已在线，用门户的强制登录接口顶号");
            portal_login_t frc = portal_force_login(user, password, w, msg, msg_len);
            if (frc == PORTAL_LOGIN_OK) {
                ESP_LOGI(TAG, "强制登录成功，本机已授权");
                last = PORTAL_LOGIN_OK;
            } else {
                ESP_LOGW(TAG, "强制登录未成功（%d），按已在线处理", frc);
                last = PORTAL_LOGIN_ALREADY;
            }
        }
        /* 注：曾试过"某一位改成 9"的候选，但实测门户验证码只出 0-8
           （用门户自身当预言机标注 150 张，含 9 的 0 张），已撤掉。 */
        if (last == PORTAL_LOGIN_CAPTCHA) {
            /* 把这次识别结果和执行次数打出来：
               如果是 model 认错，日志里看到的验证码就是错的答案，
               和网络失败（NETERR）能区分开。 */
            ESP_LOGW(TAG, "验证码被拒：识别为 %s（第 %d/4 次），换一张重试",
                     code, attempt + 1);
            continue;
        }
        return last;
    }
    return last;
}
portal_login_t portal_login(const char *user, const char *password,
                            const portal_wlan_t *w,
                            char *msg, size_t msg_len)
{
    s_login_busy = true;
    portal_login_t r = portal_login_inner(user, password, w, msg, msg_len);
    s_login_busy = false;
    return r;
}

/* 仅测试：取一张验证码并本地识别，不提交登录。
   返回 0 成功；识别结果写 code，置信度写 conf。 */
int portal_captcha_test(char *code, size_t cap, float *conf)
{
    if (s_login_busy) {
        ESP_LOGW(TAG, "登录进行中，测试命令先别插队");
        return -4;
    }

    const portal_wlan_t *w = portal_last_wlan();
    if (w != NULL && w->wlan_ac_ip[0] != '\0') {
        if (portal_open_index(w) != 0) {
            ESP_LOGW(TAG, "open_index 失败，还是先试着取一张");
        }
    } else {
        ESP_LOGW(TAG, "还没有门户参数，直接试取验证码");
    }

    char id[64];
    ESP_LOGI(TAG, "抓一张验证码看看");
    bool ok = fetch_captcha(id, sizeof(id), code, cap);
    ESP_LOGI(TAG, "抓取结果 %d，识别=%s", (int) ok, ok ? code : "-");
    if (!ok) {
        return -3;
    }
    if (conf != NULL) {
        for (int i = 0; i < CAPTCHA_CHARS; i++) {
            conf[i] = s_last_conf[i];
        }
    }
    return 0;
}
/* 测试用：把最近一次下载的验证码 PNG 交出去 */
const uint8_t *portal_last_captcha_png(size_t *len)
{
    if (len) {
        *len = s_last_png_len;
    }
    return s_last_png;
}


/* 主循环用：查询并清除"DNS 解析失败"标记 */
bool portal_take_dns_failed(void)
{
    bool v = s_dns_failed;
    s_dns_failed = false;
    return v;
}


/* 供串口导出：最后一次登录响应原文 */
size_t portal_last_reply(const char **out)
{
    if (out) {
        *out = s_last_reply;
    }
    return s_last_reply_len;
}
