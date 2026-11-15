#include "logring.h"
#include "flog.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RING_CAP   (64 * 1024)     /* PSRAM 里 64KB，约 1500 行 */

static char *s_buf = NULL;         /* 环形缓冲 */
static size_t s_head = 0;          /* 写位置 */
static size_t s_used = 0;          /* 已存字节数（<= RING_CAP） */
static unsigned s_dropped = 0;     /* 覆盖掉的旧字节数（粗略） */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig = NULL;

static void ring_put(const char *p, size_t n)
{
    if (!s_buf || n == 0) {
        return;
    }
    if (n > RING_CAP) {              /* 单条比整个环还大：只留尾巴 */
        p += n - RING_CAP;
        n = RING_CAP;
    }
    portENTER_CRITICAL(&s_lock);
    size_t first = RING_CAP - s_head;
    if (n <= first) {
        memcpy(s_buf + s_head, p, n);
    } else {
        memcpy(s_buf + s_head, p, first);
        memcpy(s_buf, p + first, n - first);
    }
    s_head = (s_head + n) % RING_CAP;
    if (s_used + n > RING_CAP) {
        s_dropped += (s_used + n - RING_CAP);
        s_used = RING_CAP;
    } else {
        s_used += n;
    }
    portEXIT_CRITICAL(&s_lock);
}

static int log_hook(const char *fmt, va_list ap)
{
    char tmp[256];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        size_t len = (size_t) n;
        if (len >= sizeof(tmp)) {
            len = sizeof(tmp) - 1;   /* 被截断，按实际写入的算 */
        }
        ring_put(tmp, len);
        flog_append(tmp, len);   /* 同时写 flash，重启后还能看 */
    }
    if (s_orig) {
        return s_orig(fmt, ap);
    }
    return vprintf(fmt, ap);
}

void logring_start(void)
{
    if (s_buf != NULL) {
        return;
    }
    /* 优先放 PSRAM，省内部 RAM；没有 PSRAM 就退到内部 */
    s_buf = heap_caps_malloc(RING_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL) {
        s_buf = heap_caps_malloc(RING_CAP, MALLOC_CAP_8BIT);
    }
    if (s_buf == NULL) {
        ESP_LOGE("logring", "环形缓冲分配失败，板上日志不可用");
        return;
    }
    s_head = 0;
    s_used = 0;
    s_dropped = 0;

    flog_start();          /* 先起 flash 日志，再挂钩子 */
    s_orig = esp_log_set_vprintf(log_hook);
    ESP_LOGI("logring", "板上日志已开启：%d KB 环形缓冲（最多约 %d 行）",
             RING_CAP / 1024, RING_CAP / 40);
}

size_t logring_used(void)
{
    return s_used;
}

unsigned logring_dropped(void)
{
    return s_dropped;
}

size_t logring_tail(char *dst, size_t dst_size, int n)
{
    if (!s_buf || !dst || dst_size < 2) {
        if (dst && dst_size) {
            dst[0] = '\0';
        }
        return 0;
    }
    if (n <= 0) {
        n = 100;
    }

    /* 先把整个环按时间顺序拷到临时区（用调用方的 dst 反过来做不划算，
       这里分配一次临时缓冲；64KB 的拷贝在串口命令里不算什么） */
    char *tmp = heap_caps_malloc(s_used + 1, MALLOC_CAP_8BIT);
    if (tmp == NULL) {
        dst[0] = '\0';
        return 0;
    }
    portENTER_CRITICAL(&s_lock);
    size_t start = (s_head + RING_CAP - s_used) % RING_CAP;
    size_t first = RING_CAP - start;
    if (s_used <= first) {
        memcpy(tmp, s_buf + start, s_used);
    } else {
        memcpy(tmp, s_buf + start, first);
        memcpy(tmp + first, s_buf, s_used - first);
    }
    size_t used = s_used;
    portEXIT_CRITICAL(&s_lock);
    tmp[used] = '\0';

    /* 从尾部往前数 n 个 '\n'，找到起点 */
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
    return len;
}

void logring_clear(void)
{
    portENTER_CRITICAL(&s_lock);
    s_head = 0;
    s_used = 0;
    s_dropped = 0;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI("logring", "板上日志已清空");
}
