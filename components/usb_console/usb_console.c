#include "usb_console.h"
#include "netstats.h"
#include "tracker_scan.h"
void ui_replay_boot_anim(int speed);
void ui_boot_hint(const char *text);
void ui_unfreeze(void);
#include "time_weather.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include "esp_timer.h"
#include "driver/usb_serial_jtag_vfs.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state.h"
#include "beep.h"
#include "logring.h"
#include "audio_level.h"
#include "flog.h"

/* main.c 里定义的（闹钟 / 扩展设置） */
void alarm_set(int idx, int hour, int min, int mask, bool en);
bool app_alarm_get(int idx, int *hour, int *min, int *mask, bool *en);
void ext_set_tick(bool on);
void app_set_mac(const char *macstr);
void app_force_auth(void);
void app_reset_esl_push(void);
void app_wifi_night(bool on);
void app_set_rotation(int deg);
void bsp_display_set_gap_adj(int x, int y);
void app_set_portal_auth(bool on);
#include "esl_ble.h"
#include "hitokoto.h"

/* 截图期间把日志"吞掉"：不临时改全局日志级别（那会覆盖别的模块单独调过的级别，
   恢复时也只能恢复成 INFO），而是换掉 vprintf 钩子，结束后原样换回来。
   logring / flog 的钩子会被这层包住，换回后照样继续收日志。 */
static int shot_log_sink(const char *fmt, va_list ap)
{
    (void) fmt;
    (void) ap;
    return 0;
}


void esl_ble_draw_test_pattern(void);
#include "screen_bg.h"
#include "captcha_loader.h"
#include "portal_client.h"
#include "stats.h"
#include "esp_app_desc.h"
#include "bsp_display.h"
#include "bsp_display.h"
#include "config_store.h"
#include "wifi_sta.h"

static const char *TAG = "usbcli";

/* SHOT/KEY 是产品功能（网页看屏/远程按键）；PAGE/CAPIMG 只是开发调试用 */
#define USB_DEBUG_CMDS 1

static void reply(const char *json)
{
    printf("@%s\n", json);
    fflush(stdout);
}

static void reply_ok(void) { reply("{\"ok\":true}"); }

static void reply_err(const char *msg)
{
    printf("@{\"ok\":false,\"err\":\"%s\"}\n", msg);
    fflush(stdout);
}

/* ---- 背景图上传（网页把 240x240 转成 RGB565，按 128 字节一块 base64 传过来） ---- */
static size_t s_bg_off = 0;

/* 截图：忙碌锁 + 30 秒兜底解冻（万一对端拔线导致 fwrite 卡住） */
static volatile bool s_shot_busy = false;
static esp_timer_handle_t s_shot_guard = NULL;

static void shot_guard_cb(void *arg)
{
    (void) arg;
    ui_freeze(false);
    ESP_LOGW(TAG, "截图超时，界面解冻");
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char *in, uint8_t *out, size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (const char *p = in; *p != '\0'; p++) {
        int v = b64_val(*p);
        if (v < 0) {
            if (*p == '=') break;
            continue;
        }
        acc = (acc << 6) | (uint32_t) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n < cap) {
                out[n++] = (uint8_t) (acc >> bits);
            }
        }
    }
    return n;
}

