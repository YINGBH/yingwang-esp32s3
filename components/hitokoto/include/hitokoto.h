#ifndef _HITOKOTO_H_
#define _HITOKOTO_H_

#include <stdbool.h>
#include <stddef.h>

/**
 * 联网拉一条「一言」。
 *
 * @param out  输出缓冲（UTF-8，最多 16 个汉字）
 * @param cap  缓冲大小，至少 8
 * @return     true = 拿到了；false = 网络失败/内容不可用（调用方应保留上一条）
 *
 * 走官方接口 https://v1.hitokoto.cn/?encode=json（15 秒超时，失败不重试）。
 */
bool hitokoto_fetch(char *out, size_t cap);

#endif  // _HITOKOTO_H_