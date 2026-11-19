#include "flog.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

static const char *TAG = "flog";

/* 日志环：放在 storage 分区 0x80000 起。
   前面 0x0 是验证码模型（~383KB），0x60000 是背景图（128KB），
   0x80000 往后的 3 MB 以前完全空着，现在全部给日志。 */
#define FLOG_BASE       0x80000         /* storage 分区内偏移 */
#define FLOG_PAGE_SIZE  0x1000
#define FLOG_PAGES      864             /* (0x3E0000-0x80000)/0x1000 = 3.375 MB */
#define FLOG_MAGIC      0x474F4C32u     /* 'FL2'：换值让旧环数据自动失效 */
#define FLOG_HDR        16
#define FLOG_STAGE_MAX  3072
#define FLOG_REC_MAX    2048
/* 单次 LOGF 最多回传 96 页（384KB，约 7600 行）：和以前一样大。
   环本身有 3.375 MB（约 68000 行），只是不回传那么多 ——
   一次回传 3 MB 会把串口占死十几分钟，界面也会卡住。 */
#define FLOG_READ_PAGES 864

static const esp_partition_t *s_part = NULL;
static volatile int s_page = -1;        /* 当前写页（-1 = 还没开始） */
static size_t s_off = 0;                /* 当前页内写位置（含页头） */
static uint32_t s_seq = 0;
static char *s_stage = NULL;
static size_t s_stage_len = 0;
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_task = NULL;
static bool s_ready = false;

/* ---------------- 内部：写一页的页头 ---------------- */
static void write_page_header(int page)
{
    uint8_t hdr[FLOG_HDR];
    memset(hdr, 0, sizeof(hdr));
    uint32_t magic = FLOG_MAGIC, seq = ++s_seq;
    memcpy(hdr + 0, &magic, 4);
    memcpy(hdr + 4, &seq, 4);
    esp_partition_write(s_part, (size_t) FLOG_BASE + (size_t) page * FLOG_PAGE_SIZE,
                        hdr, sizeof(hdr));
    s_off = FLOG_HDR;
}

/* 切到下一页（擦除后写页头） */
static bool next_page(void)
{
    int p = (s_page < 0) ? 0 : ((s_page + 1) % FLOG_PAGES);
    esp_err_t err = esp_partition_erase_range(
        s_part, (size_t) FLOG_BASE + (size_t) p * FLOG_PAGE_SIZE, FLOG_PAGE_SIZE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "擦除第 %d 页失败: %s", p, esp_err_to_name(err));
        return false;
    }
    s_page = p;
    write_page_header(p);
    return true;
}

/* 把 stage 里的内容写进 flash（尽量塞进当前页，塞不下就换页） */
static void flush_stage(void)
{
    if (!s_ready || s_stage_len == 0) {
        return;
    }
    /* 第一次写入：先把第 0 页擦干净并写好页头，
       否则 s_page = -1 会让偏移算到分区外面去 */
    if (s_page < 0) {
        if (!next_page()) {
            return;
        }
    }
    size_t written = 0;
    while (written < s_stage_len) {
        size_t room = FLOG_PAGE_SIZE - s_off;
        size_t need_hdr = 2;
        if (room < need_hdr + 1) {
            if (!next_page()) {
                break;
            }
            room = FLOG_PAGE_SIZE - s_off;
        }
        size_t chunk = s_stage_len - written;
        size_t max_data = room - need_hdr;
        if (chunk > max_data) {
            chunk = max_data;
        }
        if (chunk > FLOG_REC_MAX) {
            chunk = FLOG_REC_MAX;
        }
        if (chunk == 0) {
            if (!next_page()) {
                break;
            }
            continue;
        }
        uint8_t rec[FLOG_REC_MAX + 2];
        uint16_t len = (uint16_t) chunk;
        memcpy(rec, &len, 2);
        memcpy(rec + 2, s_stage + written, chunk);
        esp_err_t err = esp_partition_write(
            s_part, (size_t) FLOG_BASE + (size_t) s_page * FLOG_PAGE_SIZE + s_off,
            rec, chunk + 2);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "写日志失败: %s", esp_err_to_name(err));
            break;
        }
        s_off += chunk + 2;
        written += chunk;
    }
    if (written > 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memmove(s_stage, s_stage + written, s_stage_len - written);
        s_stage_len -= written;
        xSemaphoreGive(s_lock);
    }
}