static void send_status(void)
{
    app_settings_t cfg;
    config_store_snapshot(&cfg);

    bool conn = wifi_sta_connected();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        reply_err("oom");
        return;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(root, "version", app ? app->version : "?");
    cJSON_AddBoolToObject(root, "release", true);
    cJSON_AddStringToObject(root, "author", "YING");
    cJSON_AddStringToObject(root, "state", app_state_name(app_state_get(NULL, 0)));
    cJSON_AddStringToObject(root, "ssid", conn ? wifi_sta_ssid() : "");
    cJSON_AddStringToObject(root, "ip", conn ? wifi_sta_ip() : "");
    cJSON_AddNumberToObject(root, "rssi", conn ? wifi_sta_rssi() : 0);
    cJSON_AddNumberToObject(root, "heap", (double) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "psram", (double) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "uptime", (double) app_state_uptime_s());
    cJSON_AddNumberToObject(root, "alarm_vol", beep_volume());

    {
        char bd[8];
        snprintf(bd, sizeof(bd), "%02d-%02d",
                 (cfg.birthday / 100) % 100, cfg.birthday % 100);
        cJSON_AddStringToObject(root, "birthday", bd);
    }

    /* 闹钟：网页要显示当前设置 */
    {
        cJSON *arr = cJSON_AddArrayToObject(root, "alarms");
        for (int i = 1; i <= 4; i++) {
            int ah = 0, am = 0, amk = 0;
            bool aen = false;
            if (!app_alarm_get(i, &ah, &am, &amk, &aen)) {
                continue;
            }
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "id", i);
            cJSON_AddNumberToObject(o, "hour", ah);
            cJSON_AddNumberToObject(o, "min", am);
            cJSON_AddNumberToObject(o, "mask", amk);
            cJSON_AddBoolToObject(o, "en", aen);
            cJSON_AddItemToArray(arr, o);
        }
    }

    uint16_t sok = 0, sfail = 0, sdrop = 0;
    uint32_t wok = 0, wfail = 0, wdrop = 0;
    stats_today(&sok, &sfail, &sdrop);
    stats_week(&wok, &wfail, &wdrop);
    cJSON *st = cJSON_AddObjectToObject(root, "stats");
    cJSON_AddNumberToObject(st, "today_ok", sok);
    cJSON_AddNumberToObject(st, "today_fail", sfail);
    cJSON_AddNumberToObject(st, "today_drop", sdrop);
    cJSON_AddNumberToObject(st, "week_ok", (double) wok);
    cJSON_AddNumberToObject(st, "week_fail", (double) wfail);
    cJSON_AddNumberToObject(st, "week_drop", (double) wdrop);

    captcha_model_load();
    captcha_model_info_t mi;
    captcha_model_get_info(&mi);
    cJSON *mdl = cJSON_AddObjectToObject(root, "model");
    cJSON_AddBoolToObject(mdl, "ready", mi.ready);
    cJSON_AddNumberToObject(mdl, "params", (double) mi.params);
    cJSON_AddNumberToObject(mdl, "bytes", (double) mi.bytes);
    cJSON_AddNumberToObject(mdl, "crc", (double) mi.crc);

    cJSON *c = cJSON_AddObjectToObject(root, "cfg");
    cJSON_AddStringToObject(c, "ssid", cfg.wifi_ssid);
    cJSON_AddStringToObject(c, "user", cfg.user);
    cJSON_AddStringToObject(c, "city", cfg.city);
    cJSON_AddStringToObject(c, "static_ip", cfg.static_ip);
    cJSON_AddStringToObject(c, "lat", cfg.lat);
    cJSON_AddStringToObject(c, "lon", cfg.lon);
    cJSON_AddStringToObject(c, "cal_url", cfg.cal_url);
    cJSON_AddNumberToObject(c, "brightness", cfg.brightness);
    cJSON_AddNumberToObject(c, "screen_off", cfg.screen_off_min);
    /* 熄屏策略：插电是否也熄屏 / 熄屏后背光（0=全黑，>0=低亮度常显） */
    cJSON_AddNumberToObject(c, "charge_off", cfg.charge_off);
    cJSON_AddNumberToObject(c, "off_bright", cfg.off_bright);
    /* 主题：-1 = 自定义（用下面两个颜色），0..4 = 内置 */
    cJSON_AddNumberToObject(c, "theme", cfg.theme_id == 0xFF ? -1 : (int) cfg.theme_id);
    cJSON_AddNumberToObject(c, "theme_bg", cfg.theme_bg);
    cJSON_AddNumberToObject(c, "theme_accent", cfg.theme_accent);
    cJSON_AddNumberToObject(c, "deco_border", cfg.deco_border);
    cJSON_AddNumberToObject(c, "deco_icon", cfg.deco_icon);
    cJSON_AddStringToObject(c, "deco_tag", cfg.deco_tag);
    cJSON_AddNumberToObject(c, "theme_auto", cfg.theme_auto);
    cJSON_AddNumberToObject(c, "theme_day", cfg.theme_day);
    cJSON_AddNumberToObject(c, "theme_night", cfg.theme_night);
    cJSON_AddNumberToObject(c, "bg_rotate", cfg.bg_rotate);
    cJSON_AddNumberToObject(c, "bg_slot", bg_slot());
    cJSON_AddNumberToObject(c, "probe_sec", cfg.probe_sec);
    cJSON_AddNumberToObject(c, "probe_plugged", cfg.probe_plugged);
    cJSON_AddNumberToObject(c, "batt_low", cfg.batt_low_min);
    cJSON_AddNumberToObject(c, "batt_keep", cfg.batt_low_keepon);
    cJSON_AddNumberToObject(c, "batt_used", cfg.batt_used_min);
    cJSON_AddNumberToObject(c, "batt_est", cfg.batt_est_hours);
    cJSON_AddNumberToObject(c, "esl_layout", cfg.esl_layout);
    cJSON_AddNumberToObject(c, "esl_opts", cfg.esl_opts ? cfg.esl_opts : 0x0F);
    cJSON_AddStringToObject(c, "esl_text", cfg.esl_text);
    /* 站点参数：门户地址 / AC IP / 校园网 SSID，网页后台用这三项回填输入框 */
    cJSON_AddStringToObject(c, "portal_base", cfg.portal_base);
    cJSON_AddStringToObject(c, "portal_ac", cfg.portal_ac_ip);
    cJSON_AddStringToObject(c, "portal_ssid", cfg.portal_ssid);

    char *out = cJSON_PrintUnformatted(root);
    if (out != NULL) {
        reply(out);
        cJSON_free(out);
    } else {
        reply_err("oom");
    }
    cJSON_Delete(root);
}

