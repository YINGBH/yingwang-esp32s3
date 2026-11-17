#include "stats.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "stats";

#define STATS_DAYS   7
#define NVS_NS       "stats"
#define NVS_KEY      "days"

static stats_day_t s_days[STATS_DAYS];
static bool s_loaded = false;
/* 时间还没对时（today_key()==0）时，事件先挂在这里，
   等系统时间有效后第一次调用时补记进去。
   不这么做的话，DNS 坏掉时 NTP 拿不到时间，
   一天的登录成功全被静默丢掉（实测统计一直是 0）。*/
static uint16_t s_pending[3] = { 0, 0, 0 };
static esp_timer_handle_t s_flush_tmo = NULL;   /* 改完 5 秒再落盘，避免频繁写 NVS */

/* 今天的日期（YYYYMMDD）。时间没对上也给个 0，那种情况下不记 */
static uint32_t today_key(void)
{
    time_t now = time(NULL);
    if (now < 1600000000) {          /* 2020 年之前说明还没对时 */
        return 0;
    }
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    return (uint32_t) ((tm_now.tm_year + 1900) * 10000
                       + (tm_now.tm_mon + 1) * 100
                       + tm_now.tm_mday);
}

static void save(void);

static void flush_cb(void *arg)
{
    (void) arg;
    save();
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, NVS_KEY, s_days, sizeof(s_days));
    nvs_commit(h);
    nvs_close(h);
}

void stats_init(void)
{
    if (s_flush_tmo == NULL) {
        const esp_timer_create_args_t args = {
            .callback = flush_cb,
            .name = "stats_flush",
        };
        esp_timer_create(&args, &s_flush_tmo);
    }
    memset(s_days, 0, sizeof(s_days));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_days);
        if (nvs_get_blob(h, NVS_KEY, s_days, &len) != ESP_OK || len != sizeof(s_days)) {
            memset(s_days, 0, sizeof(s_days));
        }
        nvs_close(h);
    }
    s_loaded = true;

    uint16_t ok, fail, drop;
    stats_today(&ok, &fail, &drop);
    ESP_LOGI(TAG, "今日认证：成功 %u 失败 %u 掉线 %u", ok, fail, drop);
}

/* 找今天那条；没有就占一个空槽，满了就覆盖最旧的一条 */
static stats_day_t *slot_for_today(void)
{
    uint32_t today = today_key();
    if (today == 0) {
        return NULL;
    }
    int oldest = 0;
    for (int i = 0; i < STATS_DAYS; i++) {
        if (s_days[i].day == today) {
            return &s_days[i];
        }
        if (s_days[i].day < s_days[oldest].day) {
            oldest = i;
        }
    }
    memset(&s_days[oldest], 0, sizeof(s_days[oldest]));
    s_days[oldest].day = today;
    return &s_days[oldest];
}

static void bump(int which)
{
    if (!s_loaded) {
        return;
    }
    stats_day_t *d = slot_for_today();
    if (d == NULL) {
        /* 时间无效：先挂起，等对时后补记 */
        if (s_pending[which] < 0xFFFE) {
            s_pending[which]++;
        }
        return;
    }
    /* 对时成功：把之前挂起的一次性补上 */
    for (int k = 0; k < 3; k++) {
        if (s_pending[k] == 0) {
            continue;
        }
        uint16_t *q = (k == 0) ? &d->ok : (k == 1 ? &d->fail : &d->drop);
        uint32_t sum = (uint32_t) *q + s_pending[k];
        *q = (sum > 0xFFFF) ? 0xFFFF : (uint16_t) sum;
        s_pending[k] = 0;
    }
    uint16_t *p = (which == 0) ? &d->ok : (which == 1 ? &d->fail : &d->drop);
    if (*p < 0xFFFF) {
        (*p)++;
    }
    /* NVS 有写入寿命：改完等 5 秒再写，连续变化只写一次；断电最多丢 5 秒 */
    if (s_flush_tmo) {
        esp_timer_stop(s_flush_tmo);
        esp_timer_start_once(s_flush_tmo, 5LL * 1000 * 1000);
    } else {
        save();
    }
}

void stats_note_ok(void)   { bump(0); }
void stats_note_fail(void) { bump(1); }
void stats_note_drop(void) { bump(2); }

void stats_today(uint16_t *ok, uint16_t *fail, uint16_t *drop)
{
    uint32_t today = today_key();
    stats_day_t *d = NULL;
    for (int i = 0; i < STATS_DAYS; i++) {
        if (today != 0 && s_days[i].day == today) {
            d = &s_days[i];
            break;
        }
    }
    if (ok)   *ok = d ? d->ok : 0;
    if (fail) *fail = d ? d->fail : 0;
    if (drop) *drop = d ? d->drop : 0;
}

void stats_week(uint32_t *ok, uint32_t *fail, uint32_t *drop)
{
    uint32_t o = 0, f = 0, dr = 0;
    for (int i = 0; i < STATS_DAYS; i++) {
        o += s_days[i].ok;
        f += s_days[i].fail;
        dr += s_days[i].drop;
    }
    if (ok)   *ok = o;
    if (fail) *fail = f;
    if (drop) *drop = dr;
}

/* 导出 7 天记录：按 day 升序，跳过空槽 */
int stats_history(stats_day_t *out, int max)
{
    if (!s_loaded) {
        stats_init();
    }
    if (out == NULL || max <= 0) {
        return 0;
    }
    int n = 0;
    /* 简单插入排序：7 条数据，不值得上 qsort */
    for (int i = 0; i < STATS_DAYS; i++) {
        if (s_days[i].day == 0) {
            continue;
        }
        int j = n - 1;
        while (j >= 0 && out[j].day > s_days[i].day) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = s_days[i];
        if (n < max) {
            n++;
        }
    }
    return n;
}
