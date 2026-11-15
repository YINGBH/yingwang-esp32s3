#include "audio_level.h"
#include "board_pins.h"

#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"          /* esp_timer_get_time()：显式依赖，别靠别人间接带进来 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "audio";

/* 16kHz 单声道，每 50ms 一块 = 800 个采样 */
/* ---- 录音增益 ---- */
#define REC_AGC_ENABLE   1        /* 1=自动增益，0=固定增益 */
#define REC_FIXED_GAIN   8.0      /* 固定增益（AGC 关闭时用） */
#define REC_AGC_TARGET   3000.0   /* 目标 RMS（int16 满量程 32767） */
#define REC_AGC_MAX      24.0     /* 最大放大倍数 */
#define REC_PEAK_HOLD    8        /* 电平条峰值保持块数 */

#define SAMPLE_RATE     16000
/* 固定参考本底（校准已关闭，用它换算百分比） */
#define MIC_FIXED_FLOOR  600.0
#define WINDOW_MS       50
#define SAMPLES_PER_WIN (SAMPLE_RATE * WINDOW_MS / 1000)

/* 麦克风是 24 位数据放在 32 位帧里，右移 8 位得到 24 位量程 */
#define MIC_FULL_SCALE  ((float) (1 << 23))

/* -70dBFS 算 0%，0dBFS 算 100% */
#define DB_FLOOR  (-70.0f)

/* 自动校准：开机先静静听 1.5 秒，把这段时间的电平当"环境本底"。
   之后按相对本底的倍数显示，房间再安静也不会顶到半屏，
   有人说话/有动静立刻就能看到跳起来。 */
#define CAL_WINDOWS   30          /* 30 x 50ms = 1.5 秒 */
#define REL_FLOOR_DB  3.0f        /* 比本底高 3dB 算 0% */
#define REL_TOP_DB    33.0f       /* 比本底高 33dB 算 100% */

static double s_floor_rms = 0.0;
static bool s_floor_set = false;  /* 环境本底 */

static i2s_chan_handle_t s_rx = NULL;

/* 录音缓冲（由调用方提供） */
static int16_t *s_rec_dst = NULL;
static int      s_rec_cap = 0;
static SemaphoreHandle_t s_lock = NULL;
static audio_level_t s_level = {0, 0, -90.0f, false};

/* 平滑：新值权重 30%，避免数字乱跳 */
static int s_smooth = 0;

static volatile bool s_paused = false;   /* 暂停：任务空转，不读 I2S */

/* 只置标志（让电平任务让出 I2S），不动通道本身 —— 录音要用 */
void audio_level_hold(bool hold)
{
    s_paused = hold;
}

void audio_level_pause(bool pause)
{
    s_paused = pause;
    if (pause) {
        i2s_channel_disable(s_rx);
        ESP_LOGI(TAG, "麦克风已暂停");
    } else if (s_rx != NULL) {
        i2s_channel_enable(s_rx);
        ESP_LOGI(TAG, "麦克风已恢复");
    }
}

