#include "bsp_display.h"
#include "board_pins.h"
#include "font_data.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bsp_display";

/* ================================================================ 主题 */

/* 内置主题：前 4 套是深色系，第 5 套是浅色（纸感）。
   命名参考常见的编辑器/终端配色，方便"闭眼选" */
static const bsp_theme_t THEMES[BSP_THEME_COUNT] = {
    /* 0 · 深蓝（默认，原配色） */
    { .bg = 0x1082, .fg = 0xFFFF, .dim = 0x8410, .accent = 0xFFE0,
      .ok = 0x07E0, .warn = 0xF800, .info = 0x07FF, .panel = 0x2104 },
    /* 1 · 纯黑（OLED 风，最省眼） */
    { .bg = 0x0000, .fg = 0xFFFF, .dim = 0x7BEF, .accent = 0x07FF,
      .ok = 0x07E0, .warn = 0xF800, .info = 0x5D9F, .panel = 0x18E3 },
    /* 2 · 森林绿 */
    { .bg = RGB565(8, 26, 18), .fg = RGB565(230, 255, 235), .dim = RGB565(110, 150, 120),
      .accent = RGB565(120, 230, 140), .ok = RGB565(80, 220, 120), .warn = RGB565(255, 90, 90),
      .info = RGB565(140, 220, 200), .panel = RGB565(18, 46, 32) },
    /* 3 · 紫罗兰 */
    { .bg = RGB565(28, 18, 46), .fg = RGB565(245, 235, 255), .dim = RGB565(150, 130, 180),
      .accent = RGB565(255, 140, 255), .ok = RGB565(120, 240, 180), .warn = RGB565(255, 100, 110),
      .info = RGB565(150, 190, 255), .panel = RGB565(48, 32, 72) },
    /* 4 · 暖白（纸感，浅底深字；适合白天/强光） */
    { .bg = RGB565(250, 246, 236), .fg = RGB565(20, 20, 24), .dim = RGB565(120, 115, 105),
      .accent = RGB565(200, 60, 40), .ok = RGB565(20, 130, 70), .warn = RGB565(200, 30, 30),
      .info = RGB565(30, 90, 170), .panel = RGB565(228, 222, 208) },

    /* ---- 二次元配色（取自角色印象色，只借色不借素材）---- */
    /* 5 · 初音未来：葱绿 */
    { .bg = RGB565(10, 20, 24), .fg = RGB565(235, 255, 250), .dim = RGB565(110, 150, 145),
      .accent = RGB565(57, 197, 187), .ok = RGB565(120, 230, 190), .warn = RGB565(255, 100, 120),
      .info = RGB565(150, 220, 240), .panel = RGB565(20, 42, 46) },
    /* 6 · 洛天依：天依蓝 */
    { .bg = RGB565(12, 20, 40), .fg = RGB565(235, 245, 255), .dim = RGB565(120, 145, 180),
      .accent = RGB565(102, 204, 255), .ok = RGB565(120, 230, 200), .warn = RGB565(255, 110, 140),
      .info = RGB565(140, 190, 255), .panel = RGB565(22, 34, 62) },
    /* 7 · 鹿乃：樱粉 */
    { .bg = RGB565(40, 20, 30), .fg = RGB565(255, 240, 246), .dim = RGB565(180, 140, 160),
      .accent = RGB565(255, 158, 196), .ok = RGB565(255, 200, 220), .warn = RGB565(255, 80, 100),
      .info = RGB565(200, 170, 255), .panel = RGB565(62, 30, 44) },
    /* 8 · 爱莉希雅：粉紫 */
    { .bg = RGB565(28, 18, 40), .fg = RGB565(250, 240, 255), .dim = RGB565(160, 140, 185),
      .accent = RGB565(201, 160, 255), .ok = RGB565(150, 240, 210), .warn = RGB565(255, 120, 150),
      .info = RGB565(255, 190, 230), .panel = RGB565(48, 32, 66) },
    /* 9 · 蕾姆：冰蓝 */
    { .bg = RGB565(12, 18, 30), .fg = RGB565(235, 248, 255), .dim = RGB565(130, 155, 180),
      .accent = RGB565(126, 200, 227), .ok = RGB565(140, 240, 220), .warn = RGB565(255, 120, 130),
      .info = RGB565(170, 215, 255), .panel = RGB565(22, 32, 50) },
    /* 10 · 赤红：热血 */
    { .bg = RGB565(26, 12, 14), .fg = RGB565(255, 238, 238), .dim = RGB565(180, 130, 130),
      .accent = RGB565(255, 77, 109), .ok = RGB565(255, 200, 140), .warn = RGB565(255, 60, 60),
      .info = RGB565(255, 170, 150), .panel = RGB565(48, 20, 24) },
};

