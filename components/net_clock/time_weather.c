#include "time_weather.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "netmon.h"

static const char *TAG = "clock";

/* 每天 8:00 全量拉一次天气（天气 + 空气质量 + 3 天预报 + 日出日落）。
   原来是每 30 分钟，频繁 TLS 握手既占内存又占射频，对 WiFi 认证不利。 */
#define WEATHER_INTERVAL_S (60 * 60)   /* 联网数据每小时拉一次 */
#define WEATHER_RETRY_S    (5 * 60)   /* 8 点没拉成功，5 分钟后再试 */
#define WEATHER_FAIL_MAX   6          /* 最多试 6 次（半小时） */
#define RESP_MAX 6144                      /* 国内天气 JSON 约 4KB */

static time_weather_t s_tw;
static char s_lat[16] = "39.90";
static char s_lon[16] = "116.41";
static char s_city[32] = "北京";
static volatile bool s_refresh_now = false;
static int s_fail_streak = 0;
/* 手动刷新的结果，给界面显示用 */
static volatile int  s_manual_state = 0;   /* 0=无 1=进行中 2=成功 3=失败 */
static volatile int64_t s_manual_ms = 0;
static volatile uint32_t s_last_ok_epoch = 0;

/* WMO 天气代码 -> 中文。字库会自动收录这里出现的汉字 */
const char *time_weather_wmo_text(int code)
{
    switch (code) {
    case 0:  return "晴";
    case 1:  return "晴间多云";
    case 2:  return "多云";
    case 3:  return "阴";
    case 45: case 48: return "雾";
    case 51: case 53: case 55: return "毛毛雨";
    case 56: case 57: return "冻毛毛雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 66: case 67: return "冻雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "雪粒";
    case 80: return "阵雨";
    case 81: return "强阵雨";
    case 82: return "暴雨";
    case 85: case 86: return "阵雪";
    case 95: return "雷阵雨";
    case 96: case 99: return "雷暴冰雹";
    default: return "未知";
    }
}

/* ---------------------------------------------------------------- 天气 */

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        char *buf = (char *) evt->user_data;
        size_t used = strlen(buf);
        size_t room = RESP_MAX - used - 1;
        if (room > 0) {
            size_t n = (size_t) evt->data_len < room ? (size_t) evt->data_len : room;
            memcpy(buf + used, evt->data, n);
            buf[used + n] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t air_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        char *buf = (char *) evt->user_data;
        size_t used = strlen(buf);
        size_t room = 1024 - used - 1;
        if (room > 0) {
            size_t n = (size_t) evt->data_len < room ? (size_t) evt->data_len : room;
            memcpy(buf + used, evt->data, n);
            buf[used + n] = '\0';
        }
    }
    return ESP_OK;
}

