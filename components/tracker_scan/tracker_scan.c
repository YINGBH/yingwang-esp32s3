#include "tracker_scan.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "trk_scan";

#define MAX_ENTRIES 64

typedef struct {
    uint8_t  addr[6];
    uint8_t  vendor;
    uint8_t  conf;
    uint8_t  flags;
    uint8_t  cls;
    int8_t   rssi_last;
    uint16_t seen;
    char     name[17];
    int64_t  first_us;
    int64_t  last_us;
    bool     used;
} slot_t;

static slot_t s_slots[MAX_ENTRIES];
static int s_count = 0;
static volatile bool s_busy = false;
static int64_t s_finished_us = 0;
static int64_t s_started_us = 0;
static uint32_t s_cost_ms = 0;
static uint32_t s_expect_ms = 0;

static trk_raw_t s_raw[TRK_RAW_MAX];
static int s_raw_n = 0;

/* ============================================================ 指纹匹配 */

static uint8_t vendor_of_adv(const uint8_t *ad, int n, uint8_t *conf,
                             uint8_t *flags, uint8_t *cls)
{
    *conf = 0;
    *flags = 0;
    *cls = TRK_CLS_OTHER;

    for (int i = 0; i + 1 < n;) {
        uint8_t len = ad[i];
        if (len == 0 || i + len > n) {
            break;
        }
        uint8_t type = ad[i + 1];
        const uint8_t *d = ad + i + 2;
        int dlen = len - 1;

        if (type == 0xFF && dlen >= 2) {              /* 厂商自定义数据 */
            uint16_t cid = (uint16_t) (d[0] | (d[1] << 8));
            if (cid == 0x004C && dlen >= 4) {         /* Apple */
                if (d[2] == 0x12 && dlen >= 3 + 27) { /* Find My 标签 */
                    *conf = 95;
                    *cls = TRK_CLS_TAG;
                    if (d[3] & 0x04) {
                        *flags |= 0x01;
                    }
                    return TRK_APPLE_OFB;
                }
                if (d[2] == 0x07) {                   /* AirPods */
                    *conf = 60;
                    *cls = TRK_CLS_AUDIO;
                    return TRK_APPLE_OFB;
                }
                if (d[2] == 0x10 || d[2] == 0x0F || d[2] == 0x05 ||
                    d[2] == 0x0C || d[2] == 0x0D) {   /* Handoff/Nearby/AirDrop */
                    *conf = 55;
                    *cls = TRK_CLS_PHONE;
                    return TRK_APPLE_OFB;
                }
            }
            if (cid == 0x0006 && dlen >= 3 && (d[2] & 0x03) == 0x03) {
                /* 微软 Swift Pair：只认子类型，不看载荷（加密随机字节会被当成名字） */
                *conf = 60;
                *cls = TRK_CLS_COMPUTER;
                return TRK_MICROSOFT;
            }
            if (cid == 0x027D) { *conf = 45; *cls = TRK_CLS_PHONE; return TRK_HUAWEI; }
            if (cid == 0x0075) { *conf = 50; *cls = TRK_CLS_PHONE; return TRK_SAMSUNG; }
            if (cid == 0x038F) { *conf = 35; *cls = TRK_CLS_PHONE; return TRK_XIAOMI; }
        }

        if (type == 0x16 && dlen >= 2) {              /* 16-bit Service Data */
            uint16_t uuid = (uint16_t) (d[0] | (d[1] << 8));
            switch (uuid) {
            case 0xFE2C:                              /* Google Fast Pair */
                *conf = 60; *cls = TRK_CLS_PHONE; return TRK_ANDROID;
            case 0xFEAA:                              /* Google FMDN */
                *conf = 90; *cls = TRK_CLS_TAG; return TRK_GOOGLE_FMDN;
            case 0xFCB2:                              /* DULT */
                *conf = 90; *cls = TRK_CLS_TAG; return TRK_DULT;
            case 0xFD5A: *conf = 85; *cls = TRK_CLS_TAG; return TRK_SAMSUNG;
            case 0xFEED:
            case 0xFEEC:
            case 0xFE84: *conf = 80; *cls = TRK_CLS_TAG; return TRK_TILE;
            case 0xFE95: *conf = 35; *cls = TRK_CLS_PHONE; return TRK_XIAOMI;
            default: break;
            }
        }
        i += len + 1;
    }
    return TRK_UNKNOWN;
}

/* ============================================================ 名字相关 */

