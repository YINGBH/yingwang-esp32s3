#include "esp_attr.h"
#include "screen_bg.h"

#include <string.h>

#include "bsp_display.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bg";

#define BG_MAGIC      0x554E4942   /* "UNIB" */
#define BG_BASE       0x60000      /* 背景图放模型和样本区之间的空档：模型 ~0x5D890，暂存区 0xA0000 起 */
#define BG_PIX_OFFSET (BG_BASE + 0x1000)  /* 像素数据放 4KB 处，头部单独占一页 */
#define BG_REGION     0x20000      /* 抹除范围 128KB，够 115200 字节 */

typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint32_t bytes;
    uint32_t reserved;
} bg_header_t;

static const esp_partition_t *s_part = NULL;
static bool s_has_image = false;
static SemaphoreHandle_t s_lock = NULL;

esp_err_t bg_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (s_part == NULL) {
        ESP_LOGW(TAG, "没有 storage 分区，自定义背景不可用");
        return ESP_ERR_NOT_FOUND;
    }

    if (s_part->size < BG_PIX_OFFSET + BG_BYTES) {
        ESP_LOGW(TAG, "storage 分区太小（%u 字节），放不下背景图",
                 (unsigned) s_part->size);
        s_part = NULL;
        return ESP_ERR_INVALID_SIZE;
    }

    bg_header_t hdr;
    esp_err_t err = esp_partition_read(s_part, BG_BASE, &hdr, sizeof(hdr));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读分区失败: %s", esp_err_to_name(err));
        return err;
    }

    if (hdr.magic != BG_MAGIC || hdr.width != BG_WIDTH ||
        hdr.height != BG_HEIGHT || hdr.bytes != BG_BYTES) {
        ESP_LOGI(TAG, "没有自定义背景（分区 %u KB 空闲）",
                 (unsigned) (s_part->size / 1024));
        s_has_image = false;
        return ESP_OK;
    }

    s_has_image = true;
    ESP_LOGI(TAG, "已加载自定义背景 %dx%d", hdr.width, hdr.height);
    return ESP_OK;
}

bool bg_has_image(void)
{
    return s_has_image;
}



esp_err_t bg_upload_begin(void)
{
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = esp_partition_erase_range(s_part, BG_BASE, BG_REGION);
    /* 先标记成"没有图"：传到一半断了也不会显示半张 */
    s_has_image = false;
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "上传前擦除失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t bg_upload_chunk(const uint8_t *data, size_t len, size_t offset)
{
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    if (offset + len > BG_BYTES) return ESP_ERR_INVALID_SIZE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = esp_partition_write(s_part, BG_PIX_OFFSET + offset, data, len);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t bg_upload_finish(size_t total)
{
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;
    if (total != BG_BYTES) {
        ESP_LOGW(TAG, "上传不完整：%u/%u 字节", (unsigned) total, (unsigned) BG_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    bg_header_t hdr = {
        .magic = BG_MAGIC,
        .width = BG_WIDTH,
        .height = BG_HEIGHT,
        .bytes = BG_BYTES,
        .reserved = 0,
    };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = esp_partition_write(s_part, BG_BASE, &hdr, sizeof(hdr));
    if (err == ESP_OK) {
        s_has_image = true;
        ESP_LOGI(TAG, "背景图上传完成（%u 字节）", (unsigned) total);
    } else {
        ESP_LOGE(TAG, "写头部失败: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t bg_erase(void)
{
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = esp_partition_erase_range(s_part, BG_BASE, BG_REGION);
    if (err == ESP_OK) {
        s_has_image = false;
        ESP_LOGI(TAG, "背景图已删除");
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t bg_draw(void)
{
    if (s_part == NULL || !s_has_image) return ESP_ERR_INVALID_STATE;

    /* 一次读 16 行 = 7680 字节，够快又不占内存 */
    enum { BAND = 16 };
    static uint16_t band[BAND][BG_WIDTH];

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (int y = 0; y < BG_HEIGHT; y += BAND) {
        int rows = (BG_HEIGHT - y > BAND) ? BAND : (BG_HEIGHT - y);
        size_t off = BG_PIX_OFFSET + (size_t) y * BG_WIDTH * 2;
        esp_err_t err = esp_partition_read(s_part, off, band, (size_t) rows * BG_WIDTH * 2);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "读背景失败: %s", esp_err_to_name(err));
            xSemaphoreGive(s_lock);
            return err;
        }
        bsp_display_blit(0, y, BG_WIDTH, rows, (const uint16_t *) band);
    }

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

