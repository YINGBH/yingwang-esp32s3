#include "hitokoto.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"

#include "quotes_data.h"

static const char *TAG = "hitokoto";

#define HITO_MAX_CJK  16
#define HITO_BODY_MAX 4096

/* 说明：
   1) 走 HTTP（80）不走 HTTPS：这个网络里 443 会被重置；
   2) 先按域名请求；板子所在网络的 DNS 有时解析不出 v1.hitokoto.cn，
      再退到 Cloudflare 的 IP + Host 头（实测可用）。 */
#define HITO_HOST  "v1.hitokoto.cn"
#define HITO_PATH  "/?encode=text&charset=utf-8&c=d&c=i&c=k"
#define HITO_URL   "http://" HITO_HOST HITO_PATH

/* 兜底：仓库里的短句列表（一言语料摘的 80 句），走 GitHub raw */
#define HITO_QUOTES_URL \
    "https://raw.githubusercontent.com/YINGBH/yingwang-esp32s3/main/docs/quotes.txt"

static const char *FALLBACK_IPS[] = {
    "104.21.63.38",
    "172.67.169.115",
};

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} resp_ctx_t;

static esp_err_t http_event(esp_http_client_event_t *ev)
{
    resp_ctx_t *ctx = (resp_ctx_t *) ev->user_data;
    if (ctx == NULL) {
        return ESP_OK;
    }
    if (ev->event_id == HTTP_EVENT_ON_DATA && ev->data && ev->data_len > 0) {
        size_t room = ctx->cap - 1 - ctx->len;
        size_t n = ((size_t) ev->data_len < room) ? (size_t) ev->data_len : room;
        if (n > 0) {
            memcpy(ctx->buf + ctx->len, ev->data, n);
            ctx->len += n;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

/* 请求一次；host_header 非空时用它做 Host 头（IP 直连时用） */
static bool try_fetch(const char *url, const char *host_header, resp_ctx_t *ctx)
{
    ctx->len = 0;
    ctx->buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event,
        .user_data = ctx,
        .timeout_ms = 12000,
        .keep_alive_enable = false,
    };
    if (host_header) {
        cfg.host = host_header;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || ctx->len == 0) {
        ESP_LOGW(TAG, "请求失败 %s: %s (HTTP %d, %u 字节)",
                 url, esp_err_to_name(err), status, (unsigned) ctx->len);
        return false;
    }
    return true;
}


/* 从一行文本里挑出汉字，最多 maxc 个 */
static int pick_cjk(const char *src, char *out, size_t cap, int maxc)
{
    size_t o = 0;
    int cjk = 0;
    const unsigned char *q = (const unsigned char *) src;
    while (*q && cjk < maxc && o + 4 < cap) {
        if (*q < 0x80) {
            q++;
            continue;
        }
        if ((*q & 0xF0) == 0xE0 && q[1] && q[2]) {
            if (*q >= 0xE4 && *q <= 0xE9) {
                memcpy(out + o, q, 3);
                o += 3;
                cjk++;
            }
            q += 3;
        } else if ((*q & 0xF8) == 0xF0 && q[1] && q[2] && q[3]) {
            q += 4;
        } else if ((*q & 0xE0) == 0xC0 && q[1]) {
            q += 2;
        } else {
            q++;
        }
    }
    out[o] = '\0';
    return cjk;
}

bool hitokoto_fetch(char *out, size_t cap)
{
    if (out == NULL || cap < 8) {
        return false;
    }
    out[0] = '\0';

    static char *s_body = NULL;
    if (s_body == NULL) {
        s_body = (char *) malloc(HITO_BODY_MAX);
        if (s_body == NULL) {
            return false;
        }
    }
    resp_ctx_t ctx = { .buf = s_body, .cap = HITO_BODY_MAX, .len = 0 };

    bool ok = try_fetch(HITO_URL, NULL, &ctx);
    if (!ok) {
        for (size_t i = 0; i < sizeof(FALLBACK_IPS) / sizeof(FALLBACK_IPS[0]) && !ok; i++) {
            char url[96];
            snprintf(url, sizeof(url), "http://%s%s", FALLBACK_IPS[i], HITO_PATH);
            ESP_LOGI(TAG, "改用 IP 重试：%s", FALLBACK_IPS[i]);
            ok = try_fetch(url, HITO_HOST, &ctx);
        }
    }
    if (!ok) {
        /* 官方接口不通（校园网到 Cloudflare 可能直接不可达），
           改用仓库里的短句列表：拉一份文本，随机挑一行 */
        ESP_LOGI(TAG, "官方接口不通，改拉仓库短句列表");
        ok = try_fetch(HITO_QUOTES_URL, NULL, &ctx);
        if (ok) {
            /* 按行切，随机挑一条非注释行 */
            int lines = 0;
            for (char *p = s_body; *p; p++) {
                if (*p == '\n') {
                    lines++;
                }
            }
            int want = (int) (esp_random() % (uint32_t) (lines > 0 ? lines : 1));
            char *p = s_body;
            for (int i = 0; i < want && p; i++) {
                p = strchr(p, '\n');
                if (p) {
                    p++;
                }
            }
            if (p) {
                char *nl = strchr(p, '\n');
                if (nl) {
                    *nl = '\0';
                }
                if (p[0] == '#' || p[0] == '\0') {
                    p = NULL;
                }
            }
            if (p) {
                pick_cjk(p, out, cap, HITO_MAX_CJK);
                if (out[0]) {
                    ESP_LOGI(TAG, "从中取到：%s", out);
                    return true;
                }
            }
        }
        /* 连仓库也拿不到（DNS 被校园网过滤）：用固件内置的 80 句 */
        {
            const char *q = HITO_QUOTES[esp_random() % HITO_QUOTE_COUNT];
            pick_cjk(q, out, cap, HITO_MAX_CJK);
            if (out[0]) {
                ESP_LOGI(TAG, "网络不通，用内置句子：%s", out);
                return true;
            }
        }
        return false;
    }

    int cjk = pick_cjk(s_body, out, cap, HITO_MAX_CJK);
    if (cjk == 0) {
        ESP_LOGW(TAG, "一言没有汉字可用（%u 字节）", (unsigned) ctx.len);
        return false;
    }
    ESP_LOGI(TAG, "拿到一言：%s", out);
    return true;
}