/* 从广播里抠设备名（AD 0x08 简写 / 0x09 完整） */
static void adv_name(const uint8_t *ad, int n, char *out, size_t cap)
{
    out[0] = '\0';
    for (int i = 0; i + 1 < n;) {
        uint8_t len = ad[i];
        if (len == 0 || i + len > n) {
            break;
        }
        if ((ad[i + 1] == 0x08 || ad[i + 1] == 0x09) && len >= 2) {
            size_t k = (size_t) (len - 1);
            if (k > cap - 1) {
                k = cap - 1;
            }
            memcpy(out, ad + i + 2, k);
            out[k] = '\0';
            for (size_t j = 0; j < k; j++) {
                if (out[j] < 0x20 || out[j] == 0x7F) {
                    out[j] = '?';
                }
            }
            return;
        }
        i += len + 1;
    }
}

/* 是否像主机名：只认 LAPTOP- / DESKTOP- / PC- / WIN- 前缀 */
static bool looks_hostname(const char *s, int len)
{
    static const char *pfx[] = { "LAPTOP-", "DESKTOP-", "PC-", "WIN-" };
    for (size_t i = 0; i < sizeof(pfx) / sizeof(pfx[0]); i++) {
        size_t pl = strlen(pfx[i]);
        if ((size_t) len >= pl + 2 && strncasecmp(s, pfx[i], pl) == 0) {
            return true;
        }
    }
    return false;
}

/* 在整段广播里找主机名（有些 Windows 把名字塞在厂商数据里，不是 AD 0x09） */
static int adv_hostname(const uint8_t *ad, int n, char *out, size_t cap)
{
    out[0] = '\0';
    for (int i = 0; i + 1 < n;) {
        uint8_t len = ad[i];
        if (len == 0 || i + len > n) {
            break;
        }
        const uint8_t *d = ad + i + 2;
        int dl = len - 1;
        for (int k = 0; k + 7 < dl; k++) {
            if (d[k] < 0x20 || d[k] >= 0x7F) {
                continue;
            }
            int run = 0;
            while (k + run < dl && d[k + run] >= 0x20 && d[k + run] < 0x7F) {
                run++;
            }
            if (run >= 7 && looks_hostname((const char *) (d + k), run)) {
                size_t c = (size_t) run;
                if (c > cap - 1) {
                    c = cap - 1;
                }
                memcpy(out, d + k, c);
                out[c] = '\0';
                return 1;
            }
            k += run;
        }
        i += len + 1;
    }
    return 0;
}

/* 设备名里带品牌 → 具体厂商（这些品牌没有可靠的厂商 ID，只能靠名字） */
static uint8_t name_to_vendor(const char *s)
{
    static const struct { const char *kw; uint8_t v; } M[] = {
        { "OPPO",    TRK_OPPO },
        { "vivo",    TRK_VIVO },
        { "iQOO",    TRK_VIVO },
        { "HONOR",   TRK_HONOR },
        { "realme",  TRK_REALME },
        { "OnePlus", TRK_ONEPLUS },
        { "Meizu",   TRK_MEIZU },
        { "Redmi",   TRK_XIAOMI },
        { "POCO",    TRK_XIAOMI },
        { "HUAWEI",  TRK_HUAWEI },
        { "nova",    TRK_HUAWEI },
        { "Galaxy",  TRK_SAMSUNG },
        { "Pixel",   TRK_GOOGLE_FMDN },
        { "iPhone",  TRK_APPLE_OFB },
        { "iPad",    TRK_APPLE_OFB },
        { "MacBook", TRK_APPLE_OFB },
    };
    for (size_t i = 0; i < sizeof(M) / sizeof(M[0]); i++) {
        if (strcasestr(s, M[i].kw) != NULL) {
            return M[i].v;
        }
    }
    return TRK_UNKNOWN;
}