static const char *THEME_NAMES[BSP_THEME_COUNT] = {
    "深蓝", "纯黑", "森林", "紫罗兰", "暖白",
    "初音未来", "洛天依", "鹿乃", "爱莉希雅", "蕾姆", "赤红",
};

bsp_theme_t g_theme = {
    .bg = 0x1082, .fg = 0xFFFF, .dim = 0x8410, .accent = 0xFFE0,
    .ok = 0x07E0, .warn = 0xF800, .info = 0x07FF, .panel = 0x2104,
};

static int s_theme_idx = 0;

const char *bsp_theme_name(int idx)
{
    if (idx < 0 || idx >= BSP_THEME_COUNT) {
        return NULL;
    }
    return THEME_NAMES[idx];
}

void bsp_theme_load_preset(int idx)
{
    if (idx < 0 || idx >= BSP_THEME_COUNT) {
        return;
    }
    g_theme = THEMES[idx];
    s_theme_idx = idx;
    ESP_LOGI(TAG, "主题：%s（%d）", THEME_NAMES[idx], idx);
}

/* RGB565 亮度（0-255），用来判断浅底/深底 */
static int luma565(uint16_t c)
{
    int r = (c >> 11) & 0x1F;
    int g = (c >> 5) & 0x3F;
    int b = c & 0x1F;
    return (r * 8 + g * 4 + b * 8) / 3;
}

void bsp_theme_custom(uint16_t bg, uint16_t accent)
{
    bool light = luma565(bg) > 150;
    g_theme.bg = bg;
    g_theme.accent = accent;
    if (light) {
        g_theme.fg = RGB565(20, 20, 24);
        g_theme.dim = RGB565(120, 115, 105);
        g_theme.panel = RGB565(228, 222, 208);
        g_theme.ok = RGB565(20, 130, 70);
        g_theme.warn = RGB565(200, 30, 30);
        g_theme.info = RGB565(30, 90, 170);
    } else {
        g_theme.fg = 0xFFFF;
        g_theme.dim = 0x8410;
        g_theme.panel = 0x2104;
        g_theme.ok = 0x07E0;
        g_theme.warn = 0xF800;
        g_theme.info = 0x07FF;
    }
    s_theme_idx = -1;
    ESP_LOGI(TAG, "自定义主题：bg=0x%04X accent=0x%04X（%s底）", bg, accent, light ? "浅" : "深");
}

int bsp_theme_current(void)
{
    return s_theme_idx;
}

void bsp_theme_ensure_readable(bool has_background)
{
    if (!has_background) {
        return;
    }
    /* 背景图一般是照片/插画，明暗不可控；主文字和次要文字一律用亮色，
       强调色/状态色保留，主题仍然能看出来 */
    g_theme.fg = 0xFFFF;
    g_theme.dim = 0xCE79;
    ESP_LOGI(TAG, "有背景图：文字强制亮色（主题 %d）", s_theme_idx);
}


/* 字形缓冲上限：16x16 的字放大 2 倍是 32x32，留到 48 够用 */
#define GLYPH_BUF_MAX 48

static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_fb = NULL;   /* 240x240 RGB565 帧缓冲（PSRAM） */
static bool s_ready = false;

/* 显示层是共享资源：界面任务和验证码回调都会画，
   必须串行化，否则画面会花。用递归锁，允许嵌套调用。 */
static SemaphoreHandle_t s_lock = NULL;