static void audio_task(void *arg)
{
    (void) arg;

    /* 立体声读：L/R 两个通道都看，自动挑信号大的那个
       （MEMS 麦克风的 L/R 脚决定数据落在哪个通道，接反了单声道会没声） */
    static int32_t buf[SAMPLES_PER_WIN * 2];

    while (true) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx, buf, sizeof(buf), &got,
                                         pdMS_TO_TICKS(500));
        if (err != ESP_OK || got == 0) {
            /* 读超时不算错误，麦克风没数据就是安静 */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int n = (int) (got / sizeof(int32_t));
        if (n < 2) continue;

        /* 24 位数据右移 8 位；左右声道分开算，先去掉直流偏置再算 RMS
           （不去 DC 的话，话筒的偏置会把"安静"也顶成很高的电平） */
        int frames = n / 2;
        double mean_l = 0.0, mean_r = 0.0;
        for (int i = 0; i < frames; i++) {
            mean_l += (double) (buf[2 * i] >> 8);
            mean_r += (double) (buf[2 * i + 1] >> 8);
        }
        mean_l /= frames;
        mean_r /= frames;

        double sum_l = 0.0, sum_r = 0.0;
        for (int i = 0; i < frames; i++) {
            double l = (double) (buf[2 * i] >> 8) - mean_l;
            double r = (double) (buf[2 * i + 1] >> 8) - mean_r;
            sum_l += l * l;
            sum_r += r * r;
        }
        double rms_l = sqrt(sum_l / frames);
        double rms_r = sqrt(sum_r / frames);
        double rms = (rms_l > rms_r) ? rms_l : rms_r;

        rms = (rms_l > rms_r) ? rms_l : rms_r;

        /* 校准已关闭：直接用固定参考本底（见 MIC_FIXED_FLOOR）。
           以前开机静听 1.5 秒测本底，实测常测出十几万这种离谱值，
           百分比就一直是 0%；现在不测了，开机立刻有读数。 */

        float db;
        if (rms < 1.0) {
            db = -90.0f;
        } else {
            db = (float) (20.0 * log10(rms / MIC_FULL_SCALE));
            if (db < -90.0f) db = -90.0f;
        }

        if (!s_floor_set) {
            s_floor_rms = MIC_FIXED_FLOOR;
            s_floor_set = true;
        }

        /* 相对本底的分贝（20*log10(rms/floor)），映射成 0~100% */
        double rel = (rms > 1.0) ? (20.0 * log10(rms / s_floor_rms)) : -60.0;
        int pct = (int) ((rel - REL_FLOOR_DB) / (REL_TOP_DB - REL_FLOOR_DB) * 100.0);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_smooth = (s_smooth * 7 + pct * 3) / 10;
        s_level.percent = s_smooth;
        s_level.db = db;
        s_level.ready = true;
        if (s_smooth > s_level.peak_percent) {
            s_level.peak_percent = s_smooth;
        }
        xSemaphoreGive(s_lock);
    }
}

esp_err_t audio_level_start(void)
{
    /* 幂等：已经启动过就直接返回。
       以前每调一次就新建互斥锁 + I2S 通道 + 任务，
       第二次调用会出现两套音频任务抢同一个 DMA，日志反复刷屏。 */
    if (s_level.ready && s_rx != NULL) {
        /* 通道可能被 audio_level_pause() 关过。
           现在诊断页不再开麦、录音前会调 start，如果这里直接返回，
           通道就一直处于关闭状态，读 I2S 永远超时（MICRAW 实测过）。 */
        /* 已使能时 enable 会返回 INVALID_STATE，属正常，直接返回成功 */
        esp_err_t e = i2s_channel_enable(s_rx);
        if (e == ESP_OK || e == ESP_ERR_INVALID_STATE) {
            s_paused = false;
            s_level.ready = true;
            return ESP_OK;
        }
        ESP_LOGE(TAG, "使能 I2S 失败: %s（保留通道，不重建）", esp_err_to_name(e));
        return e;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 256;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S 通道创建失败: %s", esp_err_to_name(err));
        s_rx = NULL;
        s_level.ready = false;
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_MIC_SCK,
            .ws = BOARD_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = BOARD_MIC_DIN,
            .invert_flags = {false, false, false},
        },
    };

    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S 标准模式初始化失败: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }

    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S 使能失败: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx);
        s_rx = NULL;
        s_level.ready = false;
        return err;
    }

    s_level.ready = true;
    xTaskCreate(audio_task, "audio_level", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "麦克风就绪 %dHz 单声道 (WS=%d SCK=%d DIN=%d)",
             SAMPLE_RATE, BOARD_MIC_WS, BOARD_MIC_SCK, BOARD_MIC_DIN);
    return ESP_OK;
}

void audio_level_get(audio_level_t *out)
{
    if (out == NULL) return;
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        out->db = -90.0f;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_level;
    xSemaphoreGive(s_lock);
}

void audio_level_reset_peak(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_level.peak_percent = s_level.percent;
    xSemaphoreGive(s_lock);
}

