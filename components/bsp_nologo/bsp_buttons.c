#include "bsp_buttons.h"
#include "board_pins.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_buttons";

static const gpio_num_t kPins[BSP_BTN_COUNT] = {
    BOARD_BTN_BOOT,
    BOARD_BTN_UP,
    BOARD_BTN_DOWN,
};

static const char *kNames[BSP_BTN_COUNT] = {"BOOT", "UP", "DOWN"};

/* 按下后要稳定的时间，抖动的按键不加消抖会连触发好几次 */
#define DEBOUNCE_MS 30
#define SCAN_MS 10

static bool s_state[BSP_BTN_COUNT];
static bsp_button_cb_t s_cb = NULL;
static void *s_user = NULL;

void bsp_buttons_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < BSP_BTN_COUNT; i++) {
        mask |= 1ULL << kPins[i];
    }

    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   /* 按键另一端接地，按下为低 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    ESP_LOGI(TAG, "按键就绪: BOOT=%d UP=%d DOWN=%d",
             BOARD_BTN_BOOT, BOARD_BTN_UP, BOARD_BTN_DOWN);
}

void bsp_buttons_on_click(bsp_button_cb_t cb, void *user)
{
    s_cb = cb;
    s_user = user;
}

bool bsp_buttons_pressed(bsp_button_t btn)
{
    if (btn >= BSP_BTN_COUNT) return false;
    return s_state[btn];
}

static void bsp_buttons_task(void *arg)
{
    (void) arg;

    /* 每个按键记录"低电平持续了多少次扫描" */
    int low_count[BSP_BTN_COUNT] = {0};
    bool stable[BSP_BTN_COUNT] = {false};

    for (;;) {
        for (int i = 0; i < BSP_BTN_COUNT; i++) {
            bool raw_low = gpio_get_level(kPins[i]) == 0;

            if (raw_low) {
                if (low_count[i] < 1000) low_count[i]++;
            } else {
                low_count[i] = 0;
            }

            bool now = low_count[i] * SCAN_MS >= DEBOUNCE_MS;

            if (now != stable[i]) {
                stable[i] = now;
                s_state[i] = now;

                if (now) {
                    ESP_LOGI(TAG, "按下 %s", kNames[i]);
                } else {
                    ESP_LOGI(TAG, "松开 %s", kNames[i]);
                    if (s_cb) {
                        s_cb((bsp_button_t) i, s_user);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SCAN_MS));
    }
}

void bsp_buttons_start(void)
{
    xTaskCreate(bsp_buttons_task, "buttons", 3072, NULL, 4, NULL);
}