/* 屏幕要求 RGB565 高位在前，而 uint16_t 缓冲是低字节在前 —— 必须逐像素交换 */
static inline void swap16_buf(uint16_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint16_t v = p[i];
        p[i] = (uint16_t) ((v >> 8) | (v << 8));
    }
}

static SemaphoreHandle_t s_flush_done;
volatile uint32_t g_flush_ok, g_flush_timeout;

/* 一次颜色传输完成回调：唤醒等待者 */
static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata,
                                void *ctx)
{
    BaseType_t woken = pdFALSE;
    if (s_flush_done) xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

/* 绘制并等待真正发完 —— 复用静态缓冲时必须这么做，否则 DMA 读到新数据会花屏 */
static void draw_sync(int x1, int y1, int x2, int y2, const void *data)
{
    if (s_flush_done) xSemaphoreTake(s_flush_done, 0);
    esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, data);
    if (s_flush_done) {
        if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(300)) == pdTRUE)
            g_flush_ok++;
        else
            g_flush_timeout++;
    }
}

static void display_lock(void)
{
    if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static void display_unlock(void)
{
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

/* ------------------------------------------------------------------ 背光 */

static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

void bsp_display_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    uint32_t duty = (1023 * percent) / 100;
    if (BOARD_LCD_BL_INVERT) {
        duty = 1023 - duty;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ------------------------------------------------------------------ 初始化 */


/* ---------------- 屏幕方向 ---------------- */
static int s_rotation = 0;
static int s_gap_adj_x = 0;
static int s_gap_adj_y = 0;

void bsp_display_set_rotation(int deg)
{
    if (s_panel == NULL) {
        return;
    }
    /* ST7789 的显存是 240x320，而屏幕可见区只有 240x240。
       不设 gap 时 0 度看起来正常（可见区正好在显存开头），
       但 swap_xy 之后坐标轴交换，可见区会在 320 那条轴上错位，
       表现为画面被裁掉或偏出屏幕。所以每个方向都要配套设 gap。 */
    int gap_x = 0;
    int gap_y = 0;
    switch (deg) {
    case 90:
        esp_lcd_panel_swap_xy(s_panel, true);
        esp_lcd_panel_mirror(s_panel, false, true);
        gap_x = 80;          /* 沿交换后的行方向补 320-240 */
        break;
    case 180:
        esp_lcd_panel_swap_xy(s_panel, false);
        esp_lcd_panel_mirror(s_panel, true, true);
        break;
    case 270:
        esp_lcd_panel_swap_xy(s_panel, true);
        esp_lcd_panel_mirror(s_panel, true, false);
        gap_y = 80;
        break;
    case 0:
    default:
        esp_lcd_panel_swap_xy(s_panel, false);
        esp_lcd_panel_mirror(s_panel, false, false);
        deg = 0;
        break;
    }
    /* 允许通过 SET rotgap=x,y 现场微调（存扩展设置） */
    esp_lcd_panel_set_gap(s_panel, gap_x + s_gap_adj_x, gap_y + s_gap_adj_y);
    s_rotation = deg;
    ESP_LOGW(TAG, "屏幕方向: %d 度", deg);
}


/* 现场微调显存偏移：旋转后画面偏了就用它修 */
void bsp_display_set_gap_adj(int x, int y)
{
    s_gap_adj_x = x;
    s_gap_adj_y = y;
    bsp_display_set_rotation(s_rotation);   /* 重新应用 */
    ESP_LOGW(TAG, "显存偏移微调: x=%d y=%d", x, y);
}

int bsp_display_get_rotation(void)
{
    return s_rotation;
}

esp_err_t bsp_display_init(void)
{
    if (s_ready) return ESP_OK;

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = BOARD_LCD_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = BOARD_LCD_WIDTH * 40 * sizeof(uint16_t),
    };
    if (s_flush_done == NULL) {
        s_flush_done = xSemaphoreCreateBinary();
    }

    esp_err_t err = spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI 初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SPI3 就绪 MOSI=%d SCLK=%d", BOARD_LCD_MOSI, BOARD_LCD_SCLK);

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = BOARD_LCD_DC,
        /* ST7789 必须 SPI 模式 3（官方 xiaozhi 配置一致） */
        .spi_mode = 3,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t) BOARD_LCD_SPI_HOST,
                                   &io_config, &io);
    if (err == ESP_OK) {
        esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_color_trans_done };
        esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel io 创建失败: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io, &panel_config, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 创建失败: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    /* ST7789 的 240x240 屏普遍要开反色，否则白底变黑底 */
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_fb = heap_caps_malloc(BOARD_LCD_WIDTH * BOARD_LCD_HEIGHT * sizeof(uint16_t),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_fb == NULL) {
        s_fb = heap_caps_malloc(BOARD_LCD_WIDTH * BOARD_LCD_HEIGHT * sizeof(uint16_t),
                                MALLOC_CAP_8BIT);
    }
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "帧缓冲分配失败，屏幕无法显示");
    } else {
        memset(s_fb, 0, BOARD_LCD_WIDTH * BOARD_LCD_HEIGHT * sizeof(uint16_t));
        ESP_LOGI(TAG, "帧缓冲 %d KB", (int) (BOARD_LCD_WIDTH * BOARD_LCD_HEIGHT * 2 / 1024));
    }

    backlight_init();
    bsp_display_backlight(80);

    s_ready = true;
    ESP_LOGI(TAG, "ST7789 %dx%d 就绪", BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT);
    return ESP_OK;
}

