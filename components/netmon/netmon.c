#include "netmon.h"

#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

static const char *TAG = "netmon";

#define PROBE_INTERVAL_MS 5000
#define PROBE_TIMEOUT_MS  1500

static netmon_t s_stats;
static SemaphoreHandle_t s_lock = NULL;
static uint32_t s_online_since = 0;   /* 秒级时间戳 */

/* 取当前默认网关地址，拿不到返回 0 */
static uint32_t gateway_addr(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) return 0;

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        return 0;
    }
    return info.gw.addr;
}

/**
 * 对网关 80 端口做一次 TCP 连接，返回耗时毫秒，失败返回 -1。
 * 用非阻塞 connect + select 控制超时，不会把任务卡死。
 */
int netmon_ping_once(void)
{
    uint32_t gw = gateway_addr();
    if (gw == 0) {
        return -1;
    }

    /* 用 UDP 探网关：UDP 不建连接、没有 TIME_WAIT。
       以前用 TCP connect，每 5 秒一个连接、TIME_WAIT 60 秒，
       16 个 socket 里长期有 12 个被占住，导致天气/门户的
       socket 申请全部失败（表现为 getaddrinfo 失败、sock < 0）。 */
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(53);          /* 网关的 DNS 端口 */
    dest.sin_addr.s_addr = gw;

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int64_t t0 = esp_timer_get_time();
    int rtt = -1;

    uint8_t probe = 0;
    ssize_t sent = sendto(sock, &probe, 1, 0,
                          (struct sockaddr *) &dest, sizeof(dest));
    if (sent == 1) {
        /* UDP 发出去就算网关这条路径通（发不出去会立刻返回错误） */
        rtt = (int) ((esp_timer_get_time() - t0) / 1000);
    }

    close(sock);
    return rtt;
}

static void push_history(int rtt)
{
    s_stats.history[s_stats.history_head] = rtt;
    s_stats.history_head = (s_stats.history_head + 1) % NETMON_HISTORY;
    if (s_stats.history_count < NETMON_HISTORY) {
        s_stats.history_count++;
    }
}

static void recompute(void)
{
    /* 平均：最近 10 个样本里成功的那些 */
    int sum = 0, n = 0;
    for (int i = 0; i < 10 && i < s_stats.history_count; i++) {
        int idx = (s_stats.history_head - 1 - i + NETMON_HISTORY * 2) % NETMON_HISTORY;
        int v = s_stats.history[idx];
        if (v >= 0) {
            sum += v;
            n++;
        }
    }
    s_stats.rtt_avg_ms = (n > 0) ? (sum / n) : -1;

    /* 丢包：最近 20 个样本 */
    int total = 0, lost = 0;
    for (int i = 0; i < 20 && i < s_stats.history_count; i++) {
        int idx = (s_stats.history_head - 1 - i + NETMON_HISTORY * 2) % NETMON_HISTORY;
        total++;
        if (s_stats.history[idx] < 0) lost++;
    }
    s_stats.loss_percent = (total > 0) ? (lost * 100 / total) : 0;
}

static void __attribute__((unused)) netmon_task(void *arg)
{
    (void) arg;

    while (true) {
        int rtt = netmon_ping_once();

        struct timeval now;
        gettimeofday(&now, NULL);

        xSemaphoreTake(s_lock, portMAX_DELAY);

        s_stats.rtt_ms = rtt;
        push_history(rtt);
        recompute();

        bool online = (rtt >= 0);
        if (online && !s_stats.online) {
            s_online_since = (uint32_t) now.tv_sec;
        }
        if (!online && s_stats.online) {
            s_online_since = 0;
        }
        s_stats.online = online;

        if (s_online_since > 0 && now.tv_sec > 0) {
            s_stats.online_seconds = (uint32_t) (now.tv_sec - s_online_since);
        }

        xSemaphoreGive(s_lock);

        vTaskDelay(pdMS_TO_TICKS(PROBE_INTERVAL_MS));
    }
}

void netmon_start(void)
{
    /* 已停用：原来每 5 秒建一次 TCP 连接探网关，
       连接关闭后进 TIME_WAIT（60 秒），16 个 socket 里长期有 12 个被占住，
       导致天气/门户的 socket 申请失败（getaddrinfo 报错、sock < 0）。

       现在只保留被动统计（重连次数、在线时长），不再主动探测。
       状态页的延迟/丢包曲线会显示为空，其它功能不受影响。 */
    ESP_LOGW(TAG, "网络监控已停用（避免 socket 被 TIME_WAIT 占满）");
    s_stats.probing = false;
    s_stats.online = true;
    s_stats.rtt_ms = -1;
    s_stats.rtt_avg_ms = -1;
    s_stats.loss_percent = 0;
    return;
}

void netmon_get(netmon_t *out)
{
    if (out == NULL) return;
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        out->rtt_ms = -1;
        out->rtt_avg_ms = -1;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_stats;
    xSemaphoreGive(s_lock);
}

void netmon_note_reconnect(void)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stats.reconnect_count++;
    s_online_since = 0;
    s_stats.online = false;
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "记录一次重连，累计 %u 次", (unsigned) s_stats.reconnect_count);
}

void netmon_add_bytes(uint32_t in, uint32_t out)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stats.bytes_in += in;
    s_stats.bytes_out += out;
    xSemaphoreGive(s_lock);
}