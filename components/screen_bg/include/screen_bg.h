/*
 * 屏幕背景图
 *
 * 板子不解码图片 —— RAM 太紧张，也没必要。
 * 浏览器把图裁成 240x240 并转成 RGB565 原始数据传上来，
 * 这里只负责把 115200 字节存进 flash 分区，显示时逐行读出来刷屏。
 *
 * 存哪儿：partitions.csv 里那个 4MB 的 storage 分区（本来就是空的）。
 * 不用文件系统，直接裸写分区，省掉挂载 SPIFFS 的开销。
 */
#ifndef _SCREEN_BG_H_
#define _SCREEN_BG_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/** 背景图尺寸，固定 240x240 */
#define BG_WIDTH  240
#define BG_HEIGHT 240
#define BG_BYTES  (BG_WIDTH * BG_HEIGHT * 2)

/** 初始化：找到分区、校验有没有存过图 */
esp_err_t bg_init(void);

/** 有没有可用的背景图 */
bool bg_has_image(void);

/**
 * 保存背景图（RGB565 小端，115200 字节）。
 * 数据直接来自网页上传，调用方保证长度正确。
 */
/**
 * 分块上传：网页传 115200 字节，不可能一次读进内存，
 * 所以擦除一次、边收边写，全部收齐才算数（头部最后写）。
 */
esp_err_t bg_upload_begin(void);
esp_err_t bg_upload_chunk(const uint8_t *data, size_t len, size_t offset);
esp_err_t bg_upload_finish(size_t total);

/** 删除背景图 */
esp_err_t bg_erase(void);

/** 把背景图画到屏幕上（从 flash 逐行读，不用大缓冲） */
esp_err_t bg_draw(void);


/* ---- 多槽位（两张背景图轮换）---- */
#define BG_SLOTS 2

/** 当前用哪一槽（0..BG_SLOTS-1） */
void bg_set_slot(int slot);

/** 取当前槽位 */
int bg_slot(void);

/** 指定槽位有没有图 */
bool bg_has_image_at(int slot);

/** 上传/擦除指定槽位（默认接口内部用当前槽） */
esp_err_t bg_upload_begin_at(int slot);
esp_err_t bg_upload_chunk_at(int slot, const uint8_t *data, size_t len, size_t offset);
esp_err_t bg_upload_finish_at(int slot, size_t total);
esp_err_t bg_erase_at(int slot);

#endif  // _SCREEN_BG_H_