uint16_t bsp_display_width(void) { return BOARD_LCD_WIDTH; }
uint16_t bsp_display_height(void) { return BOARD_LCD_HEIGHT; }

/* ------------------------------------------------------------------ 绘制 */

static void clamp_rect(int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > BOARD_LCD_WIDTH) *w = BOARD_LCD_WIDTH - *x;
    if (*y + *h > BOARD_LCD_HEIGHT) *h = BOARD_LCD_HEIGHT - *y;
    if (*w <= 0 || *h <= 0) { *w = 0; *h = 0; }
}

/* 把帧缓冲分带刷到屏幕：每带 16 行，边拷贝边交换字节 */
void bsp_display_flush(void)
{
    if (!s_ready || s_fb == NULL) return;
    display_lock();

    enum { BAND = 16 };
    static uint16_t band[BAND][BOARD_LCD_WIDTH];

    for (int y = 0; y < BOARD_LCD_HEIGHT; y += BAND) {
        int rows = (BOARD_LCD_HEIGHT - y > BAND) ? BAND : (BOARD_LCD_HEIGHT - y);
        for (int r = 0; r < rows; r++) {
            const uint16_t *src = s_fb + (size_t) (y + r) * BOARD_LCD_WIDTH;
            uint16_t *dst = band[r];
            for (int c = 0; c < BOARD_LCD_WIDTH; c++) {
                uint16_t v = src[c];
                dst[c] = (uint16_t) ((v >> 8) | (v << 8));
            }
        }
        draw_sync(0, y, BOARD_LCD_WIDTH, y + rows, band[0]);
    }
    display_unlock();
}

void bsp_display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_ready || s_fb == NULL) return;
    display_lock();
    clamp_rect(&x, &y, &w, &h);
    if (w == 0 || h == 0) { display_unlock(); return; }

    for (int r = 0; r < h; r++) {
        uint16_t *p = s_fb + (size_t) (y + r) * BOARD_LCD_WIDTH + x;
        for (int c = 0; c < w; c++) p[c] = color;
    }
    display_unlock();
}

void bsp_display_clear(uint16_t color)
{
    bsp_display_fill_rect(0, 0, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, color);
}

void bsp_display_rect(int x, int y, int w, int h, uint16_t color)
{
    bsp_display_fill_rect(x, y, w, 1, color);
    bsp_display_fill_rect(x, y + h - 1, w, 1, color);
    bsp_display_fill_rect(x, y, 1, h, color);
    bsp_display_fill_rect(x + w - 1, y, 1, h, color);
}

/* --------------------------------------------------------------- 文本渲染 */

