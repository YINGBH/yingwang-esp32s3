"""
从开源字体生成点阵字库头文件

为什么要这么干：
  点阵字模如果手写，一个笔误就是屏幕上多一道杠，而且几百个字符没法校对。
  用 Pillow 把系统字体渲染成位图，结果确定、可复现，还能随时改字号或换字体。

用法：
  python tools/gen_font.py
  产物 -> components/bsp_nologo/font_data.h

只允许用 SIL OFL-1.1 的开源字体（思源黑体 / Noto Sans Mono）：
微软系统字体（中易黑体、Segoe UI Symbol）不允许把字形抽出来再分发。
"""

import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PROJ = os.path.dirname(HERE)          # 工程根目录（tools 的上一级）
OUT = os.path.join(PROJ, "components", "bsp_nologo", "font_data.h")

# ---------------------------------------------------------------- 字体候选
# Windows 上未必每个都有，按顺序找第一个能用的
# 开源字体：Noto Sans Mono（SIL OFL-1.1），等宽，状态屏对齐用
ASCII_FONTS = [
    os.environ.get("OSS_MONO_FONT",
                  os.path.join(HERE, "NotoSansMono-Regular.ttf")),
]
# 开源字体：思源黑体（SIL OFL-1.1）
CJK_FONTS = [
    os.environ.get("OSS_FONT",
                  os.path.join(HERE, "SourceHanSansCN-Regular.otf")),
]

ASCII_W, ASCII_H = 8, 16
CJK_W, CJK_H = 16, 16

# 汉字不再手工维护：直接扫源码里的中文字符串。
# 以前手写这张表，改了界面就会漏字，屏幕上显示成空白——
# 现在界面文案改了什么，重新生成一次字库就自动跟上。
_SOURCE_DIRS = [
    os.path.join(PROJ, "main"),
    os.path.join(PROJ, "components"),
]
_SKIP_FILES = {"font_data.h", "app_config.h"}


def strip_c_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def collect_extra_text():
    """额外扫一个纯文本文件：里面写什么字就收什么字
       （典型用途：运行时用 SET city= 配置的地名，源码里没有）"""
    out = []
    for path in (os.path.join(ROOT, "中文补充.txt"),
                 os.path.join(ROOT, "extra_cjk.txt"),
                 os.path.join(ROOT, "ESP32", "firmware", "中文补充.txt"),
                 os.path.join(ROOT, "ESP32", "firmware", "extra_cjk.txt")):
        if not os.path.exists(path):
            continue
        try:
            txt = open(path, encoding="utf-8").read()
        except OSError:
            continue
        for ch in txt:
            o = ord(ch)
            if (0x4E00 <= o <= 0x9FFF) or (0x3000 <= o <= 0x303F) or (0xFF00 <= o <= 0xFFEF):
                if ch not in out:
                    out.append(ch)
        print(f"额外字库: {os.path.basename(path)} -> {len(out)} 字")
    return out


def collect_cjk():
    chars = []
    for d in _SOURCE_DIRS:
        for dirpath, _, files in os.walk(d):
            for name in files:
                if not name.endswith((".c", ".h", ".cc")) or name in _SKIP_FILES:
                    continue
                path = os.path.join(dirpath, name)
                try:
                    src = open(path, encoding="utf-8").read()
                except (OSError, UnicodeDecodeError):
                    continue
                for ch in strip_c_comments(src):
                    # 汉字 + 中文标点全角区（！：？（）等都在 \uff00-\uffef）
                    o = ord(ch)
                    is_cjk = (0x4E00 <= o <= 0x9FFF) or (0x3000 <= o <= 0x303F) or (0xFF00 <= o <= 0xFFEF)
                    if is_cjk and ch not in chars:
                        chars.append(ch)
    return chars


def pick_font(candidates, size):
    for path in candidates:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size), path
            except OSError:
                continue
    return None, None


