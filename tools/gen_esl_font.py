"""生成价签用的点阵字库头文件（ASCII 8x16 + 汉字 16x16 + 小字 12x12）

用法（先跑 tools/fetch_fonts.py 下好开源字体）：
    python tools/gen_esl_font.py

输出 components/esl_ble/esl_font.h：
    font8x16[95][16]        ASCII 0x20..0x7E，每行 1 字节
    font16x16[]             汉字，每行 2 字节，32 字节/字
    font12x12[]             小字，每行 2 字节，24 字节/字

汉字表 = 基础界面用字 + 日历数据里所有节日名的字 + 常见节日补字。
这样以后 calendar_data.h 里加节日，重跑一次脚本字就齐了。
"""
import io
import os
import re

from PIL import Image, ImageDraw, ImageFont

# 二值化阈值：128 会丢掉抗锯齿边缘的细笔画（屏上看就是缺像素），
# 实测 176 笔画连贯又不会糊成一片。
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)          # 工程根目录（tools 的上一级）

THRESHOLD = 128      # 思源黑体比中易细，阈值从 176 降到 128 保住细笔画

# ASCII 字号：16 会挤满 8px 宽，14 更清楚
ASCII_SIZE = 14
# 小字行（12px 高）里的 ASCII 用这个字号，否则比汉字高出一截
# 阈值单独放宽：11px 的斜杠 `/` 二值化后会断成几段
SMALL_ASCII_SIZE = 12

# 黑体：16px 下笔画 2 像素，比宋体清楚得多
# 只用开源字体（思源黑体，SIL OFL-1.1）：
# 中易黑体 / Segoe UI Symbol 是微软商业字体，不能随源码分发。
# 先跑 tools/fetch_fonts.py 把字体下到 tools/ 目录，或用 OSS_FONT 指定路径。
FONT = os.environ.get("OSS_FONT",
                     os.path.join(HERE, "SourceHanSansCN-Medium.otf"))
# 宋体没有的符号（花朵等），改用这个字体渲染
# 符号也用同一套开源字体（原来用 Segoe UI Symbol，同样是微软字体）
SYMBOL_FONT = os.environ.get("OSS_SYMBOL_FONT", FONT)
# ASCII 用等宽字体：思源黑体的拉丁字母是比例宽度，大写 W 有 10px，
# 塞进 8px 的格子右边会被裁掉（真机上 W 显示成 V）。
ASCII_FONT = os.environ.get("OSS_ASCII_FONT",
                           os.path.join(HERE, "NotoSansMono-Regular.ttf"))
if not os.path.exists(ASCII_FONT):
    ASCII_FONT = FONT
# 需要走 Symbol 字体的码位
# 注意：思源黑体里只有 ❀(U+2740) 和 ✿(U+273F) 这几个花朵，❁❃❋ 之类没有，
#       往界面里加装饰符号前先确认字体里有，否则会渲染成豆腐块。
SYMBOL_SET = set("❁❀✿❃❋✺❂❈✥❆")
OUT = os.path.join(ROOT, "components", "esl_ble", "esl_font.h")
CAL = os.path.join(ROOT, "components", "calendar", "include", "calendar_data.h")

# 固件里会用到的汉字（按需给的子集，宁多勿少）
CJK = (
    "零一二三四五六七八九十百千年月日号周星期"
    "今明昨前后天早中晚节假调休上班"
    "春节元旦清明劳动端午中秋国庆重阳腊八除夕元宵"
    "七夕妇女儿童教师圣诞情人建党建军植树愚人母亲父亲"
    "感恩万圣复活光棍双十"
    "天气晴多云阴雨雪雾霾风雷阵小大暴特强低温湿度"
    "认证已通过未失败离线无连接扫描完成错误待机"
    "充电中板子价签墨水屏余剩余秒分刻"
    "还有距离下次起始结束"
    "东北西南霜降立冬小雪大雪冬至小寒冬至"
    "正二三四五六七八九十"
    "补封兆丰吃饺子元复始见白草回之计惊蛰耕田寒露白露秋分处暑"
    "端午谷雨立夏小满芒种夏至大暑立秋处暑白露霜降"
    "、"
)

# 12x12 基础小字（原来就有的那些）
SMALL_BASE = "立冬小大寒春元百草回蛰惊耕田补封兆丰饺吃见复始之计雨水雪至"

# 日历数据里没有、但常见叫法里会出现的字，补一层保险
EXTRA_HOLIDAYS = (
    "万圣节感恩节情人节母亲节父亲节"
    "光棍节双十一除夕小年寒食中元七夕重阳冬至夏至立春春分秋分"
    "腊八元宵平安狂欢复活圣诞"
)