/* ---------------------------------------------------------------- 录音
 * 复用同一路 I2S RX（16kHz / 32bit / 立体声）：取左声道高 16 位，转成 mono int16。
 * 录音期间把电平任务暂停，避免两个读者抢 DMA。
 */
static volatile bool s_rec_run = false;
static volatile int  s_rec_count = 0;
static volatile bool s_rec_done = false;
static double s_rec_gain = 0.0;
static volatile int32_t s_rec_peak = 0;
static volatile int s_rec_peak_age = 0;

static void audio_rec_task(void *arg)
{
    (void) arg;
    static int32_t buf[512 * 2];
    while (s_rec_run) {
        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx, buf, sizeof(buf), &got, pdMS_TO_TICKS(200));
        if (err != ESP_OK || got == 0) {
            continue;
        }
        int frames = (int) (got / (sizeof(int32_t) * 2));

        /* ---- 自动增益（AGC）----
           麦克风原始电平很低（RMS 几百），直接存下来回放几乎听不见。
           这里按块估 RMS，把增益往"目标电平"上拉，并做平滑，
           最后硬限幅避免爆音。 */
        if (REC_AGC_ENABLE && frames > 0) {
            double sum = 0.0;
            for (int i = 0; i < frames; i++) {
                double v = (double) (buf[i * 2] >> 16);
                sum += v * v;
            }
            double rms = sqrt(sum / frames);
            if (rms < 1.0) {
                rms = 1.0;
            }
            /* 目标 RMS：约 -20dBFS，留足余量 */
            double want = REC_AGC_TARGET / rms;
            if (want > REC_AGC_MAX) {
                want = REC_AGC_MAX;
            }
            if (want < 1.0) {
                want = 1.0;          /* 声音够大就不放大，只允许不缩 */
            }
            /* 平滑：新块只占 30%，避免"忽大忽小"的呼吸感 */
            if (s_rec_gain < 1.0) {
                s_rec_gain = want;                       /* 首块直接到位 */
            } else {
                s_rec_gain = s_rec_gain * 0.7 + want * 0.3;
            }
        }

        for (int i = 0; i < frames; i++) {
            if (s_rec_count >= s_rec_cap) {
                s_rec_run = false;
                break;
            }
            /* 左声道 32bit，取高 16 位，再乘增益 */
            int32_t v = buf[i * 2] >> 16;
            int32_t out = (int32_t) ((double) v * s_rec_gain);
            if (out > 32767) {
                out = 32767;
            } else if (out < -32768) {
                out = -32768;
            }
            s_rec_dst[s_rec_count++] = (int16_t) out;
            /* 顺便更新"最近电平"给界面画条 */
            int32_t a = out < 0 ? -out : out;
            if (a > s_rec_peak) {
                s_rec_peak = a;
            }
        }
        s_rec_peak_age++;
    }
    s_rec_done = true;
    vTaskDelete(NULL);
}

