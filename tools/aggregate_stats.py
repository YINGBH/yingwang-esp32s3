#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""汇总所有用户上报的成绩，生成 stats/latest.json

分数公式必须和固件里的 stats_percentile() 完全一致，否则"胜全国 N%"会算歪：

    分数 = int(成功率 × 0.6) + int(min(连续天数, 30) × 1.2) + min(记录天数, 7)
    结果钳到 1..99

用法：
    python tools/aggregate_stats.py --add --issue 12 --body-stdin < body.txt
    python tools/aggregate_stats.py --build          # 只按 reports.json 重算

产物：
    stats/reports.json   每条上报（含 issue 号、时间、原始数字、算出的分数）
    stats/latest.json    {"users":N, "updated":"...", "buckets":[99 个数]}
                         buckets[i] = 分数恰好是 i+1 的人数
"""
import argparse
import json
import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATS_DIR = os.path.join(ROOT, "stats")
REPORTS = os.path.join(STATS_DIR, "reports.json")
LATEST = os.path.join(STATS_DIR, "latest.json")


def score_of(ok, fail, streak, days):
    """和固件 stats_percentile() 一致"""
    total = ok + fail
    rate = (ok * 100 // total) if total else 0
    s = min(streak, 30)
    d = min(days, 7)
    score = int(rate * 0.6) + int(s * 1.2) + d
    return max(1, min(99, score))


def parse_issue_body(body):
    """从 issue 表单里抠出 4 个数字（表单渲染成 '### 标题\\n\\n内容'）"""
    def grab(*names):
        for name in names:
            m = re.search(rf"###\s*{name}\s*\n+\s*(-?\d+)", body)
            if m:
                return int(m.group(1))
        return None

    ok = grab("成功次数", "成功")
    fail = grab("失败次数", "失败")
    streak = grab("连续成功天数", "连续")
    days = grab("有记录的天数", "天数")

    if ok is None or fail is None or streak is None or days is None:
        return None
    ok = max(0, ok)
    fail = max(0, fail)
    streak = max(0, min(streak, 3650))
    days = max(1, min(days, 7))
    return ok, fail, streak, days


def load_json(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def build():
    reports = load_json(REPORTS, {"reports": []})
    buckets = [0] * 99
    for r in reports["reports"]:
        s = int(r.get("score", 0))
        if 1 <= s <= 99:
            buckets[s - 1] += 1
    latest = {
        "users": len(reports["reports"]),
        "updated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "buckets": buckets,
        # 给排行/调试用，固件不读
        "scores_sorted": sorted(int(r.get("score", 0)) for r in reports["reports"]),
    }
    os.makedirs(STATS_DIR, exist_ok=True)
    with open(LATEST, "w", encoding="utf-8", newline="\n") as f:
        json.dump(latest, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"latest.json：{latest['users']} 人上报")
    return latest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--add", action="store_true", help="从 issue 正文加一条上报")
    ap.add_argument("--issue", type=int, default=0)
    ap.add_argument("--body-stdin", action="store_true")
    ap.add_argument("--build", action="store_true")
    a = ap.parse_args()

    if a.add:
        body = sys.stdin.read() if a.body_stdin else ""
        parsed = parse_issue_body(body)
        if parsed is None:
            print("issue 里没找到 4 个数字，跳过", file=sys.stderr)
            return 2
        ok, fail, streak, days = parsed
        score = score_of(ok, fail, streak, days)
        data = load_json(REPORTS, {"reports": []})
        # 同一个 issue 重新编辑：先删旧记录
        data["reports"] = [r for r in data["reports"] if r.get("issue") != a.issue]
        data["reports"].append({
            "issue": a.issue,
            "at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "ok": ok, "fail": fail, "streak": streak, "days": days,
            "score": score,
        })
        os.makedirs(STATS_DIR, exist_ok=True)
        with open(REPORTS, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.write("\n")
        print(f"上报已记录：分数 {score}")

    if a.build or a.add:
        build()
    return 0


if __name__ == "__main__":
    sys.exit(main())