static bool fetch_weather(void)
{
    /* 拉天气要开 TLS，握手峰值 30KB 左右。内存紧（比如蓝牙正开着）
       就别硬上，不然会把系统顶到 OOM —— 下一个周期再试 */
    extern bool wifi_sta_connected(void);
    if (!wifi_sta_connected()) {
        ESP_LOGW(TAG, "WiFi 没连上，跳过这次拉取");
        return false;
    }

    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_int < 40000) {
        ESP_LOGW(TAG, "内存紧张（%uK），这次先不拉天气", (unsigned) (free_int / 1024));
        return false;
    }

    EXT_RAM_BSS_ATTR static char resp[RESP_MAX];
    resp[0] = '\0';

    char url[384];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast"
             "?latitude=%s&longitude=%s"
             "&current=temperature_2m,relative_humidity_2m,weather_code"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset"
             "&forecast_days=3"
             "&timezone=Asia%%2FShanghai", s_lat, s_lon);

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event,
        .user_data = resp,
        .timeout_ms = 8000,   /* 8 秒偶尔不够（EAGAIN），放宽到 15 秒 */
        /* 同一主机连续请求复用连接，少做一次 TLS 握手 */
        .keep_alive_enable = true,
        .keep_alive_idle = 10,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return false;
    }

    time_weather_tls_lock();
    esp_err_t err = esp_http_client_perform(client);
    time_weather_tls_unlock();
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "天气请求失败: %s (HTTP %d)", esp_err_to_name(err), status);
        return false;
    }

    netmon_add_bytes(strlen(resp), strlen(url));

    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) {
        ESP_LOGW(TAG, "天气 JSON 解析失败");
        return false;
    }

    bool ok = false;
    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (cur != NULL) {
        cJSON *temp = cJSON_GetObjectItem(cur, "temperature_2m");
        cJSON *hum = cJSON_GetObjectItem(cur, "relative_humidity_2m");
        cJSON *code = cJSON_GetObjectItem(cur, "weather_code");

        if (cJSON_IsNumber(temp) && cJSON_IsNumber(hum) && cJSON_IsNumber(code)) {
            s_tw.temp_c = (int) (temp->valuedouble + (temp->valuedouble >= 0 ? 0.5 : -0.5));
            s_tw.humidity = hum->valueint;
            s_tw.code = code->valueint;
            s_tw.desc = time_weather_wmo_text(s_tw.code);
            s_tw.weather_valid = true;
            ok = true;
            ESP_LOGI(TAG, "天气: %s %d°C 湿度 %d%%", s_tw.desc, s_tw.temp_c, s_tw.humidity);
        }
    }

    /* 3 天预报 + 日出日落 */
    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (daily != NULL) {
        cJSON *code = cJSON_GetObjectItem(daily, "weather_code");
        cJSON *tmax = cJSON_GetObjectItem(daily, "temperature_2m_max");
        cJSON *tmin = cJSON_GetObjectItem(daily, "temperature_2m_min");
        cJSON *rise = cJSON_GetObjectItem(daily, "sunrise");
        cJSON *set  = cJSON_GetObjectItem(daily, "sunset");

        if (cJSON_IsArray(code) && cJSON_IsArray(tmax) && cJSON_IsArray(tmin)) {
            for (int i = 0; i < 3 && i < cJSON_GetArraySize(code); i++) {
                cJSON *c0 = cJSON_GetArrayItem(code, i);
                cJSON *c1 = cJSON_GetArrayItem(tmax, i);
                cJSON *c2 = cJSON_GetArrayItem(tmin, i);
                if (cJSON_IsNumber(c0) && cJSON_IsNumber(c1) && cJSON_IsNumber(c2)) {
                    s_tw.fc[i].code = c0->valueint;
                    s_tw.fc[i].tmax = (int) (c1->valuedouble + (c1->valuedouble >= 0 ? 0.5 : -0.5));
                    s_tw.fc[i].tmin = (int) (c2->valuedouble + (c2->valuedouble >= 0 ? 0.5 : -0.5));
                }
            }
            s_tw.daily_valid = true;
            ok = true;
        }
        /* sunrise/sunset 形如 "2026-10-31T06:42"，只取时分 */
        if (cJSON_IsArray(rise) && cJSON_GetArraySize(rise) > 0) {
            const char *t = cJSON_GetArrayItem(rise, 0)->valuestring;
            if (t != NULL && strlen(t) >= 16) {
                snprintf(s_tw.sunrise, sizeof(s_tw.sunrise), "%.5s", t + 11);
            }
        }
        if (cJSON_IsArray(set) && cJSON_GetArraySize(set) > 0) {
            const char *t = cJSON_GetArrayItem(set, 0)->valuestring;
            if (t != NULL && strlen(t) >= 16) {
                snprintf(s_tw.sunset, sizeof(s_tw.sunset), "%.5s", t + 11);
            }
        }
        if (s_tw.daily_valid) {
            ESP_LOGI(TAG, "预报: 今 %d/%d 明 %d/%d 后 %d/%d 日出 %s 日落 %s",
                     s_tw.fc[0].tmax, s_tw.fc[0].tmin, s_tw.fc[1].tmax, s_tw.fc[1].tmin,
                     s_tw.fc[2].tmax, s_tw.fc[2].tmin, s_tw.sunrise, s_tw.sunset);
        }
    }

    cJSON_Delete(root);
    return ok;
}