static void flog_task(void *arg)
{
    (void) arg;
    /* 开机先等 30 秒：启动期日志量最大，这时候写 flash 最容易和
       WiFi 初始化抢资源（之前就卡死过一次） */
    vTaskDelay(pdMS_TO_TICKS(30000));
    int64_t last_flush = esp_timer_get_time() / 1000;
    while (true) {
        /* 每 10 秒看一眼：攒够 2KB 或满 60 秒才落盘。
           原来固定 20 秒一次，一天 4300 多次写（含擦页），对闪存寿命不划算；
           日志少的时候没必要那么勤。 */
        vTaskDelay(pdMS_TO_TICKS(10000));
        bool need = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        need = (s_stage_len >= 2048);
        xSemaphoreGive(s_lock);
        int64_t now = esp_timer_get_time() / 1000;
        if (need || (now - last_flush >= 60000)) {
            bool has;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            has = (s_stage_len > 0);
            xSemaphoreGive(s_lock);
            if (has) {
                flush_stage();
            }
            last_flush = now;
        }
    }
}

void flog_start(void)
{
    if (s_ready) {
        return;
    }
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (s_part == NULL || s_part->size < (FLOG_BASE + FLOG_PAGES * FLOG_PAGE_SIZE)) {
        ESP_LOGW(TAG, "storage 分区不够，flash 日志不可用");
        return;
    }
    s_stage = heap_caps_malloc(FLOG_STAGE_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_stage == NULL) {
        ESP_LOGW(TAG, "缓冲区分配失败，flash 日志不可用");
        return;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return;
        }
    }
    s_ready = true;

    /* 找出上次写到哪一页：扫描所有页头，取 seq 最大的一页作为"当前页" */
    uint32_t best_seq = 0;
    int best_page = -1;
    for (int p = 0; p < FLOG_PAGES; p++) {
        uint8_t hdr[FLOG_HDR];
        if (esp_partition_read(s_part, (size_t) FLOG_BASE + (size_t) p * FLOG_PAGE_SIZE,
                               hdr, sizeof(hdr)) != ESP_OK) {
            continue;
        }
        uint32_t magic, seq;
        memcpy(&magic, hdr, 4);
        memcpy(&seq, hdr + 4, 4);
        if (magic == FLOG_MAGIC && seq >= best_seq) {
            best_seq = seq;
            best_page = p;
        }
    }
    if (best_page >= 0) {
        s_page = best_page;
        s_seq = best_seq;
        /* 找到该页最后一个有效记录的末尾，接着写 */
        size_t off = FLOG_HDR;
        uint8_t rec[FLOG_REC_MAX + 2];
        while (off + 2 <= FLOG_PAGE_SIZE) {
            uint16_t len = 0;
            if (esp_partition_read(s_part, (size_t) FLOG_BASE + (size_t) s_page * FLOG_PAGE_SIZE + off,
                                   &len, 2) != ESP_OK) {
                break;
            }
            if (len == 0 || len == 0xFFFF || off + 2 + len > FLOG_PAGE_SIZE) {
                break;
            }
            (void) rec;
            off += 2 + len;
        }
        s_off = off;
        ESP_LOGI(TAG, "flash 日志续写：第 %d 页，offset %u，seq %u",
                 s_page, (unsigned) s_off, (unsigned) s_seq);
    } else {
        s_page = -1;
        s_seq = 0;
        ESP_LOGI(TAG, "flash 日志首次使用，共 %d 页 × 4KB = %d KB",
                 FLOG_PAGES, FLOG_PAGES * 4);
    }

    xTaskCreate(flog_task, "flog", 4096, NULL, 1, &s_task);
}

void flog_append(const char *text, size_t len)
{
    if (!s_ready || text == NULL || len == 0 || s_stage == NULL) {
        return;
    }
    if (len > FLOG_REC_MAX) {
        len = FLOG_REC_MAX;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_stage_len + len > FLOG_STAGE_MAX) {
        /* 缓冲满了：丢掉最旧的，保证新日志能进来 */
        size_t drop = s_stage_len + len - FLOG_STAGE_MAX;
        if (drop > s_stage_len) {
            drop = s_stage_len;
        }
        memmove(s_stage, s_stage + drop, s_stage_len - drop);
        s_stage_len -= drop;
    }
    memcpy(s_stage + s_stage_len, text, len);
    s_stage_len += len;
    xSemaphoreGive(s_lock);
}

