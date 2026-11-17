/*
 * 联网统计：从仓库读 stats/latest.json，算真实的「胜全国 N%」
 *
 * 数据来源：用户在 GitHub 提「成绩上报」issue -> Action 汇总成 stats/latest.json
 * 板子只读这个文件，**不带任何写权限**。
 *
 * 网络可能根本连不上（校园网常把 raw.githubusercontent.com 的 DNS 过滤掉），
 * 所以这里所有接口都允许失败：失败就返回 -1，界面退回本地估算。
 */
#ifndef _NETSTATS_H_
#define _NETSTATS_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** 拉一次最新统计（联网时才有用）。成功返回 ESP_OK */
esp_err_t netstats_refresh(void);

/** 有数据吗（刷新成功过） */
bool netstats_ok(void);

/** 上报人数（没数据返回 0） */
int netstats_users(void);

/**
 * 我的分数在所有人里能超过百分之多少。
 * 没数据返回 -1（调用方据此退回本地估算）。
 */
int netstats_percentile(int score);

#endif  // _NETSTATS_H_
