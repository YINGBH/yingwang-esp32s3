/*
 * 日历在线补全
 *
 * 为什么需要：内置表是编译进去的，明年国务院发新通知后得重新烧录。
 * 这个模块让板子自己把新数据拉下来存进 NVS，存好之后照样离线可用。
 *
 * 策略（不是"每天联网"）：
 *   - 数据一年才变一次，但按用户要求每 7 天拉一次，拉到了就存着
 *   - 内置表没覆盖到当前年份时，强制拉一次
 *   - 拉不到就静默用内置表，日历照样能看
 *
 * 存储：每年一份，按年份合并（不是覆盖）。
 *   否则 12 月拉了 2027，翻年 1 月又把 2026 拉回来，来回打架。
 *
 * 格式：[magic u32][count u16][pool_len u16]
 *       + count*{date u32, flags u8, name_off u8} + 名字池（去重）
 */
#include "calendar_remote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "time_weather.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "cal_remote";

#define CAL_MAGIC     0x434F564CU     /* "COVL" */
#define CAL_MAX_BLOB  4096
#define CAL_MAX_ENTRY 128             /* 几年的数据一共，够用 */
#define CAL_NAME_LEN  24
#define CAL_POOL_MAX  512
#define CAL_MIN_GAP_S (7 * 24 * 3600)    /* 7 天拉一次 */
#define CAL_HTTP_MAX  16384

#define NVS_NS   "caldata"
#define NVS_BLOB "overlay"
#define NVS_TS_FMT "t%d"              /* t2026 = 那年最后更新时间 */

typedef struct {
    uint32_t date;
    uint8_t  flags;
    uint8_t  name_off;
} __attribute__((packed)) cal_entry_t;

typedef struct {
    uint32_t date;
    uint8_t  flags;
    char     name[CAL_NAME_LEN];
} work_t;

static uint8_t  s_blob[CAL_MAX_BLOB];
static size_t   s_blob_len = 0;
static int      s_count = 0;
static int      s_from_year = 0;
static int      s_to_year = 0;
static bool     s_valid = false;
static char     s_url[160] = "";
static const char *s_pool = NULL;     /* 指向 s_blob 里的名字池 */
static uint32_t s_last_ts = 0;        /* 最近一次任何年份的成功时间 */

/* ------------------------------------------------------------------ 工具 */