size_t flog_tail(char *dst, size_t dst_size, int n)
{
    if (!s_ready || dst == NULL || dst_size < 2) {
        if (dst && dst_size) {
            dst[0] = '\0';
        }
        return 0;
    }
    if (n <= 0) {
        n = 300;
    }

    /* 先把所有有效页的 seq 收集起来，按 seq 排序，取最后 FLOG_READ_PAGES 页 */
    typedef struct { uint32_t seq; int page; } pinfo_t;
    /* 注意：不能开在栈上。环扩到 864 页后这个数组要 6.9KB，
       而调用它的 USB CLI 任务栈只有 8KB，会直接爆栈
       （现象是 spinlock_acquire 断言失败）。放 PSRAM。 */
    pinfo_t *info = heap_caps_malloc((size_t) FLOG_PAGES * sizeof(pinfo_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (info == NULL) {
        info = heap_caps_malloc((size_t) FLOG_PAGES * sizeof(pinfo_t), MALLOC_CAP_8BIT);
    }
    if (info == NULL) {
        if (dst_size) {
            dst[0] = '\0';
        }
        return 0;
    }
    int cnt = 0;
    for (int p = 0; p < FLOG_PAGES; p++) {
        uint8_t hdr[FLOG_HDR];
        if (esp_partition_read(s_part, (size_t) FLOG_BASE + (size_t) p * FLOG_PAGE_SIZE,
                               hdr, sizeof(hdr)) != ESP_OK) {
            continue;
        }
        uint32_t magic, seq;
        memcpy(&magic, hdr, 4);
        memcpy(&seq, hdr + 4, 4);
        if (magic == FLOG_MAGIC) {
            info[cnt].seq = seq;
            info[cnt].page = p;
            cnt++;
        }
    }
    if (cnt == 0) {
        free(info);
        if (dst_size) {
            dst[0] = '\0';
        }
        return 0;
    }
    /* 简单插入排序（最多 95 个） */
    for (int i = 1; i < cnt; i++) {
        pinfo_t key = info[i];
        int j = i - 1;
        while (j >= 0 && info[j].seq > key.seq) {
            info[j + 1] = info[j];
            j--;
        }
        info[j + 1] = key;
    }

    int first = cnt > FLOG_READ_PAGES ? cnt - FLOG_READ_PAGES : 0;
    size_t need = (size_t) (cnt - first) * FLOG_PAGE_SIZE + 1;
    char *tmp = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tmp == NULL) {
        tmp = heap_caps_malloc(need, MALLOC_CAP_8BIT);
    }
    if (tmp == NULL) {
        free(info);
        dst[0] = '\0';
        return 0;
    }
    size_t used = 0;
    for (int i = first; i < cnt; i++) {
        int p = info[i].page;
        size_t off = FLOG_HDR;
        while (off + 2 <= FLOG_PAGE_SIZE && used + 2 < need) {
            uint16_t len = 0;
            if (esp_partition_read(s_part, (size_t) FLOG_BASE + (size_t) p * FLOG_PAGE_SIZE + off,
                                   &len, 2) != ESP_OK) {
                break;
            }
            if (len == 0 || len == 0xFFFF || off + 2 + len > FLOG_PAGE_SIZE) {
                break;
            }
            if (used + len >= need) {
                break;
            }
            if (esp_partition_read(s_part, (size_t) FLOG_BASE + (size_t) p * FLOG_PAGE_SIZE + off + 2,
                                   tmp + used, len) != ESP_OK) {
                break;
            }
            used += len;
            off += 2 + len;
        }
    }
    tmp[used] = '\0';

    /* 环形缓冲写满后会从头覆盖，很可能截断在某一行中间：
       开头那段是"半截行"，如果保留，它会和下一行粘在一起
       （实测出现过 "<meI (133539) portal: ..." 这种）。
       所以从第一个换行符之后开始用。 */
    {
        char *nl0 = strchr(tmp, '\n');
        if (nl0 != NULL && nl0 + 1 < tmp + used) {
            size_t skip = (size_t) (nl0 + 1 - tmp);
            memmove(tmp, nl0 + 1, used - skip);
            used -= skip;
            tmp[used] = '\0';
        }
    }

    /* 从尾部数 n 个换行 */
    size_t i = used;
    int lines = 0;
    while (i > 0) {
        i--;
        if (tmp[i] == '\n') {
            lines++;
            if (lines > n) {
                i++;
                break;
            }
        }
    }
    size_t len = used - i;
    if (len >= dst_size) {
        len = dst_size - 1;
        i = used - len;
    }
    memcpy(dst, tmp + i, len);
    dst[len] = '\0';
    heap_caps_free(tmp);
    free(info);
    return len;
}

int flog_clear(void)
{
    if (!s_ready) {
        return -1;
    }
    for (int p = 0; p < FLOG_PAGES; p++) {
        esp_partition_erase_range(s_part, (size_t) FLOG_BASE + (size_t) p * FLOG_PAGE_SIZE,
                                  FLOG_PAGE_SIZE);
    }
    s_page = -1;
    s_off = 0;
    s_seq = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stage_len = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "flash 日志已清空");
    return 0;
}