static void do_set(char *kv)
{
    char *eq = strchr(kv, '=');
    if (eq == NULL) {
        reply_err("bad");
        return;
    }
    *eq = '\0';
    char *key = kv;
    char *val = eq + 1;

    while (*val == ' ') {
        val++;
    }
    char *end = val + strlen(val);
    while (end > val && (end[-1] == ' ' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }

    app_settings_t cfg;
    config_store_snapshot(&cfg);

    bool wifi_changed = false;
    bool theme_changed = false;
    bool known = true;

    /* ---- 闹钟 / 按键音（main.c 里的扩展设置）---- */
    if (strncmp(key, "alarm", 5) == 0 && key[5] >= '1' && key[5] <= '4') {
        int idx = key[5] - '1';
        int hh = 0, mm = 0, mask = 0, en = 1;
        /* 分隔符两种都认：文档/网页用逗号（07:30,127,1），
           早期脚本用竖线（07:30|127|1）。原来只认竖线，
           网页传逗号时 mask/en 解析失败，看起来像"开关写不进去"。 */
        int n = sscanf(val, "%d:%d,%d,%d", &hh, &mm, &mask, &en);
        if (n < 2) {
            n = sscanf(val, "%d:%d|%d|%d", &hh, &mm, &mask, &en);
        }
        if (n < 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59) {
            reply_err("bad_alarm");
            return;
        }
        alarm_set(idx, hh, mm, mask, en != 0);
        reply_ok();
        return;
    } else if (strcmp(key, "portal") == 0) {
        app_set_portal_auth(val[0] != '0');
        reply_ok();
        return;
    } else if (strcmp(key, "tick") == 0) {
        ext_set_tick(val[0] != '0');
        reply_ok();
        return;
    }

    /* 含义明确的别名：
         wifi_pass    = WiFi 密码（等价 pass）
         portal_pass  = 门户/宽带密码（等价 pwd）
       旧的 pass/pwd 保留兼容——但这两个名字太像，确实把 WiFi 密码改坏过一次。 */
    if (strcmp(key, "ssid") == 0) {
        strlcpy(cfg.wifi_ssid, val, sizeof(cfg.wifi_ssid));
        wifi_changed = true;
    } else if (strcmp(key, "pass") == 0 || strcmp(key, "wifi_pass") == 0) {
        strlcpy(cfg.wifi_pass, val, sizeof(cfg.wifi_pass));
        wifi_changed = true;
    } else if (strcmp(key, "user") == 0) {
        strlcpy(cfg.user, val, sizeof(cfg.user));
    } else if (strcmp(key, "pwd") == 0 || strcmp(key, "portal_pass") == 0) {
        strlcpy(cfg.password, val, sizeof(cfg.password));
    } else if (strcmp(key, "birthday") == 0) {
        /* 支持 10-31 / 10/31 / 1031 三种写法 */
        int m = 0, d = 0;
        if (sscanf(val, "%d-%d", &m, &d) != 2 &&
            sscanf(val, "%d/%d", &m, &d) != 2) {
            int n = atoi(val);
            m = n / 100;
            d = n % 100;
        }
        if (m < 1 || m > 12 || d < 1 || d > 31) {
            reply_err("birthday");
            return;
        }
        cfg.birthday = (uint16_t) (m * 100 + d);
    } else if (strcmp(key, "mac") == 0) {
        app_set_mac(val);        /* 立即应用 + 存 NVS */
        wifi_changed = true;
    } else if (strcmp(key, "rotgap") == 0) {
        int gx = 0, gy = 0;
        if (sscanf(val, "%d,%d", &gx, &gy) != 2) {
            if (sscanf(val, "%d", &gx) != 1) {
                reply_err("rotgap 用法: SET rotgap=x,y");
                return;
            }
        }
        bsp_display_set_gap_adj(gx, gy);
        reply_ok();
        return;
    } else if (strcmp(key, "rot") == 0) {
        app_set_rotation(atoi(val));
        reply_ok();
        return;
    } else if (strcmp(key, "static_ip") == 0) {
        if (strcmp(val, "-") == 0) {
            cfg.static_ip[0] = '\0';
        } else if (strlen(val) >= sizeof(cfg.static_ip) - 1) {
            reply_err("ip_too_long");
            return;
        } else {
            strlcpy(cfg.static_ip, val, sizeof(cfg.static_ip));
        }
        wifi_sta_set_static_ip(cfg.static_ip);
        wifi_changed = true;
    } else if (strcmp(key, "city") == 0) {
        strlcpy(cfg.city, val, sizeof(cfg.city));
    } else if (strcmp(key, "portal_base") == 0 || strcmp(key, "portal_url") == 0) {
        /* 门户地址：改成 "-" 或空串就回到编译期默认值 */
        if (strcmp(val, "-") == 0) {
            cfg.portal_base[0] = '\0';
        } else {
            strlcpy(cfg.portal_base, val, sizeof(cfg.portal_base));
        }
    } else if (strcmp(key, "portal_ac") == 0 || strcmp(key, "portal_ac_ip") == 0) {
        if (strcmp(val, "-") == 0) {
            cfg.portal_ac_ip[0] = '\0';
        } else {
            strlcpy(cfg.portal_ac_ip, val, sizeof(cfg.portal_ac_ip));
        }
    } else if (strcmp(key, "portal_ssid") == 0 || strcmp(key, "campus_ssid") == 0) {
        /* SSID 大小写敏感，别做任何转换 */
        if (strcmp(val, "-") == 0) {
            cfg.portal_ssid[0] = '\0';
        } else {
            strlcpy(cfg.portal_ssid, val, sizeof(cfg.portal_ssid));
        }
    } else if (strcmp(key, "lat") == 0) {
        strlcpy(cfg.lat, val, sizeof(cfg.lat));
    } else if (strcmp(key, "lon") == 0) {
        strlcpy(cfg.lon, val, sizeof(cfg.lon));
    } else if (strcmp(key, "brightness") == 0) {
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        cfg.brightness = (uint8_t) v;
        bsp_display_backlight(v);
    } else if (strcmp(key, "screen_off") == 0) {
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v > 60) v = 60;
        cfg.screen_off_min = (uint8_t) v;
    } else if (strcmp(key, "charge_off") == 0) {
        /* 1 = 插着电也按上面的分钟数熄屏；0 = 插电常亮（老行为） */
        cfg.charge_off = (uint8_t) (atoi(val) != 0);
    } else if (strcmp(key, "off_bright") == 0 || strcmp(key, "aod") == 0) {
        /* 0 = 全黑（顺便清屏防残影）；1..100 = 低亮度常显 */
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        cfg.off_bright = (uint8_t) v;
    } else if (strcmp(key, "theme") == 0) {
        /* 内置主题序号 0..BSP_THEME_COUNT-1；-1 表示自定义（配合 theme_bg/accent） */
        int v = atoi(val);
        if (v < 0 || v >= BSP_THEME_COUNT) {
            reply_err("theme 范围 0..4");
            return;
        }
        cfg.theme_id = (uint8_t) v;
        bsp_theme_load_preset(v);
        bsp_theme_ensure_readable(bg_has_image());
        theme_changed = true;
    } else if (strcmp(key, "deco_border") == 0 || strcmp(key, "deco_icon") == 0) {
        /* 边框 0..3，图标 0..5 */
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v > 5) v = 5;
        if (strcmp(key, "deco_border") == 0) {
            cfg.deco_border = (uint8_t) v;
        } else {
            cfg.deco_icon = (uint8_t) v;
        }
        bsp_deco_set(cfg.deco_border, cfg.deco_icon, cfg.deco_tag);
        theme_changed = true;
    } else if (strcmp(key, "deco_tag") == 0) {
        if (strcmp(val, "-") == 0) {
            cfg.deco_tag[0] = '\0';
        } else {
            strlcpy(cfg.deco_tag, val, sizeof(cfg.deco_tag));
        }
        bsp_deco_set(cfg.deco_border, cfg.deco_icon, cfg.deco_tag);
        theme_changed = true;
    } else if (strcmp(key, "probe_sec") == 0) {
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v > 3600) v = 3600;
        cfg.probe_sec = (uint16_t) v;
    } else if (strcmp(key, "probe_plugged") == 0) {
        cfg.probe_plugged = (uint8_t) (atoi(val) != 0);
    } else if (strcmp(key, "batt_low") == 0) {
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v > 240) v = 240;
        cfg.batt_low_min = (uint8_t) v;
    } else if (strcmp(key, "batt_keep") == 0) {
        cfg.batt_low_keepon = (uint8_t) (atoi(val) != 0);
    } else if (strcmp(key, "batt_est") == 0) {
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v > 200) v = 200;
        cfg.batt_est_hours = (uint8_t) v;
    } else if (strcmp(key, "batt_used") == 0) {
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v > 65535) v = 65535;
        cfg.batt_used_min = (uint16_t) v;
    } else if (strcmp(key, "esl_layout") == 0) {
        cfg.esl_layout = (uint8_t) ((atoi(val) != 0) ? 1 : 0);
        esl_ble_set_ui(cfg.esl_layout, cfg.esl_opts ? cfg.esl_opts : 0x0F, cfg.esl_text);
    } else if (strcmp(key, "esl_opts") == 0) {
        int v = atoi(val);
        if (v < 0) {
            v = 0;
        }
        cfg.esl_opts = (uint8_t) (v & 0x0F);
        esl_ble_set_ui(cfg.esl_layout, cfg.esl_opts, cfg.esl_text);
    } else if (strcmp(key, "esl_text") == 0) {
        if (strcmp(val, "-") == 0) {
            cfg.esl_text[0] = '\0';
        } else {
            strlcpy(cfg.esl_text, val, sizeof(cfg.esl_text));
        }
        esl_ble_set_ui(cfg.esl_layout, cfg.esl_opts ? cfg.esl_opts : 0x0F, cfg.esl_text);
    } else if (strcmp(key, "theme_auto") == 0) {
        cfg.theme_auto = (uint8_t) (atoi(val) != 0);
    } else if (strcmp(key, "theme_day") == 0 || strcmp(key, "theme_night") == 0) {
        int v = atoi(val);
        if (v < 0 || v >= BSP_THEME_COUNT) {
            reply_err("主题序号越界");
            return;
        }
        if (strcmp(key, "theme_day") == 0) {
            cfg.theme_day = (uint8_t) v;
        } else {
            cfg.theme_night = (uint8_t) v;
        }
    } else if (strcmp(key, "bg_rotate") == 0) {
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v > 2) v = 2;
        cfg.bg_rotate = (uint8_t) v;
    } else if (strcmp(key, "bg_slot") == 0) {
        bg_set_slot(atoi(val));
        theme_changed = true;
    } else if (strcmp(key, "theme_bg") == 0 || strcmp(key, "theme_accent") == 0) {
        /* 自定义主题：接受 0xRRGGBB（网页的 <input type=color>）或 0xRRRRGGGGBBBB 的 RGB565 */
        unsigned long raw = strtoul(val, NULL, 0);
        uint16_t c;
        if (raw > 0xFFFFul) {
            int r = (int) ((raw >> 16) & 0xFF);
            int g = (int) ((raw >> 8) & 0xFF);
            int b = (int) (raw & 0xFF);
            c = (uint16_t) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3));
        } else {
            c = (uint16_t) raw;
        }
        if (key[6] == 'b') {
            cfg.theme_bg = c;
        } else {
            cfg.theme_accent = c;
        }
        cfg.theme_id = 0xFF;
        bsp_theme_custom(cfg.theme_bg, cfg.theme_accent);
        bsp_theme_ensure_readable(bg_has_image());
        theme_changed = true;
    } else {
        known = false;
    }

    if (!known) {
        reply_err("unknown_key");
        return;
    }
    if (config_store_save(&cfg) != ESP_OK) {
        reply_err("nvs");
        return;
    }

    /* 站点参数随时同步给门户组件，改完下一次认证就用新地址 */
    portal_set_site(cfg.portal_base, cfg.portal_ac_ip, cfg.portal_ssid);

    ESP_LOGI(TAG, "串口改配置：%s", key);
    app_state_request(theme_changed ? APP_ACT_REDRAW :
                       (wifi_changed ? APP_ACT_RECONNECT : APP_ACT_RETRY));
    reply_ok();
}