static int utf8_decode(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *) s;
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && p[1]) {
        *cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        *cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        *cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
              ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *cp = '?';
    return 1;
}

static int cjk_index(uint32_t cp)
{
    const char *p = font_cjk_chars;
    int idx = 0;
    while (*p) {
        uint32_t cur;
        int n = utf8_decode(p, &cur);
        if (cur == cp) return idx;
        p += n;
        idx++;
    }
    return -1;
}

static const uint16_t *glyph_for(uint32_t cp, int *w, int *h)
{
    if (cp >= FONT_ASCII_FIRST && cp < FONT_ASCII_FIRST + FONT_ASCII_COUNT) {
        int i = (int) (cp - FONT_ASCII_FIRST);
        *w = FONT_ASCII_W;
        *h = FONT_ASCII_H;
        return &font_ascii_bits[i * FONT_ASCII_H];
    }
    int i = cjk_index(cp);
    if (i >= 0) {
        *w = FONT_CJK_W;
        *h = FONT_CJK_H;
        return &font_cjk_bits[i * FONT_CJK_H];
    }
    return NULL;
}

int bsp_display_text_width(const char *utf8, uint8_t scale)
{
    if (scale < 1) scale = 1;
    int total = 0;
    const char *p = utf8;
    while (*p) {
        uint32_t cp;
        p += utf8_decode(p, &cp);
        int w, h;
        if (glyph_for(cp, &w, &h)) total += w * scale;
    }
    return total;
}

int bsp_display_text(int x, int y, const char *utf8, uint16_t fg, int bg, uint8_t scale)
{
    if (!s_ready || s_fb == NULL) return x;
    if (scale < 1) scale = 1;
    display_lock();

    /* bg < 0：只画字，不碰背景像素（叠在壁纸上的透明文字用这个） */
    bool fill_bg = (bg >= 0);
    uint16_t back = fill_bg ? (uint16_t) bg : 0;
    int start_x = x;

    const char *p = utf8;
    while (*p) {
        uint32_t cp;
        p += utf8_decode(p, &cp);

        int gw, gh;
        const uint16_t *bits = glyph_for(cp, &gw, &gh);
        if (!bits) {
            /* 字库里没有这个字：退回画一个 '?'，别静默留空白
               （静默跳过会出现"文字显示不全"的错觉） */
            bits = glyph_for('?', &gw, &gh);
            if (!bits) {
                x += FONT_ASCII_W * scale;
                continue;
            }
        }

        for (int gr = 0; gr < gh; gr++) {
            uint16_t row = (uint16_t) (bits[gr] << (16 - gw));
            int py0 = y + gr * scale;
            for (int gc = 0; gc < gw; gc++) {
                bool on = (row << gc) & 0x8000;
                if (!on && !fill_bg) {
                    continue;                   /* 透明底：背景像素保持原样 */
                }
                uint16_t c = on ? fg : back;
                int px0 = x + gc * scale;
                for (int sy = 0; sy < scale; sy++) {
                    int py = py0 + sy;
                    if (py < 0 || py >= BOARD_LCD_HEIGHT) continue;
                    uint16_t *line = s_fb + (size_t) py * BOARD_LCD_WIDTH;
                    for (int sx = 0; sx < scale; sx++) {
                        int px = px0 + sx;
                        if (px >= 0 && px < BOARD_LCD_WIDTH) line[px] = c;
                    }
                }
            }
        }
        x += gw * scale;
    }
    display_unlock();
    return start_x + (x - start_x);
}

void bsp_display_text_center(int y, const char *utf8, uint16_t fg, int bg, uint8_t scale)
{
    int w = bsp_display_text_width(utf8, scale);
    int x = (BOARD_LCD_WIDTH - w) / 2;
    if (x < 0) x = 0;
    bsp_display_text(x, y, utf8, fg, bg, scale);
}

