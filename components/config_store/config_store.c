#include "config_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "config";

#define NVS_NAMESPACE "appcfg"
#define NVS_KEY       "settings"
#define CFG_MAGIC     0x554E4943   /* "UNIC" */

static app_settings_t s_cfg;
static app_settings_t s_defaults;   /* 编译期默认值，给旧 blob 补新字段用 */
static uint32_t s_generation = 0;
static bool s_inited = false;
static bool s_from_nvs = false;
static SemaphoreHandle_t s_lock = NULL;   /* 网页线程和主线程都会访问 */

static void cfg_lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void cfg_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (src == NULL) src = "";
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

esp_err_t config_store_init(const app_settings_t *defaults)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    memset(&s_cfg, 0, sizeof(s_cfg));

    if (defaults != NULL) {
        s_cfg = *defaults;
        s_cfg.magic = CFG_MAGIC;
    }
    s_defaults = s_cfg;   /* 记住默认值：旧 blob 缺的字段用它补 */

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS 里没有配置，使用编译期默认值");
        s_inited = true;
        return ESP_OK;
    }

    app_settings_t stored;
    memset(&stored, 0, sizeof(stored));
    size_t len = sizeof(stored);
    err = nvs_get_blob(h, NVS_KEY, &stored, &len);
    nvs_close(h);

    /* len <= sizeof(stored)：兼容旧固件存下的更小 blob，
       新增字段（比如屏幕方向 rot）读出来是 0，不会因为结构体变大就丢配置。 */
    /* magic 在结构体开头：校验不过就整块丢弃，用编译期默认值。
       默认值里已经包含 WiFi / 密码 / 静态 IP，重置后依然能正常联网。 */
    if (err == ESP_OK && stored.magic == CFG_MAGIC) {
        s_cfg = stored;
        /* 旧固件存的 blob 没有门户三项，读出来是空串：用编译期默认值补上，
           这样升级固件不会把已经填好的门户地址变空 */
        if (s_cfg.portal_base[0] == '\0') {
            copy_str(s_cfg.portal_base, sizeof(s_cfg.portal_base), s_defaults.portal_base);
        }
        if (s_cfg.portal_ac_ip[0] == '\0') {
            copy_str(s_cfg.portal_ac_ip, sizeof(s_cfg.portal_ac_ip), s_defaults.portal_ac_ip);
        }
        if (s_cfg.portal_ssid[0] == '\0') {
            copy_str(s_cfg.portal_ssid, sizeof(s_cfg.portal_ssid), s_defaults.portal_ssid);
        }
        s_from_nvs = true;
        /* SSID 不进日志：日志会被诊断页、截图和 issue 带出去，SSID 里常带人名 */
        ESP_LOGI(TAG, "已从 NVS 加载配置（WiFi: %.3s***，静态IP: %s）",
                 s_cfg.wifi_ssid, s_cfg.static_ip[0] ? s_cfg.static_ip : "(DHCP)");
    } else {
        ESP_LOGI(TAG, "NVS 里没有有效配置，使用编译期默认值");
    }

    s_inited = true;
    return ESP_OK;
}

/* 价签元素开关的生效值（见头文件注释） */
uint8_t config_esl_opts(const app_settings_t *c)
{
    if (c == NULL) {
        return 0x0F;
    }
    if (c->esl_opts_valid) {
        return (uint8_t) (c->esl_opts & 0x0F);
    }
    return c->esl_opts ? (uint8_t) (c->esl_opts & 0x0F) : 0x0F;
}

const app_settings_t *config_store_get(void)
{
    return &s_cfg;
}

void config_store_snapshot(app_settings_t *out)
{
    if (out == NULL) return;
    cfg_lock();
    *out = s_cfg;
    cfg_unlock();
}

esp_err_t config_store_save(const app_settings_t *s)
{
    if (s == NULL) return ESP_ERR_INVALID_ARG;

    app_settings_t tmp = *s;
    tmp.magic = CFG_MAGIC;

    /* 长度兜底：字符串字段保证以 0 结尾 */
    tmp.wifi_ssid[CFG_SSID_MAX - 1] = '\0';
    tmp.wifi_pass[CFG_PASS_MAX - 1] = '\0';
    tmp.user[CFG_USER_MAX - 1] = '\0';
    tmp.password[CFG_PASS_MAX - 1] = '\0';
    tmp.city[CFG_TEXT_MAX - 1] = '\0';
    tmp.lat[sizeof(tmp.lat) - 1] = '\0';
    tmp.lon[sizeof(tmp.lon) - 1] = '\0';
    tmp.cal_url[CFG_URL_MAX - 1] = '\0';
    tmp.portal_base[CFG_URL_MAX - 1] = '\0';
    tmp.portal_ac_ip[sizeof(tmp.portal_ac_ip) - 1] = '\0';
    tmp.portal_ssid[sizeof(tmp.portal_ssid) - 1] = '\0';

    if (tmp.brightness > 100) tmp.brightness = 100;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(h, NVS_KEY, &tmp, sizeof(tmp));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        cfg_lock();
        s_cfg = tmp;
        s_generation++;
        cfg_unlock();
        ESP_LOGI(TAG, "配置已保存（第 %u 次修改）", (unsigned) s_generation);
    } else {
        ESP_LOGE(TAG, "配置保存失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t config_store_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_erase_key(h, NVS_KEY);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(h);
        err = ESP_OK;
        s_generation++;
        ESP_LOGW(TAG, "配置已清除，重启后回到编译期默认值");
    }
    nvs_close(h);
    return err;
}

bool config_store_from_nvs(void)
{
    return s_from_nvs;
}

uint32_t config_store_generation(void)
{
    return s_generation;
}

void config_store_mask(const char *in, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return;
    if (in == NULL) {
        out[0] = '\0';
        return;
    }

    size_t n = strlen(in);
    if (n <= 2) {
        copy_str(out, cap, "**");
        return;
    }

    if (cap < 6) {
        copy_str(out, cap, "***");
        return;
    }

    out[0] = in[0];
    for (size_t i = 1; i + 1 < n && i + 3 < cap; i++) {
        out[i] = '*';
    }
    size_t pos = n - 1;
    if (pos + 2 > cap) pos = cap - 2;
    out[pos] = in[n - 1];
    out[pos + 1] = '\0';
}