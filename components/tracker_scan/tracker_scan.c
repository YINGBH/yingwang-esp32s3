#include "tracker_scan.h"

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
    int8_t   rssi_last;
    uint16_t seen;
    uint8_t  cls;
    int64_t  first_us;
    int64_t  last_us;
    bool     used;
} slot_t;

static slot_t s_slots[MAX_ENTRIES];

/* 未识别设备的原始载荷 */
static trk_raw_t s_raw[TRK_RAW_MAX];
static int s_raw_n = 0;
static int s_count = 0;
static volatile bool s_busy = false;
static int64_t s_finished_us = 0;
static uint32_t s_cost_ms = 0;

/* ---------------- 指纹匹配 ---------------- */

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

        if (type == 0xFF && dlen >= 2) {              /* Manufacturer Specific */
            uint16_t cid = (uint16_t) (d[0] | (d[1] << 8));
            if (cid == 0x004C && dlen >= 4) {         /* Apple */
                if (d[2] == 0x12 && dlen >= 3 + 27) { /* Find My 标签 */
                    *conf = 95;
                    *cls = TRK_CLS_TAG;
                    if (d[3] & 0x04) {
                        *flags |= 0x01;               /* near_owner */
                    }
                    return TRK_APPLE_OFB;
                }
                if (d[2] == 0x07) {                   /* AirPods / 耳机 */
                    *conf = 60;
                    *cls = TRK_CLS_AUDIO;
                    return TRK_APPLE_OFB;
                }
                if (d[2] == 0x10 || d[2] == 0x0F || d[2] == 0x05 ||
                    d[2] == 0x0C || d[2] == 0x0D) {   /* Handoff/Nearby/AirDrop */
                    *conf = 55;
                    *cls = TRK_CLS_PHONE;             /* 手机或电脑，统一算手机类 */
                    return TRK_APPLE_OFB;
                }
            }
            if (cid == 0x0006 && dlen >= 3 && (d[2] & 0x03) == 0x03) {
                /* 微软 Swift Pair：电脑/鼠标/键盘配对广播 */
                *conf = 60;
                *cls = TRK_CLS_COMPUTER;
                return TRK_UNKNOWN;                   /* 厂商不认识，但类型有用 */
            }
            if (cid == 0x027D) { *conf = 45; *cls = TRK_CLS_PHONE; return TRK_HUAWEI; }
            if (cid == 0x0075) { *conf = 50; *cls = TRK_CLS_PHONE; return TRK_SAMSUNG; }
            if (cid == 0x038F) { *conf = 35; *cls = TRK_CLS_PHONE; return TRK_XIAOMI; }
        }

        if (type == 0x16 && dlen >= 2) {              /* Service Data 16-bit */
            uint16_t uuid = (uint16_t) (d[0] | (d[1] << 8));
            switch (uuid) {
            case 0xFE2C:                              /* Google Fast Pair（耳机） */
                *conf = 70; *cls = TRK_CLS_AUDIO; return TRK_GOOGLE_FMDN;
            case 0xFEAA:                              /* Google FMDN */
                *conf = 90; *cls = TRK_CLS_TAG;
                if (dlen >= 3 && d[2] == 0x41) {
                    *flags |= 0x02;                   /* separated */
                }
                return TRK_GOOGLE_FMDN;
            case 0xFCB2:                              /* DULT */
                *conf = 90; *cls = TRK_CLS_TAG;
                if (dlen >= 16 && (d[15] & 0x01)) {
                    *flags |= 0x01;
                }
                return TRK_DULT;
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

/* ---------------- 对外接口 ---------------- */

void tracker_scan_begin(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    memset(s_raw, 0, sizeof(s_raw));
    s_raw_n = 0;
    s_count = 0;
    s_busy = true;
    ESP_LOGI(TAG, "开始收集");
}

void tracker_scan_feed(const uint8_t *addr, int8_t rssi, const uint8_t *ad, int len)
{
    if (addr == NULL || ad == NULL || len <= 0) {
        return;
    }
    uint8_t conf = 0, flags = 0, cls = TRK_CLS_OTHER;
    uint8_t v = vendor_of_adv(ad, len, &conf, &flags, &cls);
    if (v == TRK_UNKNOWN && cls == TRK_CLS_OTHER) {
        /* 未知厂商：留原始载荷，方便事后再分析/加指纹 */
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
        n++;
    }
    return n;
}

void tracker_scan_clear(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_count = 0;
    s_busy = false;
    s_cost_ms = 0;
    s_finished_us = 0;
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