/* 画 RGB888 图片（可选反色），按 scale 放大，写进帧缓冲 */
void bsp_display_image(int x, int y, int w, int h, const uint8_t *rgb888,
                       uint8_t scale, bool invert)
{
    if (!s_ready || s_fb == NULL || rgb888 == NULL) return;
    if (scale < 1) scale = 1;
    display_lock();

    for (int sy = 0; sy < h; sy++) {
        for (int sx = 0; sx < w; sx++) {
            const uint8_t *p = rgb888 + (sy * w + sx) * 3;
            uint8_t cr = p[0], cg = p[1], cb = p[2];
            if (invert) { cr = 255 - cr; cg = 255 - cg; cb = 255 - cb; }
            uint16_t c = (uint16_t) (((cr & 0xF8) << 8) | ((cg & 0xFC) << 3) | (cb >> 3));
            for (int dy = 0; dy < scale; dy++) {
                int py = y + sy * scale + dy;
                if (py < 0 || py >= BOARD_LCD_HEIGHT) continue;
                uint16_t *line = s_fb + (size_t) py * BOARD_LCD_WIDTH;
                for (int dx = 0; dx < scale; dx++) {
                    int px = x + sx * scale + dx;
                    if (px >= 0 && px < BOARD_LCD_WIDTH) line[px] = c;
                }
            }
        }
    }
    display_unlock();
}

/* 把一整块 RGB565 写进帧缓冲（背景图用） */
void bsp_display_blit(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (!s_ready || s_fb == NULL || pixels == NULL || w <= 0 || h <= 0) return;
    display_lock();
    for (int r = 0; r < h; r++) {
        int py = y + r;
        if (py < 0 || py >= BOARD_LCD_HEIGHT) continue;
        uint16_t *dst = s_fb + (size_t) py * BOARD_LCD_WIDTH;
        const uint16_t *src = pixels + (size_t) r * w;
        for (int c = 0; c < w; c++) {
            int px = x + c;
            if (px >= 0 && px < BOARD_LCD_WIDTH) dst[px] = src[c];
        }
    }
    display_unlock();
}
/* 调试用：把帧缓冲交出去（240x240 RGB565，小端） */
const uint16_t *bsp_display_framebuffer(void)
{
    return s_fb;
}

/* ================================================================ 装饰层 */

static struct {
    uint8_t border;
    uint8_t icon;
    char    tag[16];
} s_deco = { BSP_DECO_BORDER_NONE, BSP_DECO_ICON_NONE, "" };

void bsp_deco_set(uint8_t border, uint8_t icon, const char *tag)
{
    s_deco.border = (border > BSP_DECO_BORDER_DOUBLE) ? BSP_DECO_BORDER_NONE : border;
    s_deco.icon = (icon > BSP_DECO_ICON_MOON) ? BSP_DECO_ICON_NONE : icon;
    s_deco.tag[0] = '\0';
    if (tag) {
        strlcpy(s_deco.tag, tag, sizeof(s_deco.tag));
    }
}

/* 角标小图标：16x16 像素图（1 = 画） */
static void draw_icon16(int x, int y, int which, uint16_t color)
{
    static const uint16_t STAR[16] = {
        0x0000, 0x0180, 0x0180, 0x03C0, 0x1FF8, 0x0FF0, 0x07E0, 0x0FF0,
        0x0FF0, 0x1FF8, 0x381C, 0x300C, 0x6006, 0x0000, 0x0000, 0x0000,
    };
    static const uint16_t HEART[16] = {
        0x0000, 0x0C30, 0x1E78, 0x3FFC, 0x3FFC, 0x3FFC, 0x1FF8, 0x0FF0,
        0x07E0, 0x03C0, 0x0180, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    };
    static const uint16_t NOTE[16] = {
        0x0000, 0x0060, 0x0070, 0x0068, 0x0064, 0x0064, 0x0064, 0x00E4,
        0x01C4, 0x0180, 0x0180, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    };
    static const uint16_t SAKURA[16] = {
        0x0000, 0x0180, 0x03C0, 0x0FF0, 0x1FF8, 0x3FFC, 0x3FFC, 0x1FF8,
        0x0FF0, 0x07E0, 0x0C30, 0x1818, 0x0000, 0x0000, 0x0000, 0x0000,
    };
    static const uint16_t MOON[16] = {
        0x0000, 0x0F80, 0x3E00, 0x7C00, 0x7800, 0xF000, 0xF000, 0xF000,
        0xF000, 0xF000, 0x7800, 0x7C00, 0x3E00, 0x0F80, 0x0000, 0x0000,
    };
    const uint16_t *bm = NULL;
    switch (which) {
    case BSP_DECO_ICON_STAR:   bm = STAR;   break;
    case BSP_DECO_ICON_HEART:  bm = HEART;  break;
    case BSP_DECO_ICON_NOTE:   bm = NOTE;   break;
    case BSP_DECO_ICON_SAKURA: bm = SAKURA; break;
    case BSP_DECO_ICON_MOON:   bm = MOON;   break;
    default: return;
    }
    for (int r = 0; r < 16; r++) {
        uint16_t row = bm[r];
        for (int c = 0; c < 16; c++) {
            if (row & (0x8000 >> c)) {
                bsp_display_fill_rect(x + c, y + r, 1, 1, color);
            }
        }
    }
}

