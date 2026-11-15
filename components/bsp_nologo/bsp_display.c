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