def render_glyph(font, ch, w, h, baseline=False):
    """把一个字符渲染成 h 行 w 位（1 = 亮点）

    关键：**不要按字形的墨迹包围盒去居中**。
    以前那样做，'.' 只有 2 行高，居中后跑到第 7~8 行（竖直中间），
    看起来像横杠 —— "v1.7.0" 会显示成 "v1-7-0"。
    现在按基线对齐（ASCII）或整字居中（CJK），所有字符共用同一基线。
    """
    img = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(img)
    if baseline:
        ascent, _descent = font.getmetrics()
        base_y = ascent
        if base_y > h - 2:
            base_y = h - 2
        if base_y < h // 2:
            base_y = h // 2
        d.text((w // 2, base_y), ch, fill=255, font=font, anchor="ms")
    else:
        d.text((w // 2, h // 2), ch, fill=255, font=font, anchor="mm")

    rows = []
    for y in range(h):
        bits = 0
        for x in range(w):
            if img.getpixel((x, y)) > 96:
                bits |= 1 << (w - 1 - x)
        rows.append(bits)
    return rows


def main():
    ascii_font, ascii_path = pick_font(ASCII_FONTS, ASCII_H)
    if ascii_font is None:
        print("[错误] 找不到 ASCII 字体，先跑 python tools/fetch_fonts.py")
        print("       或用 OSS_MONO_FONT 指定 NotoSansMono-Regular.ttf 的路径")
        return 1

    cjk_font, cjk_path = pick_font(CJK_FONTS, CJK_H)
    if cjk_font is None:
        print("[错误] 找不到中文字体，先跑 python tools/fetch_fonts.py")
        print("       或用 OSS_FONT 指定 SourceHanSansCN-Regular.otf 的路径")
        return 1

    print(f"ASCII 字体: {ascii_path}")
    print(f"中文 字体: {cjk_path}")

    # ---- ASCII 32..126 ----
    ascii_rows = []
    for code in range(32, 127):
        ascii_rows.extend(render_glyph(ascii_font, chr(code), ASCII_W, ASCII_H, baseline=True))

    # ---- 汉字 ----
    seen = collect_cjk()
    for ch in collect_extra_text():
        if ch not in seen:
            seen.append(ch)
    if not seen:
        print("[警告] 源码里没扫到中文，界面会是空白")

    cjk_rows = []
    for ch in seen:
        cjk_rows.extend(render_glyph(cjk_font, ch, CJK_W, CJK_H))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)

    def emit_bytes(rows):
        out = []
        for i in range(0, len(rows), 16):
            chunk = rows[i:i + 16]
            out.append("    " + ", ".join(f"0x{v:04X}" for v in chunk) + ",")
        return "\n".join(out)

    lines = []
    lines.append("/* 自动生成，不要手改 —— 用 tools/gen_font.py 重新生成 */")
    lines.append(f"/* ASCII: {os.path.basename(ascii_path)} {ASCII_W}x{ASCII_H} */")
    lines.append(f"/* CJK  : {os.path.basename(cjk_path)} {CJK_W}x{CJK_H} */")
    lines.append("")
    lines.append("#pragma once")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append(f"#define FONT_ASCII_W {ASCII_W}")
    lines.append(f"#define FONT_ASCII_H {ASCII_H}")
    lines.append(f"#define FONT_ASCII_FIRST 32")
    lines.append(f"#define FONT_ASCII_COUNT {127 - 32}")
    lines.append(f"#define FONT_CJK_W {CJK_W}")
    lines.append(f"#define FONT_CJK_H {CJK_H}")
    lines.append(f"#define FONT_CJK_COUNT {len(seen)}")
    lines.append("")
    lines.append("/* 每行 1 个 uint16，高位在左；ASCII 用不到 16 位 */")
    lines.append("static const uint16_t font_ascii_bits[] = {")
    lines.append(emit_bytes(ascii_rows))
    lines.append("};")
    lines.append("")
    lines.append("/* 汉字按 FONT_CJK_CHARS 的顺序排列，每字 {CJK_H} 行 */")
    lines.append("static const uint16_t font_cjk_bits[] = {")
    lines.append(emit_bytes(cjk_rows))
    lines.append("};")
    lines.append("")
    lines.append("static const char *font_cjk_chars = \"" + "".join(seen) + "\";")
    lines.append("")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    size = os.path.getsize(OUT)
    print(f"\n已生成: {OUT}")
    print(f"  ASCII {127 - 32} 个字符, 汉字 {len(seen)} 个")
    print(f"  文件大小 {size:,} 字节")
    return 0


if __name__ == "__main__":
    sys.exit(main())