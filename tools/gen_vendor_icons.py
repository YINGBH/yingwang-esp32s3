# -*- coding: utf-8 -*-
"""把厂商 logo（Simple Icons, CC0）转成本工程的 1bit 位图头文件

流程：下 SVG → 拼 HTML → Edge 无头渲染成 PNG → 裁切阈值化 → 20x20 位图 → C 头文件
"""
import os
import subprocess
import time
import urllib.request

from PIL import Image

W = r"C:\Users\31658\Documents\deepseek-harness\default-workspace"
OUT = os.path.join(W, "yingwang-opensource", "components", "tracker_scan", "include", "vendor_icons.h")
ICONDIR = os.path.join(W, "tools_icons")
os.makedirs(ICONDIR, exist_ok=True)

VENDORS = [
    ("apple", "apple", "TRK_APPLE_OFB"),
    ("google", "google", "TRK_GOOGLE_FMDN"),
    ("samsung", "samsung", "TRK_SAMSUNG"),
    ("tile", "tile", "TRK_TILE"),
    ("dult", "bluetooth", "TRK_DULT"),     # DULT 没有官方图标，用蓝牙符号
    ("huawei", "huawei", "TRK_HUAWEI"),
    ("xiaomi", "xiaomi", "TRK_XIAOMI"),
    ("microsoft", "microsoft", "TRK_MICROSOFT"),
]

SIZE = 20          # 位图 20x20
CELL = 160         # 渲染时每格 160px（放大再缩，边缘更干净）


def fetch(slug):
    path = os.path.join(ICONDIR, slug + ".svg")
    if os.path.exists(path) and os.path.getsize(path) > 100:
        return open(path, encoding="utf-8").read()
    url = f"https://cdn.jsdelivr.net/npm/simple-icons@13/icons/{slug}.svg"
    for attempt in range(3):
        try:
            data = urllib.request.urlopen(url, timeout=30).read().decode("utf-8")
            open(path, "w", encoding="utf-8").write(data)
            return data
        except Exception as e:
            print(f"  {slug} 第{attempt+1}次失败: {e}")
            time.sleep(2)
    raise SystemExit(f"下载失败: {slug}")


def build_html(svgs):
    cells = []
    for i, (name, _, _) in enumerate(VENDORS):
        svg = svgs[name]
        # 黑色填充，居中，留 8% 边距
        cells.append(
            f'<div style="width:{CELL}px;height:{CELL}px;display:flex;'
            f'align-items:center;justify-content:center;background:#fff">{svg}</div>')
    return ("<html><body style='margin:0;background:#fff'>"
            f"<div style='display:flex;flex-wrap:wrap;width:{CELL*2}px'>"
            + "".join(cells) + "</div></body></html>")


def render(html):
    html_path = os.path.join(ICONDIR, "icons.html")
    png_path = os.path.join(ICONDIR, "icons.png")
    open(html_path, "w", encoding="utf-8").write(html)
    edge = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
    subprocess.run([edge, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                    f"--window-size={CELL*2},{CELL*4}",
                    f"--screenshot={png_path}",
                    "file:///" + html_path.replace("\\", "/")],
                   capture_output=True, timeout=120)
    return png_path


def to_bitmap(img):
    """裁成 SIZE x SIZE，二值化成行位图（1 = 黑）"""
    img = img.convert("L").resize((SIZE, SIZE), Image.LANCZOS)
    px = img.load()
    rows = []
    for y in range(SIZE):
        bits = 0
        for x in range(SIZE):
            if px[x, y] < 110:            # 阈值：偏黑的算笔画
                bits |= 1 << (SIZE - 1 - x)
        rows.append(bits)
    # 反色检查：笔画应该占少数，否则说明源图是黑底白字
    on = sum(bin(r).count("1") for r in rows)
    if on > SIZE * SIZE * 0.75:
        rows = [((1 << SIZE) - 1) ^ r for r in rows]
    return rows


def ms_bitmap():
    """手绘 Windows 四方块：每块 8x8，间隔 2px，20x20 内居中"""
    S = SIZE
    rows = [0] * S
    for y in range(S):
        bits = 0
        for x in range(S):
            in_x = (2 <= x < 10) or (12 <= x < 20)
            in_y = (2 <= y < 10) or (12 <= y < 20)
            if in_x and in_y:
                bits |= 1 << (S - 1 - x)
        rows[y] = bits
    return rows


def main():
    svgs = {name: fetch(slug) for name, slug, _ in VENDORS}
    # 统一给 SVG 加尺寸（simple-icons 默认 24x24，但内联时可能撑满）
    for name, _, _ in VENDORS:
        svgs[name] = svgs[name].replace("<svg ", f'<svg width="{CELL}" height="{CELL}" ', 1)

    png = render(build_html(svgs))
    sheet = Image.open(png)
    print("渲染尺寸:", sheet.size)

    bm = {"microsoft": ms_bitmap()}
    for i, (name, _, enum) in enumerate(VENDORS):
        if name in bm:
            continue                      # 手绘的跳过
        col, row = i % 2, i // 2
        box = (col * CELL, row * CELL, (col + 1) * CELL, (row + 1) * CELL)
        bm[name] = to_bitmap(sheet.crop(box))
        on = sum(bin(r).count("1") for r in bm[name])
        print(f"  {name:8s} 笔画占比 {on / (SIZE * SIZE) * 100:4.1f}%")

    lines = [f'''/*
 * 厂商图标（{SIZE}x{SIZE} 1bit，1 = 画点）
 *
 * 图标来自 Simple Icons（https://simpleicons.org，CC0 1.0），
 * 商标归各厂商所有；这里仅用于标识"扫到的是哪家的标签"。
 * 由 tools/gen_vendor_icons.py 生成，别手改。
 */
#ifndef _VENDOR_ICONS_H_
#define _VENDOR_ICONS_H_

#include <stdint.h>

#define VENDOR_ICON_SIZE {SIZE}
''']
    for name, _, enum in VENDORS:
        rows = ", ".join(f"0x{r:05X}" for r in bm[name])
        lines.append(f"/* {name} */\nstatic const uint32_t vendor_icon_{name}[{SIZE}] = {{ {rows} }};\n")

    lines.append("/* 按 trk_vendor_t 顺序取图标；DULT 用蓝牙符号 */")
    lines.append("static inline const uint32_t *vendor_icon_of(int v)\n{")
    lines.append("    switch (v) {")
    for name, _, enum in VENDORS:
        lines.append(f"    case {enum}: return vendor_icon_{name};")
    lines.append("    default: return NULL;")
    lines.append("    }")
    lines.append("}")
    lines.append("\n#endif  // _VENDOR_ICONS_H_")

    open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print("已生成:", OUT, os.path.getsize(OUT), "字节")


if __name__ == "__main__":
    main()
