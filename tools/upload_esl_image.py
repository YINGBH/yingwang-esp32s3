#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把图片上传到墨水屏（296x128 三色：黑 / 白 / 红）

两步用法（推荐，Venv 里可能缺 Pillow）：

    # 1) 转换（需要 Pillow）
    python tools/upload_esl_image.py --convert 图片.png --out esl.bin
    # 2) 上传（需要 pyserial）
    python tools/upload_esl_image.py COM3 --planes esl.bin --mode 1

一步也行（两个库都装了）：

    python tools/upload_esl_image.py COM3 图片.png --mode 2

模式：0 关（用原来的布局）· 1 整屏图片 · 2 左半图片 + 右侧天气 · 3 左侧日历 + 右半图片

协议：EIMG B → EIMG D <hex> … → EIMG E
"""
import argparse
import re
import sys
import time

W, H = 296, 128
ROW_BYTES = H // 8          # 16
PLANE = W * ROW_BYTES       # 4736
TOTAL = PLANE * 2           # 9472


def to_planes(path):
    """图片 → 双 plane（plane0：黑=0/白=1；plane1：红=1）"""
    try:
        from PIL import Image
    except ImportError:
        sys.exit("需要 Pillow：pip install pillow（只有转换这一步用得到）")

    im = Image.open(path).convert("RGB")

    # 保持比例缩放到 296x128，居中裁切（不变形）
    src_r = im.width / im.height
    dst_r = W / H
    if src_r > dst_r:
        new_h, new_w = H, int(round(H * src_r))
    else:
        new_w, new_h = W, int(round(W / src_r))
    im = im.resize((new_w, new_h), Image.LANCZOS)
    left = (new_w - W) // 2
    top = (new_h - H) // 2
    im = im.crop((left, top, left + W, top + H))

    black = bytearray(b"\xff" * PLANE)   # 1 = 白
    red = bytearray(b"\x00" * PLANE)     # 1 = 红
    px = im.load()

    for y in range(H):
        byte_i = y >> 3
        bit = 0x80 >> (y & 7)
        for x in range(W):
            r, g, b = px[x, y]
            col = W - 1 - x               # 屏是镜像的，和固件 col_of() 一致
            idx = col * ROW_BYTES + byte_i
            is_red = (r > 120 and r - g > 55 and r - b > 55)
            lum = (r * 299 + g * 587 + b * 114) // 1000
            if is_red:
                red[idx] |= bit
                black[idx] &= ~bit
            elif lum < 128:
                black[idx] &= ~bit
    return bytes(black + red)


def open_port(port):
    try:
        import serial
    except ImportError:
        sys.exit("需要 pyserial：pip install pyserial")
    s = serial.Serial(port, 115200, timeout=0.5)
    s.dtr = False
    s.rts = False
    time.sleep(8.0)                 # 打开串口会复位板子
    s.reset_input_buffer()
    return s


def send_cmd(s, cmd, timeout=5.0):
    s.write(cmd.encode() + b"\n")
    s.flush()
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        c = s.read(256)
        if c:
            buf += c
            if b'"ok"' in buf or b'"err"' in buf:
                break
    return buf.decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?")
    ap.add_argument("image", nargs="?")
    ap.add_argument("--mode", type=int, help="上传后设置模式 0/1/2/3")
    ap.add_argument("--erase", action="store_true")
    ap.add_argument("--convert", metavar="PNG", help="只转换，不上传")
    ap.add_argument("--out", metavar="BIN")
    ap.add_argument("--planes", metavar="BIN", help="上传已经转好的 .bin")
    args = ap.parse_args()

    if args.convert:
        out = args.out or (args.convert.rsplit(".", 1)[0] + "_esl.bin")
        data = to_planes(args.convert)
        open(out, "wb").write(data)
        print(f"已转换：{out}（{len(data)} 字节）")
        return

    if args.planes:
        data = open(args.planes, "rb").read()
        if len(data) != TOTAL:
            sys.exit(f"文件大小不对：{len(data)}，应为 {TOTAL}")
    elif args.image:
        data = to_planes(args.image)
    else:
        data = None

    if not args.port:
        sys.exit("要上传就得多给串口，比如 COM3")

    s = open_port(args.port)

    if args.erase:
        print("擦除:", send_cmd(s, "EIMG X").strip()[:80])
        if args.mode is not None:
            print("模式:", send_cmd(s, f"SET esl_img={args.mode}").strip()[:80])
        s.close()
        return

    if data is None:
        sys.exit("要么给图片路径 / --planes，要么加 --erase")

    print(f"待上传：{len(data)} 字节")
    print("开始:", send_cmd(s, "EIMG B").strip()[:60])

    step = 16                        # 每包 16 字节：命令行超过 ~80 字符会被截断
    total = len(data)

    def send_one(off):
        """发一片，返回板子实际收下的字节数（-1 = 没拿到应答）"""
        chunk = data[off:off + step]
        r = send_cmd(s, f"EIMG D {off} " + chunk.hex(), timeout=3.0)
        m = re.search(r'"n":(\d+)', r)
        return int(m.group(1)) if m else -1

    def send_range(off):
        bad = 0
        while off < total:
            want = len(data[off:off + step])
            ok = False
            for _ in range(6):
                got = send_one(off)
                if got == want:          # 整片收全才算成功
                    ok = True
                    break
            if not ok:
                bad += 1
                if bad > 20:
                    print(f"  连续失败太多，停在 {off}")
                    return off
            off += want
        return off

    off = send_range(0)
    if off % (step * 64) == 0:
        print(f"  {off}/{total}")

    # 整体对账（兜底）
    for _ in range(6):
        r = send_cmd(s, "EIMG Q")
        m = re.search(r'"got":(\d+)', r)
        got = int(m.group(1)) if m else -1
        if got == total:
            break
        if got <= 0:
            print("  对账失败:", r.strip()[:60])
            break
        print(f"  已收 {got}/{total}，从 {got} 补传")
        send_range(got)

    print("完成:", send_cmd(s, "EIMG E").strip()[:80])
    if args.mode is not None:
        print("模式:", send_cmd(s, f"SET esl_img={args.mode}").strip()[:80])
    s.close()


if __name__ == "__main__":
    main()
