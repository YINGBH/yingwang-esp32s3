#include "beep.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "beep";

#define BEEP_BCLK  15
#define BEEP_LRCK  16
#define BEEP_DOUT  7
#define BEEP_RATE  44100
#define BEEP_FREQ  2093.0f      /* C7 */
#define BEEP_MS    100          /* 每声 100ms */
#define BEEP_GAP   140          /* 每两声之间 */
#define BEEP_CYCLE 1500         /* 响三声停一下，再继续，直到有人按停 */
/* 没人管的时候别无限响：约 2.8 分钟后自动停，省电也免得吵 */
#define BEEP_MAX_CYCLES 80

static i2s_chan_handle_t s_tx = NULL;
static volatile bool s_stop = false;
static bool s_tick_mute = false;
static volatile bool s_active = false;
static int s_vol = 80;

/* ---------------------------------------------------------------- 音量 */

static void vol_load(void)
{
    nvs_handle_t h;
    if (nvs_open("beep", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v = 80;
    if (nvs_get_u8(h, "vol", &v) == ESP_OK && v <= 100) {
        s_vol = v;
    }
    nvs_close(h);
}

void beep_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_vol = percent;

    nvs_handle_t h;
    if (nvs_open("beep", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "vol", (uint8_t) percent);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "闹钟音量 %d%%", percent);
}

int beep_volume(void)
{
    return s_vol;
}

/* ---------------------------------------------------------------- I2S */

static bool ensure_init(void)
{
    if (s_tx != NULL) {
        return true;
    }
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    esp_err_t err = i2s_new_channel(&cc, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        s_tx = NULL;
        return false;
    }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BEEP_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BEEP_BCLK,
            .ws = BEEP_LRCK,
            .dout = BEEP_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {false, false, false},
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &sc);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_tx);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S 初始化失败: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return false;
    }
    return true;
}

static void beep_once(int16_t *chunk, int total, float *phase)
{
    const float dphase = 2.0f * (float) M_PI * BEEP_FREQ / (float) BEEP_RATE;
    float amp = 30000.0f * (float) s_vol / 100.0f;
    if (amp < 1.0f) {
        amp = 1.0f;
    }

    int done = 0;
    while (done < total && !s_stop) {
        int n = total - done;
        if (n > 256) {
            n = 256;
        }
        for (int i = 0; i < n; i++) {
            int16_t v = (int16_t) (amp * sinf(*phase));
            *phase += dphase;
            if (*phase > 6.2831853f) {
                *phase -= 6.2831853f;
            }
            chunk[i * 2] = v;
            chunk[i * 2 + 1] = v;
        }
        size_t w = 0;
        i2s_channel_write(s_tx, chunk, (size_t) n * 4, &w, pdMS_TO_TICKS(500));
        done += n;
    }
}

static void beep_task(void *arg)
{
    (void) arg;
    s_active = true;
    s_stop = false;

    if (!ensure_init()) {
        s_active = false;
        vTaskDelete(NULL);
        return;
    }

    int16_t chunk[256 * 2];
    float phase = 0.0f;
    const int total = BEEP_RATE * BEEP_MS / 1000;

    int cycles = 0;
    while (!s_stop) {
        for (int k = 0; k < 3 && !s_stop; k++) {
            beep_once(chunk, total, &phase);
            if (k < 2) {
                vTaskDelay(pdMS_TO_TICKS(BEEP_GAP));
            }
        }
        cycles++;
        if (cycles >= BEEP_MAX_CYCLES) {
            ESP_LOGW(TAG, "闹钟响了 %d 轮没人按停，自动关闭", cycles);
            break;
        }
        if (!s_stop) {
            vTaskDelay(pdMS_TO_TICKS(BEEP_CYCLE));
        }
    }

    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = NULL;
    s_active = false;
    vTaskDelete(NULL);
}

/* 按键提示音：20ms 短音。用独立小任务播放，播完立刻退出释放 I2S，
   不常驻，避免和闹钟/喇叭抢资源 */
static void tick_task(void *arg)
{
    (void) arg;
    if (beep_alarm_active()) {          /* 闹钟在响就别插嘴 */
        vTaskDelete(NULL);
        return;
    }
    if (!ensure_init()) {
        vTaskDelete(NULL);
        return;
    }
    const int total = BEEP_RATE * 20 / 1000;
    float amp = 12000.0f * (float) s_vol / 100.0f;
    if (amp < 800.0f) {
        amp = 800.0f;
    }
    const float dphase = 2.0f * (float) M_PI * 2000.0f / (float) BEEP_RATE;
    float phase = 0.0f;
    static int16_t chunk[64 * 2];
    int done = 0;
    while (done < total) {
        int n = total - done;
        if (n > 64) {
            n = 64;
        }
        for (int i = 0; i < n; i++) {
            int16_t v = (int16_t) (amp * sinf(phase));
            phase += dphase;
            if (phase > 6.2831853f) {
                phase -= 6.2831853f;
            }
            chunk[i * 2] = v;
            chunk[i * 2 + 1] = v;
        }
        size_t w = 0;
        i2s_channel_write(s_tx, chunk, (size_t) n * 4, &w, pdMS_TO_TICKS(60));
        done += n;
    }
    if (s_tx != NULL) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    vTaskDelete(NULL);
}

void beep_set_tick_mute(bool mute)
{
    s_tick_mute = mute;
}

bool beep_tick_muted(void)
{
    return s_tick_mute;
}

void beep_tick(void)
{
    if (s_tick_mute) {
        return;
    }
    if (beep_alarm_active()) {
        return;
    }
    static bool vol_loaded2 = false;
    if (!vol_loaded2) {
        vol_loaded2 = true;
        vol_load();
    }
    xTaskCreate(tick_task, "beep_tick", 3072, NULL, 2, NULL);
}

/* 播放一段单声道 PCM（16kHz int16）。阻塞调用，播完返回。
   用于语音备忘回放；和闹钟共用同一条 I2S TX，正在响铃时会直接返回。 */
void beep_play_pcm(const int16_t *pcm, int samples)
{
    if (pcm == NULL || samples <= 0 || s_active) {
        return;
    }
    static bool vol_loaded3 = false;
    if (!vol_loaded3) {
        vol_loaded3 = true;
        vol_load();
    }
    if (!ensure_init()) {
        return;
    }
    static int16_t chunk[512 * 2];
    float gain = (float) s_vol / 100.0f;
    if (gain > 1.0f) {
        gain = 1.0f;
    }
    int done = 0;
    while (done < samples) {
        int n = samples - done;
        if (n > 512) {
            n = 512;
        }
        for (int i = 0; i < n; i++) {
            int16_t v = (int16_t) ((float) pcm[done + i] * gain);
            chunk[i * 2] = v;
            chunk[i * 2 + 1] = v;
        }
        size_t w = 0;
        i2s_channel_write(s_tx, chunk, (size_t) n * 4, &w, pdMS_TO_TICKS(500));
        done += n;
    }
    if (s_tx != NULL) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
}

void beep_alarm_start(void)
{
    if (s_active) {
        return;
    }
    static bool vol_loaded = false;
    if (!vol_loaded) {
        vol_loaded = true;
        vol_load();
    }
    xTaskCreate(beep_task, "beep", 3072, NULL, 3, NULL);
}

void beep_alarm_stop(void)
{
    s_stop = true;
}

bool beep_alarm_active(void)
{
    return s_active;
}
