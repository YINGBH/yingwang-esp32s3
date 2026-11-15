/*
 * 联通校园宽带门户客户端（ESP32-S3）
 *
 * 完整流程（与 PC 端脚本 portal_login.py 一致）：
 *   1. 探测：请求一个固定 URL，被 AC 劫持说明未认证，从 Location 里拿到 wlan 参数
 *   2. open_index：带参数访问门户首页，服务器据此把会话和客户端 IP 绑起来
 *   3. generateVerifyCode：拿验证码 ID 和 PNG 图片
 *   4. 本地识别验证码（captcha 组件）
 *   5. RSA + 双重 Base64 加密密码
 *   6. POST portalLogin.wlan 提交
 */
#ifndef PORTAL_CLIENT_H
#define PORTAL_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORTAL_MAX_MSG 160

/* AC 下发的四个参数 */
typedef struct {
    char wlan_ac_name[48];
    char wlan_ac_ip[48];
    char wlan_user_ip[48];
    char ssid[48];
} portal_wlan_t;

/* 探测结果 */
typedef enum {
    PORTAL_ONLINE = 0,      /* 已认证，可以上网 */
    PORTAL_NEED_LOGIN = 1,  /* 未认证，wlan 参数已填好 */
    PORTAL_UNREACHABLE = 2, /* 网络根本不通 */
} portal_probe_t;

/* 登录结果 */
typedef enum {
    PORTAL_LOGIN_OK = 0,       /* 登录成功 */
    PORTAL_LOGIN_ALREADY = 1,  /* 已在线（凭据有效） */
    PORTAL_LOGIN_CAPTCHA = 2,  /* 验证码被拒，值得重试 */
    PORTAL_LOGIN_FAILED = 3,   /* 账号/密码或其它错误 */
    PORTAL_LOGIN_NETERR = 4,   /* 网络/协议错误 */
} portal_login_t;

/**
 * 探测当前网络状态。
 * NEED_LOGIN 时会把 AC 参数写进 out。
 */
portal_probe_t portal_probe(portal_wlan_t *out);

/** 登录前刷新门户参数（拿不到就返回 false） */
bool portal_refresh_params(portal_wlan_t *out);

/** 取本机出口公网 IP */
bool portal_get_public_ip(char *out, size_t cap);

/** 查询并清除"最近一次请求因 DNS 失败"标记 */
bool portal_take_dns_failed(void);

/** 最后一次登录响应原文（调试导出用） */
size_t portal_last_reply(const char **out);

/** 关闭探测用的 Keep-Alive 连接 */
void portal_keepalive_close(void);

/** 带参数访问门户首页，建立会话绑定。成功返回 0。 */
int portal_open_index(const portal_wlan_t *w);

/**
 * 完整登录一次（内部会自己重试验证码）。
 *
 * @param user     手机号
 * @param password 明文密码
 * @param w        AC 参数（可为 NULL，则用内部保存的）
 * @param msg      输出服务器提示文字
 * @param msg_len  msg 缓冲区大小
 */
portal_login_t portal_login(const char *user, const char *password,
                            const portal_wlan_t *w,
                            char *msg, size_t msg_len);

/* ── 验证码回调（可选）───────────────────────────────
 *
 * 每取到一张验证码，识别完会调一次回调，把图片和识别结果交出去。
 * 板子上带屏时可以用来把验证码画到屏幕上，让人眼看一眼。
 *
 * 回调返回 true  = 这张可以用，继续提交
 * 回调返回 false = 放弃这张，重新取一张（比如用户按了按键说识别错了）
 */
typedef struct {
    const uint8_t *rgb;   /* RGB888，宽*高*3 字节 */
    int            w;
    int            h;
    const char    *code;  /* 本地模型的识别结果 */
} portal_captcha_t;

typedef bool (*portal_captcha_cb_t)(const portal_captcha_t *cap, void *user);

/* ── 站点参数（门户地址 / AC IP / 校园网 SSID）─────────────
 *
 * 以前这三项是编译期写死的宏，换一所学校就得改代码重编。
 * 现在由 main 在加载配置后调用一次；串口 `SET portal_base=...` 改完也会重新调用。
 * 传空串表示这一项用编译期默认值（app_config.h 里的 APP_PORTAL_*）。
 */
void portal_set_site(const char *base, const char *ac_ip, const char *ssid);

/** 当前生效的门户地址（永远非空，没配置时返回编译期默认值） */
const char *portal_base_url(void);

/** 注册验证码回调。传 NULL 取消。 */
void portal_set_captcha_callback(portal_captcha_cb_t cb, void *user);

/** 告诉门户组件当前 IP（登录时覆盖 user_ip） */
void portal_client_set_my_ip(const char *ip);

/** 注销当前在线会话（校园网） */
portal_login_t portal_logout(const portal_wlan_t *w, char *msg, size_t msg_len);

/** 取上次抓到的参数（portal_probe 成功时会更新）。 */
const portal_wlan_t *portal_last_wlan(void);

/** 仅测试：取一张验证码并本地识别（不提交登录）。0=成功 */
int portal_captcha_test(char *code, size_t cap, float *conf);

/** 测试用：最近一次下载的验证码 PNG */
const uint8_t *portal_last_captcha_png(size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* PORTAL_CLIENT_H */