static uint16_t rd16(const uint8_t *p) { return (uint16_t) (p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static int cmp_u32(const void *a, const void *b)
{
    const work_t *x = a, *y = b;
    return (x->date < y->date) ? -1 : (x->date > y->date) ? 1 : 0;
}

/* 解析当前 s_blob */
static void parse_blob(void)
{
    s_valid = false;
    s_count = 0;
    s_from_year = s_to_year = 0;
    s_pool = NULL;

    if (s_blob_len < 8) return;
    if (rd32(s_blob) != CAL_MAGIC) return;

    int count = rd16(s_blob + 4);
    int pool_len = rd16(s_blob + 6);
    if (count <= 0 || count > CAL_MAX_ENTRY) return;

    size_t need = 8 + (size_t) count * sizeof(cal_entry_t) + (size_t) pool_len;
    if (need > s_blob_len) return;

    s_count = count;
    s_pool = (const char *) (s_blob + 8 + (size_t) count * sizeof(cal_entry_t));
    s_from_year = (int) (rd32(s_blob + 8) / 10000);
    s_to_year = (int) (rd32(s_blob + 8 + (size_t) (count - 1) * sizeof(cal_entry_t)) / 10000);
    s_valid = true;
}

/* 把 blob 里的条目摊平到 work 数组（名字拷贝出来） */
static int flatten(work_t *out, int max)
{
    if (!s_valid || s_pool == NULL) return 0;

    int n = 0;
    for (int i = 0; i < s_count && n < max; i++) {
        const cal_entry_t *e =
            (const cal_entry_t *) (s_blob + 8 + (size_t) i * sizeof(cal_entry_t));
        out[n].date = e->date;
        out[n].flags = e->flags;
        strlcpy(out[n].name, s_pool + e->name_off, CAL_NAME_LEN);
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------- 对外 */

esp_err_t cal_remote_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "还没有在线数据，用内置表");
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    size_t len = sizeof(s_blob);
    if (nvs_get_blob(h, NVS_BLOB, s_blob, &len) == ESP_OK) {
        s_blob_len = len;
        parse_blob();
    }

    /* 取各年份里最新的那个时间戳 */
    for (int y = 2024; y <= 2035; y++) {
        char key[8];
        snprintf(key, sizeof(key), NVS_TS_FMT, y);
        uint32_t ts = 0;
        if (nvs_get_u32(h, key, &ts) == ESP_OK && ts > s_last_ts) {
            s_last_ts = ts;
        }
    }
    nvs_close(h);

    if (s_valid) {
        ESP_LOGI(TAG, "在线日历: %d 条, %d-%d 年", s_count, s_from_year, s_to_year);
    }
    return ESP_OK;
}

void cal_remote_set_url(const char *tmpl)
{
    if (tmpl == NULL) return;
    strlcpy(s_url, tmpl, sizeof(s_url));
}

bool cal_remote_covers(int year)
{
    if (!s_valid) return false;

    /* 该年至少有一条放假记录才算"覆盖" */
    for (int i = 0; i < s_count; i++) {
        const cal_entry_t *e =
            (const cal_entry_t *) (s_blob + 8 + (size_t) i * sizeof(cal_entry_t));
        if ((int) (e->date / 10000) != year) continue;
        if (e->flags & 1) return true;
    }
    return false;
}

uint32_t cal_remote_last_ts(void)
{
    return s_last_ts;
}

void cal_remote_range(int *from_year, int *to_year, int *count)
{
    if (from_year) *from_year = s_from_year;
    if (to_year) *to_year = s_to_year;
    if (count) *count = s_count;
}

const char *cal_remote_lookup(uint32_t date, uint8_t *flags)
{
    if (!s_valid || s_pool == NULL) return NULL;

    int lo = 0, hi = s_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const cal_entry_t *e =
            (const cal_entry_t *) (s_blob + 8 + (size_t) mid * sizeof(cal_entry_t));
        if (e->date == date) {
            if (flags) *flags = e->flags;
            return s_pool + e->name_off;
        }
        if (e->date < date) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* ------------------------------------------------------------------ 联网 */

static uint32_t parse_date_str(const char *s)
{
    if (s == NULL) return 0;
    int y = 0, m = 0, d = 0;
    if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) return 0;
    if (y < 2000 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
    return (uint32_t) (y * 10000 + m * 100 + d);
}

static esp_err_t http_get(const char *url, char **out, int *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
            .keep_alive_enable = true,
        .keep_alive_idle = 10,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) return ESP_FAIL;

    /* TLS 握手发生在 open 里，只锁这一段（没有中途 return，天然不会漏解锁） */
    time_weather_tls_lock();
    esp_err_t err = esp_http_client_open(cli, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(cli);
    }
    time_weather_tls_unlock();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "连接失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return err;
    }

    int status = esp_http_client_get_status_code(cli);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d", status);
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
    return ESP_FAIL;
    }

    char *buf = malloc(CAL_HTTP_MAX);
    if (buf == NULL) {
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    while (total < CAL_HTTP_MAX - 1) {
        int n = esp_http_client_read(cli, buf + total, CAL_HTTP_MAX - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';

    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (total == 0) {
        free(buf);
    return ESP_FAIL;
    }

    *out = buf;
    *out_len = total;
    return ESP_OK;
}

/* 记录某年的"最近一次拉取时间"（解析到 0 条时也要记） */
static void year_ts_save(int year, uint32_t ts)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char key[16];
    snprintf(key, sizeof(key), NVS_TS_FMT, year);
    nvs_set_u32(h, key, ts);
    nvs_commit(h);
    nvs_close(h);
}

static uint32_t year_ts(int year)
{
    nvs_handle_t h;
    uint32_t ts = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        char key[8];
        snprintf(key, sizeof(key), NVS_TS_FMT, year);
        nvs_get_u32(h, key, &ts);
        nvs_close(h);
    }
    return ts;
}

esp_err_t cal_remote_fetch(int year, bool force)
{
    if (s_url[0] == '\0') return ESP_ERR_INVALID_STATE;

    /* 同样要开 TLS，内存紧的时候先不拉，别和蓝牙抢 */
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_int < 60000) {
        ESP_LOGW(TAG, "内存紧张（%uK），这次先不更新日历", (unsigned) (free_int / 1024));
        return ESP_ERR_NO_MEM;
    }

    /* 这一年已经有数据、而且 7 天内更新过，就不折腾 */
    if (!force && cal_remote_covers(year)) {
        uint32_t now = (uint32_t) time(NULL);
        uint32_t ts = year_ts(year);
        if (now > 1600000000 && ts > 1600000000 && now - ts < CAL_MIN_GAP_S) {
            ESP_LOGI(TAG, "%d 年 7 天内已更新过，跳过", year);
            return ESP_OK;
        }
    }

    char url[192];
    strlcpy(url, s_url, sizeof(url));
    char *pos = strstr(url, "{year}");
    if (pos != NULL) {
        char tail[80];
        strlcpy(tail, pos + 6, sizeof(tail));
        snprintf(pos, sizeof(url) - (size_t) (pos - url), "%d%s", year, tail);
    }

    ESP_LOGI(TAG, "拉取 %s", url);

    char *body = NULL;
    int len = 0;
    esp_err_t err = http_get(url, &body, &len);
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON 解析失败");
        return ESP_FAIL;
    }

    cJSON *days = cJSON_GetObjectItem(root, "days");
    if (!cJSON_IsArray(days)) {
        ESP_LOGW(TAG, "格式不对（没有 days 数组）");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* work 数组：先放旧数据，再用新数据替换同一年的部分 */
    work_t *work = calloc(CAL_MAX_ENTRY, sizeof(work_t));
    if (work == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    int n = flatten(work, CAL_MAX_ENTRY);

    /* 丢掉这一年的旧条目 */
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if ((int) (work[i].date / 10000) == year) continue;
        work[kept++] = work[i];
    }
    n = kept;

    int added = 0;
    int total = cJSON_GetArraySize(days);
    for (int i = 0; i < total && n < CAL_MAX_ENTRY; i++) {
        cJSON *it = cJSON_GetArrayItem(days, i);
        cJSON *jd = cJSON_GetObjectItem(it, "date");
        cJSON *jn = cJSON_GetObjectItem(it, "name");
        cJSON *jo = cJSON_GetObjectItem(it, "isOffDay");
        if (!cJSON_IsString(jd) || !cJSON_IsString(jn)) continue;

        uint32_t date = parse_date_str(jd->valuestring);
        if (date == 0 || (int) (date / 10000) != year) continue;

        work[n].date = date;
        work[n].flags = cJSON_IsTrue(jo) ? 1 : 2;
        strlcpy(work[n].name, jn->valuestring, CAL_NAME_LEN);
        n++;
        added++;
    }

    cJSON_Delete(root);

    if (added == 0) {
        /* 数据源里这一年还是空的（国务院还没公布）——
           记下尝试时间，7 天内不再重复拉，避免反复空请求。 */
        ESP_LOGI(TAG, "%d 年放假安排还没公布，7 天内不再尝试", year);
        year_ts_save(year, (uint32_t) time(NULL));
        free(work);
        return ESP_FAIL;
    }

    qsort(work, (size_t) n, sizeof(work_t), cmp_u32);

    /* 打包：名字去重，否则同一年的"国庆节"会重复存 7 次，池子直接爆 */
    uint8_t *blob = s_blob;
    wr32(blob, CAL_MAGIC);

    cal_entry_t *es = (cal_entry_t *) (blob + 8);
    char *pool = (char *) (blob + 8 + (size_t) n * sizeof(cal_entry_t));
    int pool_len = 0;

    for (int i = 0; i < n; i++) {
        int off = -1;

        /* 在已有池子里找同名 */
        for (int j = 0; j < i; j++) {
            if (strcmp(work[j].name, work[i].name) == 0) {
                off = es[j].name_off;
                break;
            }
        }

        if (off < 0) {
            size_t nlen = strlen(work[i].name);
            if (pool_len + (int) nlen + 1 > CAL_POOL_MAX) {
                ESP_LOGW(TAG, "名字池满了，后面的条目丢掉");
                n = i;
                break;
            }
            off = pool_len;
            memcpy(pool + pool_len, work[i].name, nlen);
            pool[pool_len + nlen] = '\0';
            pool_len += (int) nlen + 1;
        }

        es[i].date = work[i].date;
        es[i].flags = work[i].flags;
        es[i].name_off = (uint8_t) off;
    }

    free(work);

    if (n <= 0) return ESP_FAIL;

    wr16(blob + 4, (uint16_t) n);
    wr16(blob + 6, (uint16_t) pool_len);
    s_blob_len = 8 + (size_t) n * sizeof(cal_entry_t) + (size_t) pool_len;

    if (s_blob_len > CAL_MAX_BLOB) {
        ESP_LOGE(TAG, "数据太大（%d 字节）", (int) s_blob_len);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 存 NVS */
    nvs_handle_t h;
    err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打不开 NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(h, NVS_BLOB, s_blob, s_blob_len);
    if (err == ESP_OK) {
        char key[8];
        snprintf(key, sizeof(key), NVS_TS_FMT, year);
        uint32_t now = (uint32_t) time(NULL);
        nvs_set_u32(h, key, now);
        nvs_commit(h);
        if (now > s_last_ts) s_last_ts = now;
        parse_blob();
        ESP_LOGI(TAG, "%d 年: 新增 %d 条，共 %d 条（%d 字节）",
                 year, added, s_count, (int) s_blob_len);
    }
    nvs_close(h);

    return err;
}