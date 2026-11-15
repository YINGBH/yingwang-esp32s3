# -*- coding: utf-8 -*-
"""把训练出来的权重量化成 int16 模型包，产出两样东西：

    out/captcha_model_q.h   int16 权重（想编进 app 就用这个，不用分区）
    out/model_q.bin         模型包，烧到 storage 分区（推荐，省 app 空间）

原理：每个张量按 max|w| / 32767 量化成 int16，推理时乘回缩放系数。
实测（本项目 19 张真机样本）：float32 与 int16 的预测结果 19/19 完全一致。

模型包格式（固件 components/captcha/captcha_loader.c 按这个解析）：
    0    魔数 "ESL1"
    4    张量个数 (u32) —— 固件要求**正好 8 个**（w1,b1,w2,b2,w3,b3,w4,b4）
    8    每个张量: 元素个数 (u32) + 缩放系数 (f32)
    对齐到 8 字节后，依次是各张量的 int16 数据

用法：
    python quantize_model.py                                  # 读 out/weights.npz
    python quantize_model.py --npz 别的/weights.npz --out 别的/out
    python quantize_model.py --npz w.npz --size 60x24 --chars 0123456789

    --size / --chars 不给的话，会先找 npz 旁边的 meta.json（train.py 会自动写）。
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))

# 固件里的名字（顺序不能改，loader 按固定 8 个张量解析）
NAMES = [("w1", "CM_W1"), ("b1", "CM_B1"), ("w2", "CM_W2"), ("b2", "CM_B2"),
         ("w3", "CM_W3"), ("b3", "CM_B3"), ("w4", "CM_W4"), ("b4", "CM_B4")]


def quantize_tensor(a: np.ndarray):
    """按 max|w| 量化成 int16，返回 (int16 数据, 缩放系数)"""
    a = np.asarray(a, dtype=np.float32)
    m = float(np.abs(a).max())
    scale = (m / 32767.0) if m > 0 else 1.0
    q = np.clip(np.round(a / scale), -32767, 32767).astype(np.int16)
    return q, scale


def emit_header(path: str, tensors, shape, charset: str) -> None:
    H, W = shape
    by_name = {name: q for (name, _), (q, _s) in zip(NAMES, tensors)}
    w1, w2, w3, w4 = by_name["w1"], by_name["w2"], by_name["w3"], by_name["w4"]
    C1, C2 = w1.shape[0], w2.shape[0]
    FC = w3.shape[1]
    FLAT = w3.shape[0]
    NDIGIT, NCLS = w4.shape[0], w4.shape[2]

    lines = [
        "/* 自动生成，请勿手改 —— tools/captcha_train/quantize_model.py",
        " *",
        " * int16 量化权重：体积是 float32 版的一半，每个张量一个缩放系数。",
        f" * 字符集 \"{charset}\"，{NDIGIT} 位，输入 {W}x{H}",
        " */",
        "#ifndef PORTAL_CAPTCHA_MODEL_Q_H",
        "#define PORTAL_CAPTCHA_MODEL_Q_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define CM_IN_H   {H}",
        f"#define CM_IN_W   {W}",
        f"#define CM_C1     {C1}",
        f"#define CM_C2     {C2}",
        f"#define CM_FC     {FC}",
        f"#define CM_NCLS   {NCLS}",
        f"#define CM_NDIGIT {NDIGIT}",
        f"#define CM_PH1    {H // 2}",
        f"#define CM_PW1    {W // 2}",
        f"#define CM_PH2    {H // 4}",
        f"#define CM_PW2    {W // 4}",
        f"#define CM_FLAT   {FLAT}",
        "",
    ]
    for (_, cname), (q, scale) in zip(NAMES, tensors):
        lines.append(f"/* {cname}: {tuple(q.shape)}  量化系数 {scale:.9e} */")
        lines.append(f"#define {cname}_S {scale:.9e}f")
        lines.append(f"static const int16_t {cname}[{q.size}] = {{")
        row = []
        for v in q.ravel():
            row.append("%d" % int(v))
            if len(row) == 24:
                lines.append("    " + ",".join(row) + ",")
                row = []
        if row:
            lines.append("    " + ",".join(row) + ",")
        lines.append("};")
        lines.append("")
    lines.append("#endif")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def emit_blob(path: str, tensors) -> int:
    """产出可直接烧到 storage 分区的模型包（固件要求 8 个张量）"""
    if len(tensors) != 8:
        raise SystemExit("固件要求正好 8 个张量")
    hdr = bytearray(b"ESL1")
    hdr += struct.pack("<I", len(tensors))
    for q, scale in tensors:
        hdr += struct.pack("<If", q.size, scale)
    hdr += b"\x00" * ((8 - len(hdr) % 8) % 8)      # 8 字节对齐
    with open(path, "wb") as f:
        f.write(hdr)
        for q, _ in tensors:
            f.write(q.tobytes())
    return os.path.getsize(path)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--npz", default=os.path.join(HERE, "out", "weights.npz"))
    ap.add_argument("--meta", default=None, help="默认取 npz 同目录的 meta.json")
    ap.add_argument("--out", default=None, help="输出目录，默认 npz 同目录")
    ap.add_argument("--size", default=None, help="验证码宽x高，覆盖 meta.json")
    ap.add_argument("--chars", default=None, help="字符集，覆盖 meta.json")
    args = ap.parse_args()

    if not os.path.exists(args.npz):
        raise SystemExit(f"找不到权重 {args.npz}\n先跑 python train.py 训练")

    meta_path = args.meta or os.path.join(os.path.dirname(args.npz), "meta.json")
    meta = {}
    if os.path.exists(meta_path):
        with open(meta_path, encoding="utf-8") as f:
            meta = json.load(f)

    size = args.size or (f"{meta['shape'][1]}x{meta['shape'][0]}" if meta.get("shape") else None)
    if not size:
        raise SystemExit("不知道验证码尺寸：给 --size 宽x高（如 62x25），或让 meta.json 带上 shape")
    try:
        ws, hs = size.lower().split("x")
        W, H = int(ws), int(hs)
    except ValueError:
        raise SystemExit("--size 要写成 宽x高，例如 62x25")
    charset = args.chars or meta.get("charset")
    if not charset:
        raise SystemExit("不知道字符集：给 --chars（如 0123456789）")

    out_dir = os.path.abspath(args.out or os.path.dirname(args.npz))
    os.makedirs(out_dir, exist_ok=True)

    z = np.load(args.npz)
    missing = [k for k, _ in NAMES if k not in z]
    if missing:
        raise SystemExit(f"npz 里缺张量：{missing}")

    tensors = []
    for key, _ in NAMES:
        q, scale = quantize_tensor(z[key])
        tensors.append((q, scale))

    h_path = os.path.join(out_dir, "captcha_model_q.h")
    emit_header(h_path, tensors, (H, W), charset)
    bin_path = os.path.join(out_dir, "model_q.bin")
    n = emit_blob(bin_path, tensors)

    total = sum(q.size for q, _ in tensors)
    print(f"已生成 {h_path}")
    print(f"  参数 {total:,} 个  int16 = {total * 2:,} 字节（{total * 2 / 1024:.0f} KB）")
    print(f"  字符集 \"{charset}\"，{len(charset)} 类，输入 {W}x{H}")
    print(f"已生成 {bin_path}  {n:,} 字节")
    print("  烧录：python -m esptool --chip esp32s3 -p COM5 -b 921600 "
          "write_flash 0xC10000 model_q.bin")
    print("  （0xC10000 是 storage 分区起始地址，见 partitions.csv）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