/* 上传看门狗：PC 中途断了也不会让串口任务永久卡死——
   fread 会一直阻塞，所以用定时器兜底：60 秒还没收完就重启板子自愈 */
static esp_timer_handle_t s_bg_guard = NULL;

static void bg_guard_cb(void *arg)
{
    (void) arg;
    ESP_LOGW(TAG, "背景图上传 60 秒没完成（PC 断线？），重启板子");
    esp_restart();
}

/* 注销线程：12KB 栈，专门跑门户 HTTP */
static void logout_task(void *arg)
{
    (void) arg;
    char m[PORTAL_MAX_MSG] = {0};
    portal_login_t r = portal_logout(portal_last_wlan(), m, sizeof(m));
    printf("{\"logout\":%d,\"msg\":\"%s\"}\n", (int) r, m);
    fflush(stdout);
    vTaskDelete(NULL);
}

/* HITO：后台拉一条一言 → 写 esl_text → 保存 → 重画价签 */
static void hito_task(void *arg)
{
    (void) arg;

    char sentence[64];
    if (!hitokoto_fetch(sentence, sizeof(sentence))) {
        printf("@{\"ok\":false,\"err\":\"net\"}\n");
        fflush(stdout);
        vTaskDelete(NULL);
        return;
    }

    app_settings_t cfg;
    config_store_snapshot(&cfg);
    strlcpy(cfg.esl_text, sentence, sizeof(cfg.esl_text));
    if (config_store_save(&cfg) != ESP_OK) {
        printf("@{\"ok\":false,\"err\":\"nvs\"}\n");
        fflush(stdout);
        vTaskDelete(NULL);
        return;
    }
    esl_ble_set_ui(cfg.esl_layout, cfg.esl_opts ? cfg.esl_opts : 0x0F, cfg.esl_text);
    app_state_request(APP_ACT_REDRAW);

    cJSON *r = cJSON_CreateObject();
    if (r) {
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddStringToObject(r, "hitokoto", sentence);
        char *s = cJSON_PrintUnformatted(r);
        if (s) {
            printf("@%s\n", s);
            cJSON_free(s);
        }
        cJSON_Delete(r);
    }
    fflush(stdout);
    vTaskDelete(NULL);
}