/* 空气质量（Open-Meteo 另一个域名） */
static bool fetch_air(void)
{
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_int < 60000) {
        return false;
    }
    static char resp[1024];
    resp[0] = '\0';

    char url[220];
    snprintf(url, sizeof(url),
             "http://air-quality-api.open-meteo.com/v1/air-quality"
             "?latitude=%s&longitude=%s"
             "&current=us_aqi,pm2_5"
             "&timezone=Asia%%2FShanghai", s_lat, s_lon);

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = air_event,
        .user_data = resp,
        .timeout_ms = 8000,   /* 8 秒偶尔不够（EAGAIN），放宽到 15 秒 */
        /* 同一主机连续请求复用连接，少做一次 TLS 握手 */
        .keep_alive_enable = true,
        .keep_alive_idle = 10,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return false;
    }
    time_weather_tls_lock();
    esp_err_t err = esp_http_client_perform(client);
    time_weather_tls_unlock();
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "空气质量请求失败: %s (HTTP %d)", esp_err_to_name(err), status);
        return false;
    }
    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) {
        return false;
    }
    cJSON *cur = cJSON_GetObjectItem(root, "current");
    bool ok = false;
    if (cur != NULL) {
        cJSON *aqi = cJSON_GetObjectItem(cur, "us_aqi");
        cJSON *pm = cJSON_GetObjectItem(cur, "pm2_5");
        if (cJSON_IsNumber(aqi)) {
            s_tw.aqi = (int) aqi->valuedouble;
            s_tw.pm25 = cJSON_IsNumber(pm) ? (int) pm->valuedouble : 0;
            s_tw.aqi_valid = true;
            ok = true;
            ESP_LOGI(TAG, "空气质量: AQI %d PM2.5 %d", s_tw.aqi, s_tw.pm25);
        }
    }
    cJSON_Delete(root);
    return ok;
}

/* ---------------------------------------------------------------- 时间 */

static void sync_time(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_set_time_sync_notification_cb(NULL);
    esp_sntp_init();

    for (int i = 0; i < 30; i++) {
        time_t now = 0;
        time(&now);
        struct tm t;
        localtime_r(&now, &t);
        if (t.tm_year >= 120) {   /* 2020 年之后就算同步上了 */
            s_tw.time_valid = true;
            ESP_LOGI(TAG, "时间已同步: %04d-%02d-%02d %02d:%02d:%02d",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "时间同步超时（可能还没联网）");
}

/* ---------------------------------------------------------------- 任务 */

/* 天气存 NVS：重启后墨水屏还能显示上一次的天气，不会一片空白 */
#define WX_NS  "wx"
#define WX_KEY "last"

static void weather_save_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(WX_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    /* desc 是 const char* 指针，绝不能存进 NVS（重启后指向垃圾） */
    time_weather_t tmp = s_tw;
    tmp.desc = NULL;
    nvs_set_blob(h, WX_KEY, &tmp, sizeof(tmp));
    /* 记下这份数据是什么时候拉的：重启后也能算出"更新于 N 小时前" */
    nvs_set_u32(h, "wx_ts", (uint32_t) (s_last_ok_epoch ? s_last_ok_epoch : time(NULL)));
    nvs_close(h);
}

static void weather_load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(WX_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    time_weather_t tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, WX_KEY, &tmp, &len) == ESP_OK && len == sizeof(tmp)) {
        s_tw = tmp;
        /* 指针字段用 code 还原，避免悬空指针 */
        s_tw.desc = tmp.weather_valid ? time_weather_wmo_text(s_tw.code) : NULL;
        s_tw.fetching = false;
        ESP_LOGI(TAG, "已载入上次天气：%dC %d%% %s", s_tw.temp_c, s_tw.humidity,
                 s_tw.desc ? s_tw.desc : "-");
        /* 恢复"上次更新时间"，这样离线时也能显示数据有多旧 */
        uint32_t ts = 0;
        if (nvs_get_u32(h, "wx_ts", &ts) == ESP_OK && ts != 0) {
            s_last_ok_epoch = ts;
            ESP_LOGI(TAG, "这份数据是上次拉取的（时间戳 %u）", (unsigned) ts);
        }
    }
    nvs_close(h);
}

