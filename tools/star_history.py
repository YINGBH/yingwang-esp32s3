#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成仓库的 Star 趋势图（自托管版，不依赖 star-history.com）

原理：
  1. 调 GitHub REST API 的 stargazers 接口（Accept: application/vnd.github.star+json）
     分页拿「每个 star 的时间」
  2. 按天累计成折线
  3. 画成 SVG 写到 docs/star-history.svg

用法：
    GH_TOKEN=xxx python tools/star_history.py
    python tools/star_history.py --repo YINGBH/yingwang-esp32s3 --out docs/star-history.svg

不需要任何第三方库；GitHub Actions 里每天跑一次即可。
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from collections import OrderedDict
from datetime import datetime, timezone

API = "https://api.github.com"


def gh(url, token):
    req = urllib.request.Request(url, headers={
        "Authorization": "Bearer " + token,
        "User-Agent": "star-history-script",
        "Accept": "application/vnd.github.star+json",   # 关键：带 starred_at
    })
    with urllib.request.urlopen(req, timeout=40) as r:
        return json.load(r), r.headers.get("Link", "")


def fetch_stars(repo, token):
    """返回 [(日期字符串, 累计数)]，按天聚合"""
    dates = []
    page = 1
    while True:
        url = f"{API}/repos/{repo}/stargazers?per_page=100&page={page}"
        try:
            data, link = gh(url, token)
        except urllib.error.HTTPError as e:
            print(f"请求失败 {e.code}: {e.reason}", file=sys.stderr)
            return []
        if not data:
            break
        for item in data:
            ts = item.get("starred_at")
            if ts:
                dates.append(ts[:10])
        if 'rel="next"' not in link:
            break
        page += 1
        if page > 100:            # 1 万 star 封顶，够用了
            break

    if not dates:
        return []

    dates.sort()
    per_day = OrderedDict()
    for d in dates:
        per_day[d] = per_day.get(d, 0) + 1

    out, total = [], 0
    for d, n in per_day.items():
        total += n
        out.append((d, total))

    # 两边补齐，让曲线有横向跨度：
    #   第一天之前 0 个 → 每天累加 → 今天保持平线
    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    if out:
        if out[0][1] > 0:
            out.insert(0, (out[0][0], 0))       # 起点从 0 起跳
        if out[-1][0] < today:
            out.append((today, out[-1][1]))     # 一直平到今天
    return out


def build_svg(points, repo):
    """画一张极简折线图（宽 720 高 320）。只有一个数据点时画圆点。"""
    W, H = 720, 320
    L, R, T, B = 56, 16, 24, 44
    iw, ih = W - L - R, H - T - B

    if not points:
        points = [(datetime.now(timezone.utc).strftime("%Y-%m-%d"), 0)]
    n = len(points)
    max_y = max(1, points[-1][1])

    def px(i):
        return L + (iw * i / max(1, n - 1))

    def py(v):
        return T + ih - (ih * v / max_y)

    # Y 轴：最多 5 条整数刻度
    step = max(1, (max_y + 4) // 4)
    ticks = []
    v = 0
    while v <= max_y:
        y = py(v)
        ticks.append(
            f'<line x1="{L}" y1="{y:.1f}" x2="{W - R}" y2="{y:.1f}" stroke="#8884" stroke-width="1"/>'
            f'<text x="{L - 8}" y="{y + 4:.1f}" font-size="11" fill="#888" text-anchor="end">{v}</text>')
        v += step

    # 折线 + 面积
    if n == 1:
        cx, cy = px(0), py(points[0][1])
        body = (f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="4" fill="#e4572e"/>')
    else:
        line = " ".join(f"{'M' if i == 0 else 'L'}{px(i):.1f},{py(v):.1f}"
                        for i, (_, v) in enumerate(points))
        area = (f"M{px(0):.1f},{T + ih:.1f} " +
                " ".join(f"L{px(i):.1f},{py(v):.1f}" for i, (_, v) in enumerate(points)) +
                f" L{px(n - 1):.1f},{T + ih:.1f} Z")
        body = (f'<path d="{area}" fill="#e4572e22" stroke="none"/>'
                f'<path d="{line}" fill="none" stroke="#e4572e" stroke-width="2" stroke-linejoin="round"/>')

    # X 轴：首 / 中 / 尾（去掉重复日期；首尾用 start/end 对齐，免得被裁掉）
    labels = []
    picks = [0, n // 2, n - 1]
    seen = set()
    for idx in picks:
        d = points[idx][0]
        if d in seen:
            continue
        seen.add(d)
        if idx == 0:
            anchor, x = "start", L
        elif idx == n - 1:
            anchor, x = "end", W - R
        else:
            anchor, x = "middle", px(idx)
        labels.append(f'<text x="{x:.1f}" y="{H - B + 18}" font-size="11" '
                      f'fill="#888" text-anchor="{anchor}">{d}</text>')

    total = points[-1][1]
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">
  <rect width="{W}" height="{H}" rx="10" fill="#ffffff"/>
  <text x="{L}" y="16" font-size="13" font-weight="600" fill="#333">{repo} · Star History</text>
  <text x="{W - R}" y="16" font-size="13" fill="#333" text-anchor="end">\u2605 {total}</text>
  {''.join(ticks)}
  <line x1="{L}" y1="{T + ih}" x2="{W - R}" y2="{T + ih}" stroke="#8888" stroke-width="1"/>
  {body}
  {''.join(labels)}
</svg>
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default="YINGBH/yingwang-esp32s3")
    ap.add_argument("--out", default="docs/star-history.svg")
    a = ap.parse_args()

    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if not token:
        sys.exit("需要 GH_TOKEN（GitHub Actions 里用 secrets.GITHUB_TOKEN）")

    points = fetch_stars(a.repo, token)
    print(f"拿到 {sum(n for _, n in points[-1:]) if points else 0} 个 star，{len(points)} 天有增长")

    svg = build_svg(points, a.repo)
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(svg)
    print("已写入", a.out)


if __name__ == "__main__":
    main()