# ---------------- 自动补字 ----------------
# 扫源码里出现的所有汉字，保证"画什么就有什么"，避免缺字显示成 ?
AUTO_SCAN = [
    os.path.join(ROOT, "components", "esl_ble", "esl_ble.c"),
    # 天气描述（晴 / 毛毛雨 / 雷阵雨 / 冻雨 …）在 time_weather.c 里，
    # 不扫这里就会缺字：之前“毛毛雨”的「毛」就漏了，屏上只显示“雨”
    os.path.join(ROOT, "components", "net_clock", "time_weather.c"),
    CAL,
    os.path.join(ROOT, "components", "esl_ble", "include", "esl_ble.h"),
]


def scanned_cjk():
    chars = set()
    for path in AUTO_SCAN:
        try:
            text = io.open(path, encoding="utf-8", errors="ignore").read()
        except Exception:
            continue
        for ch in text:
            if "\u4e00" <= ch <= "\u9fff" or ch in SYMBOL_SET:
                chars.add(ch)
    return "".join(sorted(chars))


# 最终字表 = 手写表 + 源码自动扫描（保证画什么就有什么）
CJK_ALL = "".join(dict.fromkeys(CJK + SMALL_BASE + scanned_cjk()))


def holiday_names():
    """从日历数据头文件里抠出所有节日名"""
    try:
        src = open(CAL, encoding="utf-8").read()
    except OSError:
        return []
    m = re.search(r"cal_names\[CAL_NAME_COUNT\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        return []
    return re.findall(r'"([^"]+)"', m.group(1))


def uniq(s):
    out = []
    for ch in s:
        if ch.strip() and ch not in out:
            out.append(ch)
    return out


def render_symbol_centered(ch, w, h, size):
    """符号字（花朵等）按包围盒居中放进 w*h，否则会被裁成一条线。
       实测 ✿/❀ 直接画在 16x16 里，上半截会被切掉，只剩中间横条。"""
    big = 64
    img = Image.new("L", (big, big), 255)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(SYMBOL_FONT, size)
    d.text((big // 3, big // 4), ch, font=f, fill=0)
    bb = img.point(lambda v: 255 - v).getbbox()
    out = Image.new("L", (w, h), 255)
    if bb is None:
        return out
    x0, y0, x1, y1 = bb
    gw, gh = x1 - x0, y1 - y0
    ox = (w - gw) // 2 - x0
    oy = (h - gh) // 2 - y0
    out.paste(img.crop(bb), (ox + x0, oy + y0))
    return out


# 16x16 中文加 1 像素描边（原笔画只有 1px，e-ink 上像一条线）
STROKE_WIDE = 0   # 1px 描边在 16px 下太粗，已关闭


def render(ch, w, h, size, dy=0):
    """按墨迹包围盒放进 w*h 的格子，避免不同字体基线差异把字形裁掉。

       · 汉字：垂直居中
       · ASCII：底部对齐到 h-3（保留"点在下、字母在中"的视觉层次）
    """
    if ch in SYMBOL_SET:
        return render_symbol_centered(ch, w, h, size)

    BIG = 64
    canvas = Image.new("L", (BIG, BIG), 255)
    d = ImageDraw.Draw(canvas)
    fnt = ASCII_FONT if ord(ch) < 0x7f else FONT
    f = ImageFont.truetype(fnt, size)
    d.text((BIG // 4, BIG // 4), ch, font=f, fill=0)

    bb = canvas.point(lambda v: 255 - v).getbbox()   # 找墨迹范围
    out = Image.new("L", (w, h), 255)
    if bb is None:
        return out                                   # 空格之类

    x0, y0, x1, y1 = bb
    gw, gh = x1 - x0, y1 - y0
    crop = canvas.crop(bb)

    # 超宽/超高就缩放到能塞进格子（比例字体常见，等宽字体偶尔也有）
    if gw > w or gh > h:
        k = min(w / gw, h / gh)
        nw, nh = max(1, int(gw * k)), max(1, int(gh * k))
        crop = crop.resize((nw, nh), Image.LANCZOS)
        gw, gh = nw, nh

    px = max(0, (w - gw) // 2)
    if ord(ch) > 0x7f:
        py = max(0, (h - gh) // 2)                   # 汉字居中
    else:
        py = max(0, h - 3 - gh)                      # ASCII 底对齐
    out.paste(crop, (px, py))
    return out


def to_bytes(img, rows_bytes, th=None):
    if th is None:
        th = THRESHOLD
    px = img.load()
    w, h = img.size
    out = []
    for y in range(h):
        row = [1 if px[x, y] < th else 0 for x in range(w)]
        if rows_bytes == 1:
            v = 0
            for x in range(min(8, w)):
                v = (v << 1) | row[x]
            out.append(v)
        else:
            hi = 0
            for x in range(8):
                hi = (hi << 1) | (row[x] if x < w else 0)
            lo = 0
            for x in range(8, 16):
                lo = (lo << 1) | (row[x] if x < w else 0)
            out.append(hi)
            out.append(lo)
    return out


def main():
    missing = [p for p in (FONT, ASCII_FONT) if not os.path.exists(p)]
    if missing:
        print("[错误] 找不到字体文件：")
        for p in missing:
            print("    " + p)
        print("先跑 python tools/fetch_fonts.py 下载开源字体，")
        print("或用环境变量 OSS_FONT / OSS_ASCII_FONT 指定字体路径。")
        return 1

    names = holiday_names()
    holiday_text = "".join(names) + "".join(EXTRA_HOLIDAYS)

    # ASCII 8x16：用 16px 字号渲染到 8x16 的格子
    ascii_glyphs = []
    ascii12_glyphs = []
    for cp in range(0x20, 0x7F):
        # 16px 会把数字挤满 8px 宽、笔画贴着边框，屏上都糊在一起；
        # 14px 留出左右各 1px 空隙，笔画完整，数字之间也分得开。
        img = render(chr(cp), 8, 16, ASCII_SIZE, dy=1)
        ascii_glyphs.append(to_bytes(img, 1))
        # 小字行（12px 高）用的 ASCII：字号小一档，和 12x12 汉字齐平
        img12 = render(chr(cp), 8, 12, SMALL_ASCII_SIZE)
        ascii12_glyphs.append(to_bytes(img12, 1, th=140))

    # 汉字 16x16：基础字 + 日历里出现过的所有字
    cjk_chars = uniq(CJK_ALL + holiday_text)
    cjk_glyphs = []
    for ch in cjk_chars:
        img = render(ch, 16, 16, 16)
        cjk_glyphs.append((ord(ch), to_bytes(img, 2)))

    # 12x12 小字：基础小字 + 日历里出现过的所有字（日历格里的节日名用这套）
    small_chars = uniq(CJK_ALL + holiday_text)
    small = []
    for ch in small_chars:
        img = render(ch, 12, 12, 12)
        small.append((ord(ch), to_bytes(img, 2)))

    lines = []
    lines.append("/* 自动生成，别手改 —— 见 tools/gen_esl_font.py */")
    lines.append("/* 字体: 思源黑体 Medium + Noto Sans Mono（SIL OFL-1.1，可随源码分发） */")
    lines.append("#ifndef ESL_FONT_H")
    lines.append("#define ESL_FONT_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("/* ASCII 0x20..0x7E，每个 8x16，每行 1 字节 */")
    lines.append("/* 小字行的 ASCII：8x12，配合 font12x12 的汉字 */")
    lines.append("static const uint8_t font8x12[%d][12] = {" % len(ascii12_glyphs))
    for i, g in enumerate(ascii12_glyphs):
        if len(g) < 12:
            g = list(g) + [0] * (12 - len(g))
        ch = chr(0x20 + i)
        show = ch if 0x21 <= ord(ch) <= 0x7E else '  '
        lines.append("    { %s },  /* %02X %s */" %
                     (", ".join("0x%02X" % b for b in g), 0x20 + i, show))
    lines.append("};")
    lines.append("")
    lines.append("static const uint8_t font8x16[%d][16] = {" % len(ascii_glyphs))
    for i, g in enumerate(ascii_glyphs):
        ch = chr(0x20 + i)
        show = ch if ch not in '"\\' else " "
        lines.append("    { %s },  /* %02X %s */"
                     % (", ".join("0x%02X" % b for b in g), 0x20 + i, show))
    lines.append("};")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    uint16_t cp;")
    lines.append("    uint8_t  bmp[32];   /* 16 行，每行 2 字节 */")
    lines.append("} esl_glyph_t;")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    uint16_t cp;")
    lines.append("    uint8_t  bmp[24];   /* 12 行，每行 2 字节 */")
    lines.append("} esl_glyph12_t;")
    lines.append("")
    lines.append("static const esl_glyph_t font16x16[] = {")
    for cp, g in cjk_glyphs:
        lines.append("    { 0x%04X, { %s } },  /* %s */"
                     % (cp, ", ".join("0x%02X" % b for b in g), chr(cp)))
    lines.append("};")
    lines.append("#define FONT16_COUNT %d" % len(cjk_glyphs))
    lines.append("")
    lines.append("/* 12x12 小字（节日名塞进日历格用） */")
    lines.append("static const esl_glyph12_t font12x12[] = {")
    for cp, g in small:
        lines.append("    { 0x%04X, { %s } },  /* %s */"
                     % (cp, ", ".join("0x%02X" % b for b in g), chr(cp)))
    lines.append("};")
    lines.append("#define FONT12_COUNT %d" % len(small))
    lines.append("")
    lines.append("#endif  /* ESL_FONT_H */")
    lines.append("")

    open(OUT, "w", encoding="utf-8").write("\n".join(lines))
    print("已生成", OUT)
    print("  节日名", len(names), "个，共", len(uniq(holiday_text)), "个不同的字")
    print("  ASCII", len(ascii_glyphs), " 汉字16x16", len(cjk_glyphs), " 小字12x12", len(small))


if __name__ == "__main__":
    main()
