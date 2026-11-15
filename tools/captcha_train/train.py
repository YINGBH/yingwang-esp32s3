# -*- coding: utf-8 -*-
"""
验证码小 CNN 训练器（纯 numpy，不依赖 torch / tensorflow）

默认参数对应「颖网」那套联通门户的验证码：
    输入 1x25x62（灰度）
    conv 16@3x3 + ReLU + maxpool2  -> 16x12x31
    conv 32@3x3 + ReLU + maxpool2  -> 32x6x15
    flatten 2880 -> fc 64 + ReLU
    4 个输出头，每头 9 类（字符集 0-8）

换别的学校时，字符集 / 图片尺寸用命令行改，一般不用动代码：
    python train.py --chars 0123456789 --size 60x24 --ink dark --data ../我的数据

硬限制：**输出固定 4 位**（w4 的形状、reshape、loss 循环里都写死了 4）。
3 位 / 5 位的验证码要改三处：Net.__init__ 的 w4、forward 里 reshape(1,4,NCLS)、
loss_and_grad 里 range(NDIGIT)。

用法：
    python train.py                        # 训练 + 评估（默认 40 轮）
    python train.py --gradcheck            # 先做数值梯度校验
    python train.py --epochs 60 --export-c # 另外导出 C 头文件
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import sys
import time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))          # 仓库根目录
DATA = os.path.join(ROOT, "data", "captcha")           # 放 labels.csv 和 samples/
OUT = os.path.join(HERE, "out")                        # 权重和 meta.json
LABELS = os.path.join(DATA, "labels.csv")
SAMPLES = os.path.join(DATA, "samples")

CHARSET = "012345678"      # 实测该门户验证码只有 0-8
NCLS = len(CHARSET)
H, W = 25, 62
NDIGIT = 4

# 网络规模（改这里即可，导出 C 时会自动适配）
C1, C2, FC = 16, 32, 64
FLAT = C2 * 6 * 15


# ==========================================================================
# 数据
# ==========================================================================
def load_dataset(limit: int | None = None, ink: str = "red", resize: bool = False):
    if not os.path.exists(LABELS):
        raise SystemExit(f"找不到 {LABELS}\n"
                         "先用 tools/captcha_train/harvest.py 采样本，或用 --data 指定目录")

    xs, ys = [], []
    skipped_size = skipped_code = 0
    with open(LABELS, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            code = row["code"].strip()
            if len(code) != NDIGIT or any(c not in CHARSET for c in code):
                skipped_code += 1
                continue
            path = os.path.join(SAMPLES, row["file"])
            if not os.path.exists(path):
                continue
            im = Image.open(path).convert("RGB")
            if im.size != (W, H):
                if not resize:
                    skipped_size += 1
                    continue
                im = im.resize((W, H), Image.LANCZOS)
            a = np.asarray(im).astype(np.float32)
            if ink == "dark":
                # 黑字 / 深色字白底：取「暗度」= 1 - 亮度
                g = 1.0 - a.mean(axis=2) / 255.0
            else:
                # 红字白底：取「红色富余」= R - max(G,B)，红字≈1，白底和彩色噪点≈0
                # （直接取 R 通道没用：白底的 R 也是 255）
                g = np.clip((a[:, :, 0] - np.maximum(a[:, :, 1], a[:, :, 2])) / 255.0, 0.0, 1.0)
            xs.append(g)
            ys.append([CHARSET.index(c) for c in code])
            if limit and len(xs) >= limit:
                break

    if not xs:
        raise SystemExit(
            "没有可用样本：\n"
            f"  尺寸不是 {W}x{H} 的 {skipped_size} 张（可用 --resize 强转）\n"
            f"  标签不是 {NDIGIT} 位或含字符集 '{CHARSET}' 之外字符的 {skipped_code} 条\n"
            "检查 --size / --chars 是否和实际验证码一致")
    if skipped_size or skipped_code:
        print(f"（跳过：尺寸不符 {skipped_size} 张、标签不合字符集 {skipped_code} 条）")

    X = np.stack(xs)[:, None, :, :].astype(np.float32)      # (N,1,25,62)
    Y = np.array(ys, dtype=np.int64)                        # (N,4)
    return X, Y


def augment(X, Y, rng):
    """随机平移 1~2 像素，模拟字符位置抖动。

    注：试过加 ±7° 随机旋转，整码准确率从 76.7% 掉到 48%
    （验证集是干净图像，旋转属于分布外扰动），因此不采用。
    """
    out = np.empty_like(X)
    for i in range(len(X)):
        img = X[i, 0]
        dx, dy = int(rng.integers(-2, 3)), int(rng.integers(-1, 2))
        img = np.roll(img, (dy, dx), axis=(0, 1))
        if dy > 0:
            img[:dy, :] = 0
        elif dy < 0:
            img[dy:, :] = 0
        if dx > 0:
            img[:, :dx] = 0
        elif dx < 0:
            img[:, dx:] = 0
        out[i, 0] = img
    return out, Y


# ==========================================================================
# 基础算子
# ==========================================================================
def conv_forward(x, w, b, pad=1):
    N, C, Hh, Ww = x.shape
    O, _, kh, kw = w.shape
    xp = np.pad(x, ((0, 0), (0, 0), (pad, pad), (pad, pad)))
    Ho, Wo = Hh + 2 * pad - kh + 1, Ww + 2 * pad - kw + 1
    out = np.zeros((N, O, Ho, Wo), dtype=x.dtype)
    for ky in range(kh):
        for kx in range(kw):
            patch = xp[:, :, ky:ky + Ho, kx:kx + Wo]
            # w[:, :, ky, kx] 形状是 (O, C)，所以是 "oc" 不是 "co"
            out += np.einsum("nchw,oc->nohw", patch, w[:, :, ky, kx], optimize=True)
    out += b.reshape(1, O, 1, 1)
    return out, (xp, w, pad)


def conv_backward(dout, cache):
    xp, w, pad = cache
    N, C, Hp, Wp = xp.shape
    O, _, kh, kw = w.shape
    Ho, Wo = dout.shape[2], dout.shape[3]
    dw = np.zeros_like(w, dtype=dout.dtype)
    dxp = np.zeros_like(xp)
    for ky in range(kh):
        for kx in range(kw):
            patch = xp[:, :, ky:ky + Ho, kx:kx + Wo]
            dw[:, :, ky, kx] = np.einsum("nohw,nchw->oc", dout, patch, optimize=True)
            dxp[:, :, ky:ky + Ho, kx:kx + Wo] += np.einsum(
                "nohw,oc->nchw", dout, w[:, :, ky, kx], optimize=True)
    db = dout.sum(axis=(0, 2, 3)).astype(np.float32)
    Hh, Ww = Hp - 2 * pad, Wp - 2 * pad
    return dxp[:, :, pad:pad + Hh, pad:pad + Ww], dw, db


def maxpool_forward(x):
    N, C, Hh, Ww = x.shape
    H2, W2 = Hh // 2, Ww // 2
    xc = x[:, :, :H2 * 2, :W2 * 2]
    xr = xc.reshape(N, C, H2, 2, W2, 2).transpose(0, 1, 2, 4, 3, 5).reshape(N, C, H2, W2, 4)
    idx = xr.argmax(axis=-1)
    out = np.take_along_axis(xr, idx[..., None], axis=-1)[..., 0]
    return out, (x.shape, idx)


def maxpool_backward(dout, cache):
    (N, C, Hh, Ww), idx = cache
    H2, W2 = Hh // 2, Ww // 2
    dxr = np.zeros((N, C, H2, W2, 4), dtype=dout.dtype)
    np.put_along_axis(dxr, idx[..., None], dout[..., None], axis=-1)
    dxc = dxr.reshape(N, C, H2, W2, 2, 2).transpose(0, 1, 2, 4, 3, 5).reshape(N, C, H2 * 2, W2 * 2)
    dx = np.zeros((N, C, Hh, Ww), dtype=dout.dtype)
    dx[:, :, :H2 * 2, :W2 * 2] = dxc
    return dx


# ==========================================================================
# 网络
# ==========================================================================
class Net:
    def __init__(self, rng):
        self.p = {
            "w1": (rng.standard_normal((C1, 1, 3, 3)) * np.sqrt(2 / 9)).astype(np.float32),
            "b1": np.zeros(C1, np.float32),
            "w2": (rng.standard_normal((C2, C1, 3, 3)) * np.sqrt(2 / (C1 * 9))).astype(np.float32),
            "b2": np.zeros(C2, np.float32),
            "w3": (rng.standard_normal((FLAT, FC)) * np.sqrt(2 / FLAT)).astype(np.float32),
            "b3": np.zeros(FC, np.float32),
            "w4": (rng.standard_normal((4, FC, NCLS)) * np.sqrt(2 / FC)).astype(np.float32),
            "b4": np.zeros((4, NCLS), np.float32),
        }

    def forward(self, x):
        c = {}
        o1, c["c1"] = conv_forward(x, self.p["w1"], self.p["b1"])
        a1 = np.maximum(o1, 0)
        p1, c["p1"] = maxpool_forward(a1)

        o2, c["c2"] = conv_forward(p1, self.p["w2"], self.p["b2"])
        a2 = np.maximum(o2, 0)
        p2, c["p2"] = maxpool_forward(a2)

        flat = p2.reshape(len(x), -1)
        o3 = flat @ self.p["w3"] + self.p["b3"]
        a3 = np.maximum(o3, 0)

        # (N,4,9)
        logits = np.einsum("nf,ofc->noc", a3, self.p["w4"]) + self.p["b4"].reshape(1, 4, NCLS)
        return logits, (c, x, a1, p1, a2, p2, flat, o3, a3)

    def loss_and_grad(self, x, y):
        logits, cache = self.forward(x)
        c, x_in, a1, p1, a2, p2, flat, o3, a3 = cache
        N = len(x)

        z = logits - logits.max(axis=-1, keepdims=True)
        e = np.exp(z)
        prob = e / e.sum(axis=-1, keepdims=True)

        idx = np.arange(N)
        loss = 0.0
        for k in range(NDIGIT):
            loss -= np.log(prob[idx, k, y[:, k]] + 1e-9).mean()

        dlogits = prob.copy()
        for k in range(NDIGIT):
            dlogits[idx, k, y[:, k]] -= 1.0
        dlogits /= N

        g = {}
        g["w4"] = np.einsum("noc,nf->ofc", dlogits, a3, optimize=True)
        g["b4"] = dlogits.sum(axis=0)
        da3 = np.einsum("noc,ofc->nf", dlogits, self.p["w4"], optimize=True)

        do3 = da3 * (o3 > 0)
        g["w3"] = flat.T @ do3
        g["b3"] = do3.sum(axis=0)
        dflat = do3 @ self.p["w3"].T

        dp2 = dflat.reshape(p2.shape)
        da2 = maxpool_backward(dp2, c["p2"])
        do2 = da2 * (a2 > 0)
        dp1, g["w2"], g["b2"] = conv_backward(do2, c["c2"])

        da1 = maxpool_backward(dp1, c["p1"])
        do1 = da1 * (a1 > 0)
        _, g["w1"], g["b1"] = conv_backward(do1, c["c1"])

        return float(loss), g

    def predict(self, x, bs=256):
        outs = []
        for i in range(0, len(x), bs):
            logits, _ = self.forward(x[i:i + bs])
            outs.append(logits.argmax(axis=-1))
        return np.concatenate(outs)


# ==========================================================================
# Adam
# ==========================================================================
class Adam:
    def __init__(self, params, lr=1e-3):
        self.lr, self.b1, self.b2, self.eps = lr, 0.9, 0.999, 1e-8
        self.m = {k: np.zeros_like(v) for k, v in params.items()}
        self.v = {k: np.zeros_like(v) for k, v in params.items()}
        self.t = 0

    def step(self, params, grads):
        self.t += 1
        for k in params:
            g = grads[k]
            self.m[k] = self.b1 * self.m[k] + (1 - self.b1) * g
            self.v[k] = self.b2 * self.v[k] + (1 - self.b2) * (g * g)
            mh = self.m[k] / (1 - self.b1 ** self.t)
            vh = self.v[k] / (1 - self.b2 ** self.t)
            params[k] -= self.lr * mh / (np.sqrt(vh) + self.eps)


# ==========================================================================
# 梯度校验
# ==========================================================================
def gradcheck():
    print("数值梯度校验…")
    rng = np.random.default_rng(0)
    net = Net(rng)
    for k in net.p:
        net.p[k] = net.p[k].astype(np.float64)
    x = rng.random((4, 1, H, W))
    y = rng.integers(0, NCLS, (4, NDIGIT))

    loss, g = net.loss_and_grad(x, y)
    ok = True
    for name in ("w1", "b1", "w4", "b4", "w3"):
        p = net.p[name]
        idx = tuple(rng.integers(0, s) for s in p.shape)
        eps = 1e-6
        old = p[idx]
        p[idx] = old + eps
        lp, _ = net.loss_and_grad(x, y)
        p[idx] = old - eps
        lm, _ = net.loss_and_grad(x, y)
        p[idx] = old
        num = (lp - lm) / (2 * eps)
        ana = g[name][idx]
        rel = abs(num - ana) / max(1e-12, abs(num) + abs(ana))
        flag = "OK " if rel < 1e-4 else "BAD"
        print(f"  {flag} {name}{idx}  数值={num:+.6e} 解析={ana:+.6e} rel={rel:.2e}")
        ok &= rel < 1e-4
    print("梯度校验", "通过" if ok else "失败")
    return 0 if ok else 1


# ==========================================================================
# 导出
# ==========================================================================
def export_c(net, path):
    """导出 float32 权重头文件（放 Flash 即可，S3 上计算够快）。"""
    p = net.p
    lines = ["/* 自动生成，请勿手改：python 训练/train.py --export-c */",
             "#ifndef PORTAL_CAPTCHA_MODEL_H",
             "#define PORTAL_CAPTCHA_MODEL_H",
             "",
             f"#define CM_IN_H   {H}",
             f"#define CM_IN_W   {W}",
             f"#define CM_C1     {C1}",
             f"#define CM_C2     {C2}",
             f"#define CM_FC     {FC}",
             f"#define CM_NCLS   {NCLS}",
             f"#define CM_NDIGIT {NDIGIT}",
             "",
             "/* 内部尺寸：25x62 -pool-> 12x31 -pool-> 6x15 */",
             f"#define CM_PH1    {(H // 2)}",
             f"#define CM_PW1    {(W // 2)}",
             f"#define CM_PH2    {(H // 4)}",
             f"#define CM_PW2    {(W // 4)}",
             f"#define CM_FLAT   {FLAT}",
             ""]

    for name in ("w1", "b1", "w2", "b2", "w3", "b3", "w4", "b4"):
        arr = np.asarray(p[name], dtype=np.float32).ravel()
        lines.append(f"/* {name}: shape {tuple(p[name].shape)} */")
        lines.append(f"static const float CM_{name.upper()}[{len(arr)}] = {{")
        for i in range(0, len(arr), 8):
            lines.append("  " + ", ".join(f"{v:.7e}f" for v in arr[i:i + 8]) + ",")
        lines.append("};")
        lines.append("")

    lines.append("#endif")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"已导出 C 头文件: {path}")


# ==========================================================================
# 主流程
# ==========================================================================
def main() -> int:
    global CHARSET, NCLS, H, W, FLAT, DATA, LABELS, SAMPLES, OUT
    ap = argparse.ArgumentParser()
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--chars", default=CHARSET, help="字符集，如 0123456789")
    ap.add_argument("--size", default=f"{W}x{H}", help="验证码宽x高，如 62x25")
    ap.add_argument("--ink", choices=("red", "dark"), default="red",
                    help="red=红字白底（取红色富余）；dark=深色字白底（取暗度）")
    ap.add_argument("--resize", action="store_true", help="尺寸不符时强转到 --size")
    ap.add_argument("--data", default=DATA, help="样本目录（含 labels.csv 和 samples/）")
    ap.add_argument("--out", default=OUT, help="权重输出目录")
    ap.add_argument("--gradcheck", action="store_true")
    ap.add_argument("--export-c", action="store_true")
    args = ap.parse_args()

    if args.gradcheck:
        return gradcheck()

    # 应用命令行给的形状 / 字符集（Net 和导出都读这些全局量）
    CHARSET = args.chars
    NCLS = len(CHARSET)
    try:
        ws, hs = args.size.lower().split("x")
        W, H = int(ws), int(hs)
    except ValueError:
        raise SystemExit("--size 要写成 宽x高，例如 62x25")
    if H < 8 or W < 8:
        raise SystemExit(f"--size {W}x{H} 太小：要经得起两次 2x2 池化")
    # 池化是向下取整，所以 25x62 这种不整除的尺寸也能用
    FLAT = C2 * (H // 4) * (W // 4)

    DATA = os.path.abspath(args.data)
    LABELS = os.path.join(DATA, "labels.csv")
    SAMPLES = os.path.join(DATA, "samples")
    OUT = os.path.abspath(args.out)

    print("加载数据…")
    X, Y = load_dataset(args.limit, ink=args.ink, resize=args.resize)
    print(f"样本 {len(X)}，字符集 '{CHARSET}'，{W}x{H}")

    rng = np.random.default_rng(42)
    perm = rng.permutation(len(X))
    X, Y = X[perm], Y[perm]
    n_val = max(50, int(len(X) * 0.15))
    Xtr, Ytr = X[:-n_val], Y[:-n_val]
    Xva, Yva = X[-n_val:], Y[-n_val:]
    print(f"训练 {len(Xtr)} / 验证 {len(Xva)}\n")

    net = Net(np.random.default_rng(1))
    opt = Adam(net.p, lr=args.lr)
    aug_rng = np.random.default_rng(7)

    best = 0.0
    t0 = time.time()
    for ep in range(1, args.epochs + 1):
        idx = aug_rng.permutation(len(Xtr))
        tot = 0.0
        for i in range(0, len(idx), args.batch):
            b = idx[i:i + args.batch]
            xb, yb = augment(Xtr[b], Ytr[b], aug_rng)
            loss, g = net.loss_and_grad(xb, yb)
            opt.step(net.p, g)
            tot += loss * len(b)
        opt.lr = args.lr * (0.5 ** (ep // 20))

        pred = net.predict(Xva)
        digit_acc = (pred == Yva).mean()
        string_acc = (pred == Yva).all(axis=1).mean()
        if string_acc > best:
            best = string_acc
        if ep % 5 == 0 or ep == 1:
            print(f"  epoch {ep:>3}  loss {tot/len(Xtr):.4f}  "
                  f"单字 {digit_acc*100:5.1f}%  整码 {string_acc*100:5.1f}%")

    pred = net.predict(Xva)
    print(f"\n最终：单字 {(pred==Yva).mean()*100:.2f}%   整码 {(pred==Yva).all(axis=1).mean()*100:.2f}%"
          f"   （最好 {best*100:.2f}%）  用时 {time.time()-t0:.0f}s")

    os.makedirs(OUT, exist_ok=True)
    np.savez(os.path.join(OUT, "weights.npz"), **net.p)
    with open(os.path.join(OUT, "meta.json"), "w", encoding="utf-8") as f:
        json.dump({"charset": CHARSET, "shape": [H, W], "ncls": NCLS,
                   "val_string_acc": float((pred == Yva).all(axis=1).mean()),
                   "val_digit_acc": float((pred == Yva).mean())}, f, ensure_ascii=False, indent=2)
    print(f"权重已保存: {OUT}")

    if args.export_c:
        export_c(net, os.path.join(OUT, "captcha_model.h"))

    return 0


if __name__ == "__main__":
    sys.exit(main())