static void cli_task(void *arg)
{
    (void) arg;
    char line[256];

    while (true) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (n == 0) {
            continue;
        }

        if (strncmp(line, "GET", 3) == 0) {
            send_status();
        } else if (strncmp(line, "SET ", 4) == 0) {
            do_set(line + 4);
        } else if (strncmp(line, "KEY ", 4) == 0) {
            ui_press(atoi(line + 4));      /* 0=BOOT 1=UP 2=DOWN */
            reply_ok();
        } else if (strncmp(line, "MODE ", 5) == 0) {
            ui_timer_mode(atoi(line + 5));   /* 0=倒计时 1=秒表 */
            reply_ok();
        } else if (strncmp(line, "SPLASH", 6) == 0) {
            ui_show_logo(!(strlen(line) > 7 && line[7] == '0'));
            reply_ok();
        } else if (strcmp(line, "TASKS") == 0) {
            /* 任务 CPU 占用 + 栈使用 */
            static TaskStatus_t ts[24];
            UBaseType_t n = uxTaskGetSystemState(ts, 24, NULL);
            uint32_t total = 0;
            for (UBaseType_t i = 0; i < n; i++) {
                total += ts[i].ulRunTimeCounter;
            }
            printf("@TASKS");
            for (UBaseType_t i = 0; i < n; i++) {
                uint32_t pct = total ? (uint32_t)(ts[i].ulRunTimeCounter * 100ULL / total) : 0;
                printf("\n  %-12s cpu%3u%%  stack_free%5u bytes",
                       ts[i].pcTaskName, (unsigned) pct,
                       (unsigned) (ts[i].usStackHighWaterMark * sizeof(StackType_t)));
            }
            printf("\n@END\n");
            fflush(stdout);
        } else if (strcmp(line, "WXREFRESH") == 0) {
            time_weather_force_refresh();
            reply_ok();
        } else if (strncmp(line, "LOG", 3) == 0 &&
                   (line[3] == '\0' || line[3] == ' ')) {
            if (line[3] == 'C' || (line[3] == ' ' && line[4] == 'C')) {
                logring_clear();
                reply_ok();
            } else {
                int n = 100;
                if (line[3] == ' ') {
                    n = atoi(line + 4);
                    if (n <= 0) {
                        n = 100;
                    }
                    if (n > 2000) {
                        n = 2000;
                    }
                }
                static char *buf = NULL;
                if (buf == NULL) {
                    buf = heap_caps_malloc(24 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                }
                if (buf == NULL) {
                    reply_err("oom");
                    break;
                }
                size_t len = logring_tail(buf, 24 * 1024, n);
                printf("@{\"ok\":true,\"lines\":%d,\"bytes\":%u,\"dropped\":%u}\n",
                       n, (unsigned) len, logring_dropped());
                fwrite(buf, 1, len, stdout);
                printf("\n@LOGEND\n");
                fflush(stdout);
            }
        } else if (strncmp(line, "LOGF", 4) == 0 &&
                   (line[4] == '\0' || line[4] == ' ')) {
            /* LOGF [n]：从 flash 里读历史日志（重启前的也能看到） */
            int n = 300;
            if (line[4] == ' ') {
                n = atoi(line + 5);
                if (n <= 0) {
                    n = 300;
                }
                if (n > 40000) {
                    n = 40000;
                }
            }
            static char *fbuf = NULL;
            if (fbuf == NULL) {
                fbuf = heap_caps_malloc(3400 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }
            if (fbuf == NULL) {
                reply_err("oom");
                break;
            }
            size_t len = flog_tail(fbuf, 3400 * 1024, n);
            printf("@{\"ok\":true,\"src\":\"flash\",\"lines\":%d,\"bytes\":%u}\n",
                   n, (unsigned) len);
            fwrite(fbuf, 1, len, stdout);
            printf("\n@LOGEND\n");
            fflush(stdout);
        } else if (strcmp(line, "FLOGC") == 0) {
            flog_clear();
            reply_ok();
        } else if (strcmp(line, "DNS") == 0) {
            printf("@{\"ok\":true,\"dns\":\"%s\"}\n", wifi_sta_dns_name());
            fflush(stdout);
        } else if (strcmp(line, "DNSNEXT") == 0) {
            wifi_sta_dns_rotate();
            printf("@{\"ok\":true,\"dns\":\"%s\"}\n", wifi_sta_dns_name());
            fflush(stdout);
        } else if (strcmp(line, "WIFIHARD") == 0) {
            wifi_sta_hard_restart();
            reply_ok();
        } else if (strcmp(line, "WIFIOFF") == 0) {
            app_wifi_night(true);
            reply_ok();
        } else if (strcmp(line, "WIFION") == 0) {
            app_wifi_night(false);
            reply_ok();
        } else if (strcmp(line, "RESETPUSH") == 0) {
            app_reset_esl_push();
            reply_ok();
        } else if (strcmp(line, "AUTH") == 0) {
            app_force_auth();
            reply_ok();

        } else if (strcmp(line, "PORTALHTML") == 0) {
            const char *body = NULL;
            size_t blen = portal_last_reply(&body);
            if (blen == 0 || body == NULL) {
                reply_err("no_reply");
            } else {
                printf("@HTML %u\n", (unsigned) blen);
                fwrite(body, 1, blen, stdout);
                printf("\n@HTMLEND\n");
            }
            fflush(stdout);
        } else if (strcmp(line, "MICRAW") == 0) {
            int32_t mn = 0, mx = 0;
            double rms = 0;
            int32_t smp[8] = {0};
            esp_err_t pe = audio_level_probe(&mn, &mx, &rms, smp, 8);
            if (pe != ESP_OK) {
                /* 注意：这里不能用 break —— 会跳出整个命令循环，
                   任务函数返回会触发 FreeRTOS abort。 */
                reply_err(esp_err_to_name(pe));
                fflush(stdout);
            } else {
            printf("@{\"ok\":true,\"min\":%d,\"max\":%d,\"rms\":%d,"
                   "\"p2p\":%d,\"samples\":[%d,%d,%d,%d,%d,%d,%d,%d]}\n",
                   (int) mn, (int) mx, (int) rms, (int) (mx - mn),
                   (int) smp[0], (int) smp[1], (int) smp[2], (int) smp[3],
                   (int) smp[4], (int) smp[5], (int) smp[6], (int) smp[7]);
                fflush(stdout);
            }
        } else if (strcmp(line, "DNS") == 0) {
            wifi_sta_dump_dns();
            reply_ok();
        } else if (strcmp(line, "WX") == 0) {
            /* 手动更新天气（和信息页长按 BOOT 同一个入口） */
            time_weather_force_refresh();
            reply_ok();
        } else if (strcmp(line, "ESLTEST") == 0) {
            /* 必须先把画布准备好（esl_ble_prepare 会分配缓冲、取数据），
               直接画会用到未初始化的画布 → 崩溃。 */
            esp_err_t pe = esl_ble_prepare(true);
            if (pe != ESP_OK) {
                reply_err(esp_err_to_name(pe));
                fflush(stdout);
                break;
            }
            esl_ble_draw_test_pattern();
            esp_err_t e = esl_ble_push_manual(true);   /* 手动推：绕过网络门 */
            if (e == ESP_OK) {
                reply_ok();
            } else {
                reply_err(esp_err_to_name(e));
            }
        } else if (strncmp(line, "BOOTHINT", 8) == 0) {
            /* BOOTHINT 1=连接中 2=认证中 3=已认证 4=未完成；不带参数=1 */
            int n = (line[8] == ' ') ? atoi(line + 9) : 1;
            if (n == 0) {
                ui_unfreeze();
                reply_ok();
            } else {
            switch (n) {
            case 2:
                ui_boot_hint("网络已通，正在认证中");
                break;
            case 3:
                ui_boot_hint("已认证，进入主界面");
                break;
            case 4:
                ui_boot_hint("认证未完成，进界面继续重试");
                break;
            default:
                ui_boot_hint("正在连接 WiFi 中");
                break;
            }
            reply_ok();
            }
        } else if (strcmp(line, "BOOTANIM") == 0) {
            ui_replay_boot_anim(4);          /* 慢 4 倍，方便看/截图 */
            reply_ok();
        } else if (strcmp(line, "SCAN") == 0 || strncmp(line, "SCAN ", 5) == 0) {
            int sec = (line[4] == ' ') ? atoi(line + 5) : 8;
            if (sec < 1) sec = 1;
            if (sec > 30) sec = 30;
            esp_err_t e = esl_ble_tracker_scan_async((uint32_t) sec * 1000);
            if (e == ESP_OK) {
                printf("@{\"ok\":true,\"sec\":%d}\n", sec);
            } else {
                reply_err("busy");
            }
            fflush(stdout);
        } else if (strncmp(line, "EPDCACHE", 8) == 0) {
            /* EPDCACHE 查看缓存；EPDCACHE RESET 作废缓存 */
            bool reset = (strstr(line, "RESET") != NULL);
            int built = 0;
            bool authed = false;
            esl_ble_cache_debug(reset, &built, &authed);
            printf("@{\"ok\":true,\"built_ms\":%d,\"authed\":%s}\n",
                   built, authed ? "true" : "false");
            fflush(stdout);
        } else if (strncmp(line, "SCANTEST ", 9) == 0) {
            /* SCANTEST <hex>：只分类，不记账。验证手机/电脑判定用 */
            const char *hexp = line + 9;
            size_t hl = strlen(hexp);
            uint8_t ad[64];
            size_t n = 0;
            for (size_t i = 0; i + 1 < hl && n < sizeof(ad); i += 2) {
                char b[3] = { hexp[i], hexp[i + 1], 0 };
                ad[n++] = (uint8_t) strtoul(b, NULL, 16);
            }
            int cls = 0, conf = 0;
            char nm[20] = { 0 };
            int v = tracker_scan_classify(ad, (int) n, &cls, &conf, nm, sizeof(nm));
            printf("@{\"ok\":true,\"bytes\":%u,\"vendor\":%d,\"cls\":%d,"
                   "\"conf\":%d,\"name\":\"%s\"}\n",
                   (unsigned) n, v, cls, conf, nm);
            fflush(stdout);
        } else if (strcmp(line, "SCANRAW") == 0) {
            /* 导出未识别设备的原始广播（hex），用于加新厂商指纹 */
            trk_raw_t raw[TRK_RAW_MAX];
            int n = tracker_scan_raw(raw, TRK_RAW_MAX);
            printf("@{\"ok\":true,\"count\":%d,\"items\":[", n);
            for (int i = 0; i < n; i++) {
                printf("%s{\"rssi\":%d,\"seen\":%d,\"hex\":\"",
                       i ? "," : "", raw[i].rssi, (int) raw[i].seen);
                for (int k = 0; k < raw[i].len; k++) {
                    printf("%02x", raw[i].data[k]);
                }
                printf("\"}");
            }
            printf("]}\n");
            fflush(stdout);
        } else if (strcmp(line, "SCANTOP") == 0) {
            trk_entry_t top[8];
            int n = tracker_scan_top(top, 8);
            printf("@{\"ok\":true,\"total\":%d,\"cost_ms\":%u,\"items\":[",
                   tracker_scan_count(), (unsigned) tracker_scan_last_cost_ms());
            for (int i = 0; i < n; i++) {
                printf("%s{\"vendor\":%d,\"cls\":%d,\"conf\":%d,\"rssi\":%d,"
                       "\"seen\":%d,\"name\":\"%s\"}",
                       i ? "," : "", top[i].vendor, top[i].cls, top[i].conf,
                       top[i].rssi_last, (int) top[i].seen, top[i].name);
            }
            printf("]}\n");
            fflush(stdout);
        } else if (strcmp(line, "NETSTAT") == 0) {
            /* 拉一次仓库里的统计（联网才有用），返回人数和我的百分位 */
            esp_err_t e = netstats_refresh();
            if (e == ESP_OK) {
                printf("@{\"ok\":true,\"users\":%d}\n", netstats_users());
            } else {
                printf("@{\"ok\":false,\"err\":\"net\",\"users\":%d}\n",
                        netstats_users());
            }
            fflush(stdout);
        } else if (strcmp(line, "HITO") == 0) {
            /* 联网拉一句「一言」：HTTP 吃栈，丢后台任务 */
            if (xTaskCreate(hito_task, "hito", 8192, NULL, 4, NULL) != pdPASS) {
                reply_err("no_mem");
            }
        } else if (strcmp(line, "ESLPREP") == 0) {
            /* 调试：直接画一张信息页进缓冲（不改状态、不推蓝牙） */
            if (esl_ble_prepare(true) == ESP_OK) {
                reply_ok();
            } else {
                reply_err("no_mem");
            }
        } else if (strcmp(line, "ESLIMG") == 0) {
            /* dump 墨水屏画面：9472 字节 -> 18944 个 hex 字符 */
            size_t len = 0;
            const uint8_t *img = esl_ble_image(&len);
            if (img == NULL || len == 0) {
                reply_err("no_image");
            } else {
                printf("@IMG %u\n", (unsigned) len);
                for (size_t i = 0; i < len; i++) {
                    printf("%02x", img[i]);
                    if ((i & 63) == 63) {
                        printf("\n");
                    }
                }
                printf("\n@END\n");
                fflush(stdout);
            }
        } else if (strcmp(line, "LOGOUT") == 0) {
            /* 门户 HTTP 请求很吃栈，不能在本任务里做（会把 usbcli 撑爆） */
            if (xTaskCreate(logout_task, "logout", 12288, NULL, 4, NULL) != pdPASS) {
                printf("{\"logout\":-1,\"msg\":\"no_mem\"}\n");
                fflush(stdout);
            }
        } else if (strcmp(line, "NOISE") == 0) {
            ui_noise_test();
            reply_ok();
        } else if (strncmp(line, "PAGE ", 5) == 0) {
            ui_goto(atoi(line + 5));
            reply_ok();
        } else if (strcmp(line, "SHOT") == 0) {
            /* 把 240x240 RGB565 帧缓冲原样吐出来（115200 字节，约 10 秒） */
            const uint16_t *fb = bsp_display_framebuffer();
            if (fb == NULL) {
                reply_err("nofb");
                break;
            }
            if (s_shot_busy) {
                reply_err("busy");
                break;
            }
            s_shot_busy = true;
            ui_freeze(true);
            vTaskDelay(pdMS_TO_TICKS(600));      /* 等当前这一帧画完 */

            /* 转储期间静音日志，否则别的任务的日志会插进二进制流。
               用"换钩子"而不是改日志级别：不覆盖各模块单独调过的级别。 */
            vprintf_like_t prev_log = esp_log_set_vprintf(shot_log_sink);
            if (s_shot_guard) {
                esp_timer_start_once(s_shot_guard, 30LL * 1000 * 1000);
            }
            printf("@SHOT %u\n", (unsigned) (240 * 240 * 2));
            fflush(stdout);
            const uint8_t *p = (const uint8_t *) fb;
            size_t want = 240 * 240 * 2, done = 0;
            int64_t deadline = esp_timer_get_time() / 1000 + 30000;
            while (done < want) {
                size_t n = fwrite(p + done, 1, want - done, stdout);
                if (n > 0) {
                    done += n;
                    continue;
                }
                /* TX 环满了（对端没在读）：等一下再来，别当成写完 */
                if (esp_timer_get_time() / 1000 >= deadline) {
                    ESP_LOGW(TAG, "截图只发出 %u/%u 字节，放弃",
                             (unsigned) done, (unsigned) want);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            fflush(stdout);
            if (s_shot_guard) {
                esp_timer_stop(s_shot_guard);
            }
            esp_log_set_vprintf(prev_log);      /* 换回原来的日志钩子 */
            ui_freeze(false);
            s_shot_busy = false;
#if USB_DEBUG_CMDS
        } else if (strcmp(line, "CAPIMG") == 0) {
            size_t n = 0;
            const uint8_t *png = portal_last_captcha_png(&n);
            printf("@{\"ok\":true,\"len\":%u,\"hex\":\"", (unsigned) n);
            for (size_t i = 0; i < n && i < 2048; i++) {
                printf("%02x", png[i]);
            }
            printf("\"}\n");
            fflush(stdout);
#endif   /* USB_DEBUG_CMDS */
        } else if (strcmp(line, "CAPTCHA") == 0) {
            char code[8] = {0};
            float conf[4] = {0};
            int rc = portal_captcha_test(code, sizeof(code), conf);
            printf("@{\"ok\":%s,\"code\":\"%s\",\"conf\":[%.2f,%.2f,%.2f,%.2f]}\n",
                   rc == 0 ? "true" : "false", code, conf[0], conf[1], conf[2], conf[3]);
            fflush(stdout);
        } else if (strcmp(line, "ESL") == 0) {
            esp_err_t e = esl_ble_push_manual(true);   /* 手动推：绕过网络门 */
            printf("@{\"ok\":%s}\n", e == ESP_OK ? "true" : "false");
            fflush(stdout);

        } else if (strncmp(line, "BGR ", 4) == 0) {
            /* 原始二进制上传：BGR <字节数> 之后紧跟裸数据，不再走 base64，
               115200 字节在 115200 波特率下约 10 秒 */
            size_t total = (size_t) atoi(line + 4);
            if (total != BG_BYTES) {
                reply_err("len");
                break;
            }
            if (bg_upload_begin() != ESP_OK) {
                reply_err("erase");
                break;
            }
            uint8_t buf[512];
            size_t off = 0;
            if (s_bg_guard) {
                esp_timer_start_once(s_bg_guard, 60LL * 1000 * 1000);
            }
            while (off < total) {
                size_t want = total - off;
                if (want > sizeof(buf)) {
                    want = sizeof(buf);
                }
                    size_t got = fread(buf, 1, want, stdin);
                if (got == 0) {
                    break;                  /* 对端没了/超时，交给 finish 报错 */
                }
                bg_upload_chunk(buf, got, off);
                off += got;
            }
            if (s_bg_guard) {
                esp_timer_stop(s_bg_guard);
            }
            esp_err_t e = bg_upload_finish(off);
            if (e == ESP_OK) {
                printf("@{\"ok\":true,\"off\":%u}\n", (unsigned) off);
                app_state_request(APP_ACT_REDRAW);
            } else {
                printf("@{\"ok\":false,\"err\":\"upload\",\"off\":%u}\n", (unsigned) off);
            }
            fflush(stdout);
        } else if (strcmp(line, "BGB") == 0) {
            s_bg_off = 0;
            esp_err_t e = bg_upload_begin();
            printf("@{\"ok\":%s}\n", e == ESP_OK ? "true" : "false");
            fflush(stdout);
        } else if (strncmp(line, "BGD ", 4) == 0) {
            uint8_t buf[160];
            size_t n = b64_decode(line + 4, buf, sizeof(buf));
            esp_err_t e = (n > 0) ? bg_upload_chunk(buf, n, s_bg_off) : ESP_ERR_INVALID_ARG;
            if (e == ESP_OK) {
                s_bg_off += n;
            }
            printf("@{\"ok\":%s,\"off\":%u}\n",
                   e == ESP_OK ? "true" : "false", (unsigned) s_bg_off);
            fflush(stdout);
        } else if (strcmp(line, "BGE") == 0) {
            esp_err_t e = bg_upload_finish(s_bg_off);
            printf("@{\"ok\":%s,\"off\":%u}\n",
                   e == ESP_OK ? "true" : "false", (unsigned) s_bg_off);
            fflush(stdout);
            s_bg_off = 0;
            if (e == ESP_OK) {
                app_state_request(APP_ACT_REDRAW);
            }
        } else if (strcmp(line, "BGC") == 0) {
            esp_err_t e = bg_erase();
            printf("@{\"ok\":%s}\n", e == ESP_OK ? "true" : "false");
            fflush(stdout);
            if (e == ESP_OK) {
                app_state_request(APP_ACT_REDRAW);
            }
        } else if (strncmp(line, "VOL ", 4) == 0) {
            beep_set_volume(atoi(line + 4));
            reply_ok();
        } else if (strcmp(line, "PING") == 0) {
            reply_ok();
        } else if (strcmp(line, "RECONNECT") == 0) {
            app_state_request(APP_ACT_RECONNECT);
            reply_ok();
        } else if (strcmp(line, "REBOOT") == 0) {
            reply_ok();
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        } else {
            ESP_LOGW(TAG, "未知命令: %.60s", line);
            reply_err("unknown");
        }
    }
}

void usb_console_init(void)
{
    /* 控制台默认会把 \n 转成 \r\n、把 CRLF 转成 LF，二进制数据（截图、背景图）
       会被改坏：这里改成不转换，命令仍按 \n 分帧 */
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);

    if (s_bg_guard == NULL) {
        const esp_timer_create_args_t bg_args = {
            .callback = bg_guard_cb,
            .name = "bgguard",
        };
        esp_timer_create(&bg_args, &s_bg_guard);
    }
    const esp_timer_create_args_t guard_args = {
        .callback = shot_guard_cb,
        .name = "shotguard",
    };
    if (s_shot_guard == NULL) {
        esp_timer_create(&guard_args, &s_shot_guard);
    }
    xTaskCreate(cli_task, "usbcli", 8192, NULL, 2, NULL);   /* fetch_captcha 的 4KB 局部缓冲要够 */
}