static bool s_grid = false;

void bsp_deco_set_grid(bool on)
{
    s_grid = on;
}

void bsp_deco_draw_grid(void)
{
    if (!s_grid) {
        return;
    }
    /* 24px 网格，用次要文字色画 1px 线：比 panel 色明显，又不抢内容 */
    for (int x = 0; x < 240; x += 24) {
        bsp_display_fill_rect(x, 0, 1, 240, TH_DIM);
    }
    for (int y = 0; y < 240; y += 24) {
        bsp_display_fill_rect(0, y, 240, 1, TH_DIM);
    }
}

void bsp_deco_draw(void)
{
    const int W = 240, H = 240;

    /* 边框 */
    if (s_deco.border == BSP_DECO_BORDER_THIN) {
        bsp_display_rect(2, 2, W - 4, H - 4, TH_ACCENT);
    } else if (s_deco.border == BSP_DECO_BORDER_DOUBLE) {
        bsp_display_rect(2, 2, W - 4, H - 4, TH_ACCENT);
        bsp_display_rect(5, 5, W - 10, H - 10, TH_ACCENT);
    } else if (s_deco.border == BSP_DECO_BORDER_ROUND) {
        /* 圆角：四条直边 + 四个 1/4 圆弧（半径 16，用整数近似圆） */
        const int r = 16, m = 2;
        bsp_display_fill_rect(m + r, m, W - 2 * (m + r), 2, TH_ACCENT);
        bsp_display_fill_rect(m + r, H - m - 2, W - 2 * (m + r), 2, TH_ACCENT);
        bsp_display_fill_rect(m, m + r, 2, H - 2 * (m + r), TH_ACCENT);
        bsp_display_fill_rect(W - m - 2, m + r, 2, H - 2 * (m + r), TH_ACCENT);
        for (int i = 0; i <= r; i++) {
            /* j = r - sqrt(r² - i²)，用整数牛顿法算整数平方根 */
            int v = r * r - i * i;
            int j = r;
            while (j > 0 && j * j > v) {
                j--;
            }
            j = r - j;
            if (j < 0) {
                j = 0;
            }
            bsp_display_fill_rect(m + i, m + j, 1, 2, TH_ACCENT);
            bsp_display_fill_rect(W - m - i - 1, m + j, 1, 2, TH_ACCENT);
            bsp_display_fill_rect(m + i, H - m - j - 2, 1, 2, TH_ACCENT);
            bsp_display_fill_rect(W - m - i - 1, H - m - j - 2, 1, 2, TH_ACCENT);
        }
    }

    /* 右上角小图标（避开标题栏，从 y=30 开始） */
    if (s_deco.icon != BSP_DECO_ICON_NONE) {
        draw_icon16(W - 24, 30, s_deco.icon, TH_ACCENT);
    }

    /* 左下角文字 */
    if (s_deco.tag[0]) {
        int tw = bsp_display_text_width(s_deco.tag, 1);
        bsp_display_text(W - 8 - tw, H - 22, s_deco.tag, TH_ACCENT, -1, 1);
    }
}