static bool name_looks_phone(const char *s)
{
    static const char *kw[] = { "iPhone", "iPad", "Android", "Redmi", "HUAWEI",
                                "OPPO", "vivo", "iQOO", "Galaxy", "Pixel", "HONOR",
                                "Honor", "OnePlus", "realme", "Meizu", "POCO",
                                "nova", "手机" };
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
        if (strcasestr(s, kw[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static bool name_looks_computer(const char *s)
{
    static const char *kw[] = { "DESKTOP-", "LAPTOP-", "MacBook", "ThinkPad",
                                "Notebook", "Windows", "PC-", "iMac", "Mac mini",
                                "Surface" };
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
        if (strcasestr(s, kw[i]) != NULL) {
            return true;
        }
    }
    return false;
}

/* 用名字 + 苹果子类型修正类型 */
static uint8_t refine_class(uint8_t vendor, uint8_t cls, const uint8_t *ad, int n,
                            const char *name)
{
    if (name != NULL && name[0] != '\0') {
        if (name_looks_computer(name)) {
            return TRK_CLS_COMPUTER;
        }
        if (name_looks_phone(name)) {
            return TRK_CLS_PHONE;
        }
    }
    /* 苹果子类型：热点=手机；AirDrop/AirPlay=偏电脑 */
    if (vendor == TRK_APPLE_OFB) {
        for (int i = 0; i + 1 < n;) {
            uint8_t len = ad[i];
            if (len == 0 || i + len > n) {
                break;
            }
            if (ad[i + 1] == 0xFF && len >= 5 && ad[i + 2] == 0x4C && ad[i + 3] == 0x00) {
                uint8_t sub = ad[i + 4];
                if (sub == 0x0D || sub == 0x0E) {
                    return TRK_CLS_PHONE;
                }
                if (sub == 0x05 || sub == 0x09 || sub == 0x0A) {
                    return TRK_CLS_COMPUTER;
                }
            }
            i += len + 1;
        }
    }
    return cls;
}

/* 统一加工：名字定品牌 + 名字/子类型定类型。feed 和 classify 都走这里 */
static void enrich(const uint8_t *ad, int len, uint8_t *vendor, uint8_t *conf,
                   uint8_t *cls, char *name, size_t name_cap)
{
    char nm[17];
    adv_name(ad, len, nm, sizeof(nm));
    if (nm[0] == '\0') {
        adv_hostname(ad, len, nm, sizeof(nm));
    }

    if (nm[0] != '\0') {
        if (looks_hostname(nm, (int) strlen(nm))) {
            *cls = TRK_CLS_COMPUTER;
            if (*vendor == TRK_UNKNOWN) {
                *vendor = TRK_MICROSOFT;   /* LAPTOP-/DESKTOP- 主机名基本就是 Windows */
            }
            if (*conf < 50) {
                *conf = 50;     /* 主机名是设备自报的 */
            }
        }
        uint8_t nv = name_to_vendor(nm);
        if (nv != TRK_UNKNOWN) {
            *vendor = nv;
            if (*cls == TRK_CLS_OTHER) {
                *cls = TRK_CLS_PHONE;
            }
            if (*conf < 50) {
                *conf = 50;
            }
        }
    }
    *cls = refine_class(*vendor, *cls, ad, len, nm);

    if (name != NULL && name_cap > 0) {
        snprintf(name, name_cap, "%s", nm);
    }
}

/* ============================================================ 对外接口 */

void tracker_scan_expect(uint32_t ms)
{
    s_expect_ms = ms;
}

uint32_t tracker_scan_expect_ms(void)
{
    return s_expect_ms;
}

void tracker_scan_begin(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    memset(s_raw, 0, sizeof(s_raw));
    s_raw_n = 0;
    s_count = 0;
    s_busy = true;
    s_started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "开始收集");
}

void tracker_scan_feed(const uint8_t *addr, int8_t rssi, const uint8_t *ad, int len)
{
    if (addr == NULL || ad == NULL || len <= 0) {
        return;
    }
    uint8_t conf = 0, flags = 0, cls = TRK_CLS_OTHER;
    uint8_t v = vendor_of_adv(ad, len, &conf, &flags, &cls);

    char nm[17];
    enrich(ad, len, &v, &conf, &cls, nm, sizeof(nm));

    /* 认不出来的：留原始载荷 */
    if (v == TRK_UNKNOWN && cls == TRK_CLS_OTHER) {
        for (int i = 0; i < s_raw_n; i++) {
            if (memcmp(s_raw[i].addr, addr, 6) == 0) {
                s_raw[i].rssi = rssi;
                s_raw[i].seen++;
                return;
            }
        }
        if (s_raw_n < TRK_RAW_MAX) {
            trk_raw_t *r = &s_raw[s_raw_n++];
            memcpy(r->addr, addr, 6);
            r->rssi = rssi;
            r->seen = 1;
            r->len = (uint8_t) (len > (int) sizeof(r->data) ? sizeof(r->data) : len);
            memcpy(r->data, ad, r->len);
        }
        return;
    }

    slot_t *s = NULL;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_slots[i].used && memcmp(s_slots[i].addr, addr, 6) == 0) {
            s = &s_slots[i];
            break;
        }
    }
    if (s == NULL) {
        for (int i = 0; i < MAX_ENTRIES; i++) {
            if (!s_slots[i].used) {
                s = &s_slots[i];
                s->used = true;
                memcpy(s->addr, addr, 6);
                s->first_us = esp_timer_get_time();
                s_count++;
                break;
            }
        }
    }
    if (s == NULL) {
        return;
    }
    s->vendor = v;
    s->conf = conf;
    s->flags = flags;
    s->cls = cls;
    if (nm[0] != '\0') {
        memcpy(s->name, nm, sizeof(s->name));
    }
    s->rssi_last = rssi;
    s->seen++;
    s->last_us = esp_timer_get_time();
}

