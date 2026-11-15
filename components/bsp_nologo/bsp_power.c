#include "bsp_power.h"
#include "board_pins.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_power";

static bool s_held = false;

void bsp_power_hold(void)
{
    if (s_held) return;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_POWER_HOLD,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    /* 拉高锁住供电，之后松手也不会掉电 */
    gpio_set_level(BOARD_POWER_HOLD, 1);
    s_held = true;
    ESP_LOGI(TAG, "电源自锁已拉高 (GPIO%d)", BOARD_POWER_HOLD);

    /* 充电检测脚 */
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << BOARD_CHARGE_DETECT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);
}

void bsp_power_off(void)
{
    if (!s_held) return;
    ESP_LOGW(TAG, "松开电源自锁，准备断电");
    vTaskDelay(pdMS_TO_TICKS(100));  /* 让日志先吐出去 */
    gpio_set_level(BOARD_POWER_HOLD, 0);
    s_held = false;
    /* 电源已经断了，不会走到这里 */
    vTaskDelay(pdMS_TO_TICKS(200));
}

bool bsp_power_is_charging(void)
{
    return gpio_get_level(BOARD_CHARGE_DETECT) != 0;
}