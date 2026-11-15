/*
 * 配置持久化（NVS）
 *
 * 之前 WiFi 密码和宽带账号是编译期常量，改一次就得重新编译烧录。
 * 现在存到 NVS：网页后台能直接改，掉电不丢，改完不用接电脑。
 *
 * 首次开机 NVS 是空的，用传进来的编译期默认值填充。
 */
#ifndef _CONFIG_STORE_H_
#define _CONFIG_STORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define CFG_SSID_MAX 33
#define CFG_PASS_MAX 65
#define CFG_USER_MAX 33
#define CFG_TEXT_MAX 32
#define CFG_URL_MAX  128

typedef struct {
    uint32_t magic;           /* 校验值放最前：以后加字段不会让旧配置失效 */
    char     wifi_ssid[CFG_SSID_MAX];
    char     wifi_pass[CFG_PASS_MAX];
    char     user[CFG_USER_MAX];
    char     password[CFG_PASS_MAX];
    char     lat[16];
    char     lon[16];
    char     city[CFG_TEXT_MAX];
    char     cal_url[CFG_URL_MAX];  /* 日历数据地址，{year} 占位 */
    uint8_t  brightness;      /* 背光 0-100 */
    uint8_t  screen_off_min;  /* 无操作多少分钟后息屏，0=常亮（复用原来的保留字节） */
    uint16_t birthday;        /* 生日 mm*100+dd（如 1031 = 10月31日），0=不设置 */
    char     static_ip[32];    /* 静态 IP，空=用 DHCP */
    uint16_t rot;              /* 屏幕方向 0/90/180/270 */

    /* ── 校园网门户（新加，放在末尾保证旧 blob 前缀不变）──────
       空字符串 = 用编译期默认值（app_config.h 里的 APP_PORTAL_*） */
    char     portal_base[CFG_URL_MAX];   /* 门户地址，如 http://10.0.0.1:8090 */
    char     portal_ac_ip[48];           /* AC IP（wlanacip） */
    char     portal_ssid[48];            /* 校园网 SSID（大小写敏感） */

    /* ── 熄屏策略（末尾追加，旧 blob 读出来是 0 = 老行为）──────
       charge_off : 插着电时也允许息屏（1=允许；0=插电常亮）
       off_bright : 息屏后背光；0=全关+清屏，1..100=低亮度常显（AOD，减轻残影） */
    uint8_t  charge_off;
    uint8_t  off_bright;

    /* ── 主题（末尾追加；旧 blob 读出来全是 0 = 深蓝默认主题）──────
       theme_id   : 内置主题序号 0..BSP_THEME_COUNT-1；0xFF = 自定义
       theme_bg   : 自定义背景色（RGB565）
       theme_fg_  : 保留 */
    uint8_t  theme_id;
    uint16_t theme_bg;
    uint16_t theme_accent;

    /* ── 装饰层（二次元定制）：边框 / 角标图标 / 角标文字 ────── */
    uint8_t  deco_border;     /* 0 无 1 细线 2 圆角 3 双线 */
    uint8_t  deco_icon;       /* 0 无 1 星星 2 爱心 3 音符 4 樱花 5 月亮 */
    char     deco_tag[16];    /* 左下角小字，空 = 不显示 */
    uint8_t  deco_grid;       /* 1 = 画淡网格背景 */

    /* ── 自动化：主题昼夜切换 / 背景图轮换（末尾追加）────── */
    uint8_t  theme_auto;      /* 1 = 按日出日落在 theme_day/theme_night 之间切 */
    uint8_t  theme_day;       /* 白天主题序号 */
    uint8_t  theme_night;     /* 夜里主题序号 */
    uint8_t  bg_rotate;       /* 0 关 / 1 每小时换 / 2 每天换 */

    /* ── 墨水屏界面（末尾追加）──────
       esl_layout : 0 标准（日历+天气） / 1 大字时间
       esl_opts   : bit0 日历 bit1 天气 bit2 日出日落 bit3 品牌版本
       0 = 旧配置读出来（用默认全开） */
    uint8_t  esl_layout;
    uint8_t  esl_opts;
    char     esl_text[24];    /* 自定义一行字 */
} app_settings_t;

/**
 * 初始化并加载配置。
 * @param defaults 编译期默认值（app_config.h），NVS 为空时使用
 */
esp_err_t config_store_init(const app_settings_t *defaults);

/** 取当前配置（只读指针，直接指向内部存储） */
const app_settings_t *config_store_get(void);

/**
 * 拷贝一份配置快照。
 * 网页线程和主线程会同时访问，用这个拿副本，别长期持有内部指针。
 */
void config_store_snapshot(app_settings_t *out);

/** 修改并保存。返回 ESP_OK 表示已写入 NVS */
esp_err_t config_store_save(const app_settings_t *s);

/** 恢复出厂设置（清掉 NVS 里的配置，下次开机用编译期默认值） */
esp_err_t config_store_reset(void);

/**
 * 这份配置是不是从 NVS 读出来的（而不是编译期默认值）。
 * 开机连不上 WiFi 时，主程序据此决定要不要退回默认配置救场。
 */
bool config_store_from_nvs(void);

/** 配置被改过几次（主程序用来判断要不要重连 WiFi） */
uint32_t config_store_generation(void);

/** 打码显示密码，日志和网页里用：abcdef → a****f */
void config_store_mask(const char *in, char *out, size_t cap);

#endif  // _CONFIG_STORE_H_