esp_err_t audio_rec_start(int16_t *dst, int cap_samples)
{
    if (dst == NULL || cap_samples <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 麦克风只在诊断页才会初始化；录音前如果还没初始化就先建好 */
    if (s_rx == NULL) {
        esp_err_t e = audio_level_start();
        if (e != ESP_OK) {
            ESP_LOGE("audio", "初始化麦克风失败: %s", esp_err_to_name(e));
            return e;
        }
    }
    if (s_rec_run) {
        return ESP_ERR_INVALID_STATE;
    }
    s_rec_dst = dst;
    s_rec_cap = cap_samples;
    s_rec_count = 0;
    s_rec_done = false;
    s_rec_run = true;
    s_rec_gain = REC_AGC_ENABLE ? 0.0 : REC_FIXED_GAIN;
    s_rec_peak = 0;
    s_rec_peak_age = REC_PEAK_HOLD;
    /* 录音前强制复位通道状态：disable 再 enable */
    i2s_channel_disable(s_rx);
    i2s_channel_enable(s_rx);
    audio_level_hold(true);      /* 让电平任务让出 I2S，但保持通道使能 */
    if (xTaskCreate(audio_rec_task, "audio_rec", 3072, NULL, 5, NULL) != pdPASS) {
        s_rec_run = false;
        audio_level_hold(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* 停止录音，返回录到的采样数 */
int audio_rec_stop(void)
{
    if (!s_rec_run && s_rec_done) {
        int n = s_rec_count;
        s_rec_done = false;
        audio_level_hold(false);
        return n;
    }
    s_rec_run = false;
    /* 等任务退出（最多 1 秒） */
    for (int i = 0; i < 50 && !s_rec_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_rec_done = false;
    audio_level_hold(false);
    return s_rec_count;
}

bool audio_rec_active(void)
{
    return s_rec_run;
}


/* 录音时的实时电平（0-100，取最近若干块的峰值，带保持） */
int audio_rec_level(void)
{
    if (s_rec_peak_age > REC_PEAK_HOLD) {
        return 0;
    }
    /* 应用增益后满量程按 32767 算 */
    int pct = (int) ((int64_t) s_rec_peak * 100 / 32767);
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    return pct;
}

/* 当前录音增益（调试用） */
float audio_rec_gain(void)
{
    return (float) s_rec_gain;
}


/* 调试用：抓 0.5 秒原始采样，统计 min/max/RMS，并给出前 8 个样本。
   用来判断麦克风硬件是否在工作（说话时 max 应该明显变化）。 */
esp_err_t audio_level_probe(int32_t *out_min, int32_t *out_max,
                            double *out_rms, int32_t *samples, int n_samples)
{
    if (s_rx == NULL) {
        esp_err_t e = audio_level_start();
        if (e != ESP_OK) {
            return e;
        }
    }
    /* 关键：先把电平任务停住，否则两个任务同时读一个 I2S 通道，
       会互相抢 DMA，表现为 TIMEOUT / INVALID_STATE（实测过）。 */
    audio_level_hold(true);
    vTaskDelay(pdMS_TO_TICKS(60));      /* 等它让出 */

    /* 强制把通道拉到"已使能"：
       先 disable（清状态，已关闭时会返回 INVALID_STATE，忽略）
       再 enable（此时必定成功）。
       之前只调 enable 救不回来——它返回 INVALID_STATE 时无法判断
       到底是"已使能"还是"未初始化"，读就一直失败。 */
    i2s_channel_disable(s_rx);
    esp_err_t een = i2s_channel_enable(s_rx);
    if (een != ESP_OK) {
        ESP_LOGE(TAG, "使能 I2S 失败: %s", esp_err_to_name(een));
        return een;
    }
    s_paused = false;

    static int32_t buf[256 * 2];
    int32_t vmin = 32767, vmax = -32768;
    double sum = 0.0;
    int total = 0, taken = 0;
    int errcnt = 0;

    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < 500000) {      /* 0.5 秒 */
        size_t got = 0;
        esp_err_t rerr = i2s_channel_read(s_rx, buf, sizeof(buf), &got, pdMS_TO_TICKS(100));
        if (rerr != ESP_OK) {
            if (errcnt < 3) {
                errcnt++;
                ESP_LOGW(TAG, "MICRAW 读 I2S 失败: %s（got=%u）",
                         esp_err_to_name(rerr), (unsigned) got);
            }
            continue;
        }
        int frames = (int) (got / (sizeof(int32_t) * 2));
        for (int i = 0; i < frames; i++) {
            int32_t v = buf[i * 2] >> 16;
            if (v < vmin) vmin = v;
            if (v > vmax) vmax = v;
            sum += (double) v * v;
            total++;
            if (samples != NULL && taken < n_samples) {
                samples[taken++] = v;
            }
        }
    }
    audio_level_hold(false);           /* 恢复电平任务 */

    if (total == 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (out_min) *out_min = vmin;
    if (out_max) *out_max = vmax;
    if (out_rms) *out_rms = sqrt(sum / total);
    return ESP_OK;
}


