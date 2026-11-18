# -*- coding: utf-8 -*-
"""生成「设备类型」图标（手机 / 电脑 / 标签 / 耳机）→ 20x20 1bit C 头文件

用 Pillow 直接画形状再二值化，不依赖网络/字体，结果稳定可复现。
"""
import os

from PIL import Image, ImageDraw

W = r"C:\Users\31658\Documents\deepseek-harness\default-workspace"
OUT = os.path.join(W, "yingwang-opensource", "components", "tracker_scan",
                   "include", "type_icons.h")

S = 20      # 最终 20x20
K = 8       # 先画 160x160 再缩


def canvas():
    im = Image.new("L", (S * K, S * K), 255)
    return im, ImageDraw.Draw(im)


def draw_phone(d):
    # 机身
    d.rounded_rectangle([6 * K, 2 * K, 14 * K - 1, 18 * K - 1], radius=2 * K, outline=0, width=2 * K)
    # 听筒
    d.rectangle([9 * K, 4 * K, 11 * K, 4 * K + K // 2], fill=0)
    # 屏幕区域（留白即可）
    # home 条
    d.rectangle([8 * K, 15 * K, 12 * K, 15 * K + K // 2], fill=0)


def draw_laptop(d):
    # 屏幕
    d.rectangle([3 * K, 4 * K, 17 * K, 13 * K], outline=0, width=2 * K)
    # 底座
    d.rounded_rectangle([1 * K, 14 * K, 19 * K, 17 * K - 1], radius=K, fill=0)
    # 屏幕内一道（表示内容）
    d.rectangle([6 * K, 7 * K, 14 * K, 8 * K], fill=0)


def draw_tag(d):
    # 标签主体（圆角方块 + 孔）
    d.rounded_rectangle([3 * K, 6 * K, 17 * K, 16 * K - 1], radius=2 * K, outline=0, width=2 * K)
    d.ellipse([7 * K, 9 * K, 10 * K, 12 * K], fill=0)
    # 系绳
    d.arc([8 * K, 1 * K, 12 * K, 6 * K], 180, 360, fill=0, width=K)


def draw_headphones(d):
    # 头梁
    d.arc([3 * K, 3 * K, 17 * K, 18 * K], 180, 360, fill=0, width=2 * K)
    # 左右罩
    d.rounded_rectangle([3 * K, 10 * K, 6 * K, 16 * K - 1], radius=K, fill=0)
    d.rounded_rectangle([14 * K, 10 * K, 17 * K, 16 * K - 1], radius=K, fill=0)


DRAW = {
    "tag": draw_tag,
    "phone": draw_phone,
    "computer": draw_laptop,
    "audio": draw_headphones,
}


def to_rows(im):
    im = im.resize((S, S), Image.LANCZOS)
    px = im.load()
    rows = []
    for y in range(S):
        bits = 0
        for x in range(S):
            if px[x, y] < 128:
                bits |= 1 << (S - 1 - x)
        rows.append(bits)
    return rows


def main():
    out = {}
    for name, fn in DRAW.items():
        im, d = canvas()
        fn(d)
        out[name] = to_rows(im)
        on = sum(bin(r).count("1") for r in out[name])
        print(f"  {name:9s} 笔画 {on / (S * S) * 100:4.1f}%")

    # ASCII 预览
    for name in DRAW:
        print(f"--- {name} ---")
        for r in out[name]:
            print("".join("#" if (r >> (S - 1 - x)) & 1 else "." for x in range(S)))

    lines = [f'''/*
 * 设备类型图标（{S}x{S} 1bit，1 = 画点）
 * 由 tools/gen_type_icons.py 生成，别手改。
 */
#ifndef _TYPE_ICONS_H_
#define _TYPE_ICONS_H_

#include <stdint.h>

#define TYPE_ICON_SIZE {S}
''']
    for name in ("tag", "phone", "computer", "audio"):
        rows = ", ".join(f"0x{r:05X}" for r in out[name])
        lines.append(f"/* {name} */\nstatic const uint32_t type_icon_{name}[{S}] = {{ {rows} }};\n")

    lines += [
        "/* 按 trk_class_t 取图标 */",
        "static inline const uint32_t *type_icon_of(int cls)",
        "{",
        "    switch (cls) {",
        "    case 0: return type_icon_tag;      /* TRK_CLS_TAG */",
        "    case 1: return type_icon_phone;    /* TRK_CLS_PHONE */",
        "    case 2: return type_icon_computer; /* TRK_CLS_COMPUTER */",
        "    case 3: return type_icon_audio;    /* TRK_CLS_AUDIO */",
        "    default: return NULL;",
        "    }",
        "}",
        "",
        "#endif  // _TYPE_ICONS_H_",
    ]
    open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print("已生成:", OUT, os.path.getsize(OUT), "字节")


if __name__ == "__main__":
    main()