/* 全局 TLS 串行锁：天气、空气质量、日历不要同时握手
   （每次 TLS 峰值要 30KB 连续内部 RAM，两个一起来就会失败） */
static SemaphoreHandle_t s_tls_mutex = NULL;

void time_weather_tls_lock(void)
{
    if (s_tls_mutex == NULL) {
        s_tls_mutex = xSemaphoreCreateMutex();
    }
    if (s_tls_mutex) {
        xSemaphoreTake(s_tls_mutex, portMAX_DELAY);
    }
}

void time_weather_tls_unlock(void)
{
    if (s_tls_mutex) {
        xSemaphoreGive(s_tls_mutex);
    }
}

static void clock_task(void *arg)
{
    (void) arg;

    weather_load_nvs();     /* 先恢复上次天气，界面上马上有数据 */

    /* 对时可能要等十几秒，放在任务里做，
       不能阻塞 app_main，否则屏幕迟迟不亮、界面任务起不来 */
    sync_time();

    /* 开机第一次进来就拉一次（否则要等到早上 8 点才有数据） */
    bool first = true;
    int last_yday = -1;              /* 保留（小时制不再用它） */
    int retry_left = 0;              /* 8 点失败后的重试次数 */
    int retry_wait = 0;              /* 距离下次重试还有多少秒 */

    while (true) {
        time_t now = 0;
        time(&now);
        struct tm t;
        localtime_r(&now, &t);

        s_tw.year = t.tm_year + 1900;
        s_tw.hour = t.tm_hour;
        s_tw.minute = t.tm_min;
        s_tw.second = t.tm_sec;
        s_tw.month = t.tm_mon + 1;
        s_tw.day = t.tm_mday;
        s_tw.weekday = t.tm_wday;
        if (t.tm_year >= 120) {
            s_tw.time_valid = true;
        }

        if (s_tw.weather_valid) {
            s_tw.last_update_s = 0;
        }

        /* 什么时候全量拉：
           1) 开机第一次
           2) 每天 8 点（还没拉过）
           3) 8 点那次失败，5 分钟重试（最多 6 次）
           4) 手动 time_weather_refresh() */
        bool due = false;
        (void) last_yday;
        if (first) {
            due = true;
            retry_left = WEATHER_FAIL_MAX;      /* 开机这次失败也要重试 */
        } else if (!s_tw.weather_valid) {
            /* 还没有天气数据：不管几点都要重试，否则墨水屏一整天没天气。
               前 6 次每 5 分钟，之后每 30 分钟试一次，直到拿到数据 */
            if (retry_wait > 0) {
                retry_wait--;
            } else {
                due = true;
            }
        } else if (t.tm_year >= 120) {
            /* 每小时拉一次联网数据（天气 + 空气质量）。
               失败时按 retry_wait 的节奏在那个小时里重试 */
            uint32_t now_e = (uint32_t) time(NULL);
            bool need = (s_last_ok_epoch == 0) ||
                        (now_e - s_last_ok_epoch >= WEATHER_INTERVAL_S);
            if (need) {
                if (retry_wait > 0) {
                    retry_wait--;
                } else {
                    due = true;
                }
            } else {
                retry_wait = 0;
            }
        }
        if (s_refresh_now) {
            s_refresh_now = false;
            due = true;
            s_manual_state = 1;          /* 进行中 */
        }

        if (due) {
            first = false;
            s_tw.fetching = true;
            bool ok = fetch_weather();
            if (!ok) {
                /* DNS 可能返回多个 IP，第一次连到不通的那个很正常：立刻再试一次 */
                vTaskDelay(pdMS_TO_TICKS(1500));
                ok = fetch_weather();
            }
            /* 空气质量是另一个接口，单独拉一次（失败不影响天气） */
            fetch_air();
            s_tw.fetching = false;

            if (s_manual_state == 1) {
                s_manual_state = ok ? 2 : 3;
                s_manual_ms = esp_timer_get_time() / 1000;

                ESP_LOGI(TAG, "手动刷新天气：%s", ok ? "成功" : "失败");
            }

            if (ok) {
                s_last_ok_epoch = (uint32_t) time(NULL);
                last_yday = t.tm_yday;
                retry_left = 0;
                retry_wait = 0;
                s_fail_streak = 0;
                s_tw.last_update_s = 0;
                weather_save_nvs();
                ESP_LOGI(TAG, "天气已全量更新（%04d-%02d-%02d %02d:%02d）",
                         t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
            } else {
                /* 网络时通时断：失败后先密后疏地重试。
                   前 WEATHER_FAIL_MAX 次各 60 秒，之后拉到 5 分钟，
                   都不行才落到 30 分钟慢慢试 */
                if (retry_left > 0 || s_fail_streak < WEATHER_FAIL_MAX) {
                    retry_left--;
                    retry_wait = 60;
                    s_fail_streak++;
                } else if (s_fail_streak < WEATHER_FAIL_MAX + 6) {
                    retry_wait = 5 * 60;
                    s_fail_streak++;
                } else {
                    retry_wait = 30 * 60;
                }
                ESP_LOGW(TAG, "天气没拉成功，%d 秒后再试（连续失败 %d 次）",
                         retry_wait, s_fail_streak);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void time_weather_start(const char *lat, const char *lon, const char *city)
{
    if (lat) snprintf(s_lat, sizeof(s_lat), "%s", lat);
    if (lon) snprintf(s_lon, sizeof(s_lon), "%s", lon);
    if (city) snprintf(s_city, sizeof(s_city), "%s", city);

    setenv("TZ", "CST-8", 1);
    tzset();
    s_tw.desc = "未知";
    s_tw.last_update_s = -1;

    xTaskCreate(clock_task, "clock", 6144, NULL, 2, NULL);
    ESP_LOGI(TAG, "对时和天气任务已启动，位置 %s (%s,%s)", s_city, s_lat, s_lon);
}

void time_weather_get(time_weather_t *out)
{
    if (out == NULL) return;
    *out = s_tw;
}

/* 手动强制刷新：不管现在几点、有没有数据，都立刻拉一次全量数据 */
void time_weather_force_refresh(void)
{
    s_refresh_now = true;
    s_manual_state = 1;
    s_manual_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "手动触发天气更新");
}

/* 界面用：距上次成功拉取多少秒（0 = 不知道） */
int time_weather_age_s(void)
{
    if (s_last_ok_epoch == 0) {
        return 0;
    }
    uint32_t now = (uint32_t) time(NULL);
    if (now < s_last_ok_epoch) {
        return 0;
    }
    return (int) (now - s_last_ok_epoch);
}

/* 界面用：0=无 1=进行中 2=成功 3=失败（成功/失败 8 秒后自动清除） */
int time_weather_manual_state(void)
{
    if (s_manual_state >= 2) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now - s_manual_ms > 8000) {
            s_manual_state = 0;
        }
    }
    return s_manual_state;
}

void time_weather_refresh(void)      /* 断线重连时被调用 */
{
    /* 三种情况要补拉：
       1) 还没有天气数据（开机/缓存丢了）
       2) 上次手动刷新失败了
       3) 缓存里的数据已经超过 5 分钟 —— 断网期间显示的是旧数据，
          一恢复联网就该换上新的 */
    uint32_t now = (uint32_t) time(NULL);
    bool stale = (s_last_ok_epoch == 0) ||
                 (now > s_last_ok_epoch && (now - s_last_ok_epoch) >= 300);
    if (!s_tw.weather_valid || s_manual_state == 3 || stale) {
        s_manual_state = 0;
        s_refresh_now = true;
        ESP_LOGI(TAG, "网络恢复，补拉联网数据（上次更新 %u 秒前）",
                 (unsigned) (s_last_ok_epoch ? now - s_last_ok_epoch : 0));
    }
}