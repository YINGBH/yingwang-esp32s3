# -*- coding: utf-8 -*-
"""下载生成点阵所需的开源字体（SIL OFL-1.1）。

   用法：
       python tools/fetch_fonts.py

   产物放在 tools/ 目录下（和 gen_font.py 同级），脚本会自动找到它们。
   也可以自己下载后设环境变量：
       OSS_FONT=路径/SourceHanSansCN-Medium.otf
       OSS_MONO_FONT=路径/NotoSansMono-Regular.ttf
"""
import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))

TARGETS = [
    (
        "SourceHanSansCN-Regular.otf",
        "https://cdn.jsdelivr.net/gh/adobe-fonts/source-han-sans@release"
        "/SubsetOTF/CN/SourceHanSansCN-Regular.otf",
        8 * 1024 * 1024,
        "思源黑体 Regular（SIL OFL-1.1）",
    ),
    (
        "NotoSansMono-Regular.ttf",
        "https://cdn.jsdelivr.net/gh/googlefonts/noto-fonts@main"
        "/hinted/ttf/NotoSansMono/NotoSansMono-Regular.ttf",
        400 * 1024,
        "Noto Sans Mono Regular（SIL OFL-1.1）",
    ),
]


def main():
    # 系统代理没开的时候 urllib 会被拒，这里强制直连
    urllib.request.getproxies = lambda: {}

    ok = True
    for name, url, min_size, desc in TARGETS:
        dst = os.path.join(HERE, name)
        if os.path.exists(dst) and os.path.getsize(dst) > min_size:
            print("[跳过] %s 已存在（%.1f MB）" % (name, os.path.getsize(dst) / 1e6))
            continue
        print("[下载] %s —— %s" % (desc, url))
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "fetch-fonts/1.0"})
            with urllib.request.urlopen(req, timeout=180) as r, open(dst, "wb") as f:
                f.write(r.read())
            size = os.path.getsize(dst)
            if size < min_size:
                print("     文件太小（%d 字节），可能下错了" % size)
                ok = False
            else:
                print("     完成：%.1f MB" % (size / 1e6))
        except Exception as e:
            print("     失败：%s" % str(e)[:100])
            ok = False

    if not ok:
        print("\n有字体没拿到。可以手动下载后放到：%s" % HERE)
        return 1

    print("\n字体就绪。下一步：")
    print("    python tools/gen_font.py        # 生成彩屏点阵")
    print("    python tools/gen_esl_font.py    # 生成墨水屏点阵")
    return 0


if __name__ == "__main__":
    sys.exit(main())