void tracker_scan_finish(uint32_t cost_ms)
{
    s_busy = false;
    s_cost_ms = cost_ms;
    s_finished_us = esp_timer_get_time();
    ESP_LOGI(TAG, "结束：%d 台设备，用时 %u ms", s_count, (unsigned) cost_ms);
}

bool tracker_scan_busy(void)
{
    return s_busy;
}

int tracker_scan_count(void)
{
    return s_count;
}

uint32_t tracker_scan_last_cost_ms(void)
{
    return s_cost_ms;
}

uint32_t tracker_scan_elapsed_ms(void)
{
    if (!s_busy || s_started_us == 0) {
        return 0;
    }
    return (uint32_t) ((esp_timer_get_time() - s_started_us) / 1000);
}

uint32_t tracker_scan_last_age_ms(void)
{
    if (s_finished_us == 0) {
        return 0;
    }
    return (uint32_t) ((esp_timer_get_time() - s_finished_us) / 1000);
}

int tracker_scan_vendor_count(trk_vendor_t v)
{
    int n = 0;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_slots[i].used && s_slots[i].vendor == v) {
            n++;
        }
    }
    return n;
}

int tracker_scan_class_count(trk_class_t c)
{
    int n = 0;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_slots[i].used && s_slots[i].cls == (uint8_t) c) {
            n++;
        }
    }
    return n;
}

int tracker_scan_top(trk_entry_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    bool taken[MAX_ENTRIES] = { false };
    int n = 0;
    while (n < max) {
        int best = -1;
        for (int i = 0; i < MAX_ENTRIES; i++) {
            if (!s_slots[i].used || taken[i]) {
                continue;
            }
            if (best < 0 || s_slots[i].seen > s_slots[best].seen ||
                (s_slots[i].seen == s_slots[best].seen &&
                 s_slots[i].rssi_last > s_slots[best].rssi_last)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        taken[best] = true;
        memcpy(out[n].addr, s_slots[best].addr, 6);
        out[n].vendor = s_slots[best].vendor;
        out[n].conf = s_slots[best].conf;
        out[n].flags = s_slots[best].flags;
        out[n].cls = s_slots[best].cls;
        out[n].rssi_last = s_slots[best].rssi_last;
        out[n].seen = s_slots[best].seen;
        out[n].dur_ms = (uint32_t) ((s_slots[best].last_us - s_slots[best].first_us) / 1000);
        memcpy(out[n].name, s_slots[best].name, sizeof(out[n].name));
        n++;
    }
    return n;
}

int tracker_scan_raw(trk_raw_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    int n = s_raw_n < max ? s_raw_n : max;
    memcpy(out, s_raw, sizeof(trk_raw_t) * (size_t) n);
    return n;
}

int tracker_scan_unknown_count(void)
{
    return s_raw_n;
}

void tracker_scan_clear(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    memset(s_raw, 0, sizeof(s_raw));
    s_raw_n = 0;
    s_count = 0;
    s_busy = false;
    s_cost_ms = 0;
    s_finished_us = 0;
}

int tracker_scan_classify(const uint8_t *ad, int len, int *out_cls, int *out_conf,
                          char *out_name, int name_cap)
{
    uint8_t conf = 0, flags = 0, cls = TRK_CLS_OTHER;
    uint8_t v = vendor_of_adv(ad, len, &conf, &flags, &cls);
    char nm[17];
    enrich(ad, len, &v, &conf, &cls, nm, sizeof(nm));
    if (out_cls) {
        *out_cls = cls;
    }
    if (out_conf) {
        *out_conf = conf;
    }
    if (out_name && name_cap > 0) {
        snprintf(out_name, (size_t) name_cap, "%s", nm);
    }
    return v;
}
