#include "netstats.h"

#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "netstats";

/* 仓库里的汇总文件（板子只读） */
#define NETSTATS_URL  "https://raw.githubusercontent.com/YINGBH/yingwang-esp32s3/main/stats/latest.json"
#define NETSTATS_URL2 "http://raw.githubusercontent.com/YINGBH/yingwang-esp32s3/main/stats/latest.json"

#define BUCKETS 99

static int s_buckets[BUCKETS];
static int s_users = 0;
static bool s_ok = false;

typedef struct {
    char buf[4096];
    int len;
} resp_t;

static esp_err_t http_event(esp_http_client_event_t *e)
{
    resp_t *r = (resp_t *) e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0 && r) {
        int room = (int) sizeof(r->buf) - 1 - r->len;
        int n = e->data_len < room ? e->data_len : room;
        if (n > 0) {
            memcpy(r->buf + r->len, e->data, n);
            r->len += n;
            r->buf[r->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool fetch(const char *url, resp_t *r)
{
    r->len = 0;
    r->buf[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event,
        .user_data = r,
        .timeout_ms = 12000,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return false;
    }
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200 || r->len == 0) {
        ESP_LOGW(TAG, "拉取失败 %s: %s (HTTP %d, %d 字节)", url,
                 esp_err_to_name(err), status, r->len);
        return false;
    }
    return true;
}

esp_err_t netstats_refresh(void)
{
    static resp_t resp;                 /* 4KB 放静态区，别占任务栈 */
    if (!fetch(NETSTATS_URL, &resp) && !fetch(NETSTATS_URL2, &resp)) {
        s_ok = false;
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON 解析失败");
        s_ok = false;
        return ESP_FAIL;
    }

    int users = 0;
    int buckets[BUCKETS] = { 0 };
    bool good = false;

    cJSON *ju = cJSON_GetObjectItem(root, "users");
    cJSON *jb = cJSON_GetObjectItem(root, "buckets");
    if (cJSON_IsNumber(ju) && cJSON_IsArray(jb) &&
        cJSON_GetArraySize(jb) == BUCKETS) {
        users = ju->valueint;
        for (int i = 0; i < BUCKETS; i++) {
            cJSON *it = cJSON_GetArrayItem(jb, i);
            buckets[i] = cJSON_IsNumber(it) ? it->valueint : 0;
        }
        good = users > 0;
    }
    cJSON_Delete(root);

    if (!good) {
        ESP_LOGW(TAG, "数据不完整（users=%d）", users);
        s_ok = false;
        return ESP_FAIL;
    }

    memcpy(s_buckets, buckets, sizeof(s_buckets));
    s_users = users;
    s_ok = true;
    ESP_LOGI(TAG, "联网统计：%d 人上报", s_users);
    return ESP_OK;
}

bool netstats_ok(void)
{
    return s_ok && s_users > 0;
}

int netstats_users(void)
{
    return s_ok ? s_users : 0;
}

int netstats_percentile(int score)
{
    if (!netstats_ok()) {
        return -1;
    }
    if (score < 1) {
        score = 1;
    }
    if (score > BUCKETS) {
        score = BUCKETS;
    }
    /* 分数 <= 我 的人数 */
    long lower = 0;
    for (int i = 0; i < score; i++) {
        lower += s_buckets[i];
    }
    int pct = (int) (lower * 100 / s_users);
    if (pct < 1) {
        pct = 1;
    }
    if (pct > 99) {
        pct = 99;
    }
    return pct;
}
