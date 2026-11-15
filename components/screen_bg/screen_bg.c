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
#define BG_BASE       0x60000      /* 模型（~0x5D890）和样本区（0xA0000）之间 */
#define BG_SLOT_SIZE  0x20000      /* 每槽 128KB：头部 4KB + 像素 115200 字节 */
#define BG_PIX_OFFSET 0x1000
#define BG_REGION     BG_SLOT_SIZE

typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint32_t bytes;
    uint32_t slot;        /* 槽位号；旧格式这里是 0 */
} bg_header_t;

static const esp_partition_t *s_part = NULL;
static bool s_slot_ok[BG_SLOTS] = { false, false };
static int s_slot = 0;
static SemaphoreHandle_t s_lock = NULL;

static inline uint32_t slot_base(int slot)
{
    if (slot < 0 || slot >= BG_SLOTS) {
        slot = 0;
    }
    return BG_BASE + (uint32_t) slot * BG_SLOT_SIZE;
}

void bg_set_slot(int slot)
{
    if (slot < 0 || slot >= BG_SLOTS) {
        slot = 0;
    }
    s_slot = slot;
    ESP_LOGI(TAG, "背景图切到槽 %d（%s）", slot, s_slot_ok[slot] ? "有图" : "空");
}

int bg_slot(void)
{
    return s_slot;
}

bool bg_has_image(void)
{
    return s_slot_ok[s_slot];
}

bool bg_has_image_at(int slot)
{
    if (slot < 0 || slot >= BG_SLOTS) {
        return false;
    }
    return s_slot_ok[slot];
}

/* 读某个槽的头部，判断里面是不是有效图片 */
static bool probe_slot(int slot)
{
    bg_header_t hdr;
    if (s_part == NULL) {
        return false;
    }
    esp_err_t err = esp_partition_read(s_part, slot_base(slot), &hdr, sizeof(hdr));
    if (err != ESP_OK) {
        return false;
    }
    return hdr.magic == BG_MAGIC && hdr.width == BG_WIDTH &&
           hdr.height == BG_HEIGHT && hdr.bytes == BG_BYTES;
}

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

    if (s_part->size < slot_base(BG_SLOTS - 1) - BG_BASE + BG_PIX_OFFSET + BG_BYTES) {
        ESP_LOGW(TAG, "storage 分区太小（%u 字节），放不下 %d 张背景",
                 (unsigned) s_part->size, BG_SLOTS);
        s_part = NULL;
        return ESP_ERR_INVALID_SIZE;
    }

    for (int i = 0; i < BG_SLOTS; i++) {
        s_slot_ok[i] = probe_slot(i);
    }
    ESP_LOGI(TAG, "背景槽：槽0 %s / 槽1 %s（分区 %u KB）",
             s_slot_ok[0] ? "有图" : "空", s_slot_ok[1] ? "有图" : "空",
             (unsigned) (s_part->size / 1024));
    return ESP_OK;
}

/* ---------------- 上传 / 擦除（按槽） ---------------- */

esp_err_t bg_upload_begin_at(int slot)
{
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;
    if (slot < 0 || slot >= BG_SLOTS) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = esp_partition_erase_range(s_part, slot_base(slot), BG_REGION);
    s_slot_ok[slot] = false;      /* 传到一半断了也不会显示半张 */
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "上传前擦除失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t bg_upload_chunk_at(int slot, const uint8_t *data, size_t len, size_t offset)
{
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    if (slot < 0 || slot >= BG_SLOTS) return ESP_ERR_INVALID_ARG;
    if (offset + len > BG_BYTES) return ESP_ERR_INVALID_SIZE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = esp_partition_write(s_part, slot_base(slot) + BG_PIX_OFFSET + offset,
                                        data, len);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t bg_upload_finish_at(int slot, size_t total)
{
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;
    if (slot < 0 || slot >= BG_SLOTS) return ESP_ERR_INVALID_ARG;
    if (total != BG_BYTES) {
        ESP_LOGW(TAG, "上传不完整：%u/%u 字节", (unsigned) total, (unsigned) BG_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    bg_header_t hdr = {
        .magic = BG_MAGIC,
        .width = BG_WIDTH,
        .height = BG_HEIGHT,
        .bytes = BG_BYTES,
        .slot = (uint32_t) slot,
    };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = esp_partition_write(s_part, slot_base(slot), &hdr, sizeof(hdr));
    if (err == ESP_OK) {
        s_slot_ok[slot] = true;
        ESP_LOGI(TAG, "背景图上传完成（槽 %d，%u 字节）", slot, (unsigned) total);
    } else {
        ESP_LOGE(TAG, "写头部失败: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t bg_erase_at(int slot)
{
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;
    if (slot < 0 || slot >= BG_SLOTS) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = esp_partition_erase_range(s_part, slot_base(slot), BG_REGION);
    if (err == ESP_OK) {
        s_slot_ok[slot] = false;
        ESP_LOGI(TAG, "背景图已删除（槽 %d）", slot);
    }
    xSemaphoreGive(s_lock);
    return err;
}

/* ---------------- 兼容旧接口：作用于当前槽 ---------------- */

esp_err_t bg_upload_begin(void)
{
    return bg_upload_begin_at(s_slot);
}

esp_err_t bg_upload_chunk(const uint8_t *data, size_t len, size_t offset)
{
    return bg_upload_chunk_at(s_slot, data, len, offset);
}

esp_err_t bg_upload_finish(size_t total)
{
    return bg_upload_finish_at(s_slot, total);
}

esp_err_t bg_erase(void)
{
    return bg_erase_at(s_slot);
}

esp_err_t bg_draw(void)
{
    if (s_part == NULL || !s_slot_ok[s_slot]) return ESP_ERR_INVALID_STATE;

    enum { BAND = 16 };
    static uint16_t band[BAND][BG_WIDTH];

    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (int y = 0; y < BG_HEIGHT; y += BAND) {
        int rows = (BG_HEIGHT - y > BAND) ? BAND : (BG_HEIGHT - y);
        size_t off = slot_base(s_slot) + BG_PIX_OFFSET + (size_t) y * BG_WIDTH * 2;
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
