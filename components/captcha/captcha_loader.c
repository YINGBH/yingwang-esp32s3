/*
 * 从 storage 分区（0xC10000 起，见 partitions.csv）加载量化模型
 *
 * 模型不再编进 app，而是烧到独立的 storage 分区，运行时用
 * esp_partition_mmap() 直接映射（写时复制，不占 RAM），
 * app 体积因此再省 374 KB。
 *
 * 包格式（tools/captcha_train/quantize_model.py 生成）：
 *   0   魔数 "ESL1"
 *   4   张量个数 (u32) —— 固定 8
 *   8   每项: 元素个数 (u32) + 缩放系数 (f32)
 *   对齐后是原始 int16 数据
 */
#include "captcha_loader.h"

#include "esp_rom_crc.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "captcha_mdl";

/* 权重指针（推理代码直接用这些名字） */
const int16_t *CM_W1 = NULL, *CM_B1 = NULL, *CM_W2 = NULL, *CM_B2 = NULL;
const int16_t *CM_W3 = NULL, *CM_B3 = NULL, *CM_W4 = NULL, *CM_B4 = NULL;

float CM_W1_S, CM_B1_S, CM_W2_S, CM_B2_S, CM_W3_S, CM_B3_S, CM_W4_S, CM_B4_S;

static bool s_loaded = false;
static esp_err_t s_err = ESP_ERR_INVALID_STATE;
static uint32_t s_params = 0;
static uint32_t s_bytes = 0;
static uint32_t s_crc = 0;

#define MODEL_MAGIC 0x314C5345u          /* "ESL1" 小端 */

esp_err_t captcha_model_load(void)
{
    if (s_loaded) {
        return s_err;
    }
    s_loaded = true;

    const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (part == NULL) {
        ESP_LOGE(TAG, "找不到 storage 分区");
        s_err = ESP_ERR_NOT_FOUND;
        return s_err;
    }

    const void *base = NULL;
    esp_partition_mmap_handle_t h;
    esp_err_t err = esp_partition_mmap(part, 0, 4096, ESP_PARTITION_MMAP_DATA,
                                       &base, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap 失败 %s", esp_err_to_name(err));
        s_err = err;
        return s_err;
    }

    const uint8_t *p = (const uint8_t *) base;
    uint32_t magic;
    memcpy(&magic, p, 4);
    if (magic != MODEL_MAGIC) {
        ESP_LOGE(TAG, "分区里没有模型（魔数 %08x）——先烧 model_q.bin", magic);
        esp_partition_munmap(h);
        s_err = ESP_ERR_INVALID_VERSION;
        return s_err;
    }

    /* 头部之后是数据；映射整段（数据在 4 KB 之外，需要再映射一次） */
    esp_partition_munmap(h);
    size_t total = part->size;
    err = esp_partition_mmap(part, 0, total, ESP_PARTITION_MMAP_DATA, &base, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap 整段失败 %s", esp_err_to_name(err));
        s_err = err;
        return s_err;
    }
    p = (const uint8_t *) base;

    uint32_t n = 0;
    memcpy(&n, p + 4, 4);
    if (n != 8) {
        ESP_LOGE(TAG, "张量个数不对：%u", (unsigned) n);
        esp_partition_munmap(h);        /* 不然这块映射就漏了 */
        s_err = ESP_ERR_INVALID_SIZE;
        return s_err;
    }

    uint32_t counts[8];
    float scales[8];
    for (uint32_t i = 0; i < 8; i++) {
        memcpy(&counts[i], p + 8 + i * 8, 4);
        memcpy(&scales[i], p + 8 + i * 8 + 4, 4);
    }
    /* 先按头部声明算总大小，确认分区装得下再设指针：
       否则分区里是残缺/伪造的模型时，推理会读到分区外面去 */
    uint32_t total_elems = 0;
    for (uint32_t i = 0; i < 8; i++) {
        total_elems += counts[i];          /* counts 是元素个数，不是字节数 */
    }
    size_t need = 8 + 8 * 8 + (size_t) total_elems * 2;
    if (need > part->size) {
        ESP_LOGE(TAG, "模型头声明 %u 字节，分区只有 %u 字节，拒绝加载",
                 (unsigned) need, (unsigned) part->size);
        esp_partition_munmap(h);
        s_err = ESP_ERR_INVALID_SIZE;
        return s_err;
    }

    const int16_t *data = (const int16_t *) (p + 8 + 8 * 8);
    /* 对齐到 8 字节 */
    data = (const int16_t *) (((uintptr_t) data + 7) & ~(uintptr_t) 7);

    /* counts 是"元素个数"（int16 个数），不是字节数！
       以前这里写成 counts[i] / 2，每个张量只读了一半、后面全部错位，
       推理结果等于垃圾 —— 这是 OCR 一直不准的根因 */
    CM_W1 = data;           data += counts[0];   CM_W1_S = scales[0];
    CM_B1 = data;           data += counts[1];   CM_B1_S = scales[1];
    CM_W2 = data;           data += counts[2];   CM_W2_S = scales[2];
    CM_B2 = data;           data += counts[3];   CM_B2_S = scales[3];
    CM_W3 = data;           data += counts[4];   CM_W3_S = scales[4];
    CM_B3 = data;           data += counts[5];   CM_B3_S = scales[5];
    CM_W4 = data;           data += counts[6];   CM_W4_S = scales[6];
    CM_B4 = data;                                CM_B4_S = scales[7];

    s_params = total_elems;
    s_bytes = need;
    s_crc = esp_rom_crc32_le(0, p, 8 + 8 * 8);

    ESP_LOGI(TAG, "模型已映射：%u 个参数，w3 缩放 %.3e",
             (unsigned) s_params, CM_W3_S);
    s_err = ESP_OK;
    return ESP_OK;
}

bool captcha_model_ready(void)
{
    return s_err == ESP_OK;
}

void captcha_model_get_info(captcha_model_info_t *out)
{
    if (out == NULL) {
        return;
    }
    out->ready = (s_err == ESP_OK);
    out->params = s_params;
    out->bytes = s_bytes;
    out->crc = s_crc;
    out->tensors = 8;
}

size_t captcha_model_size(void)
{
    /* 没加载过就先加载一次（内部有 s_loaded 缓存） */
    if (!s_loaded) {
        captcha_model_load();
    }
    return (s_err == ESP_OK) ? s_bytes : 0;
}
