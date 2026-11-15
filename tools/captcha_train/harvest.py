# -*- coding: utf-8 -*-
"""验证码样本采集器（自动标注，零人工）—— 换学校时改这里

核心技巧（本项目准确率的真正来源）
--------------------------------
门户是**先校验验证码、再校验账号**。所以用一个不存在的假账号提交：
  * 返回「用户名或密码错误」 -> 说明验证码是对的：这张图 + 刚才猜的答案 = 标注样本
  * 返回「验证码错误」       -> 说明猜错了：丢弃
于是"谁认对了"由服务器免费判卷，不用人工标一张图，也完全不碰真实账号。

⚠️ 这是模板：下面的 CONFIG 必须按你学校的门户改，改完先 --n 20 跑通再放量。
   本仓库没有你的门户，所以这段代码没有实测过。
   注意控制频率（--delay），别把学校门户当压测目标。

用法
----
    python harvest.py --n 20              # 先小批量验证闭环
    python harvest.py --n 200 --delay 0.5 # 放量
    python harvest.py --stats             # 只看已有样本统计

产出（train.py 直接吃）
----
    <data>/labels.csv       file,code,sha1,ts
    <data>/samples/*.png
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import os
import sys
import time

import requests

# ============================ CONFIG：改这里 ============================
# 门户地址（连上校园网后，浏览器被跳到登录页时地址栏里那个 IP:端口）
BASE = "http://10.0.0.1:8090"

# 取验证码 / 提交登录 的路径（F12 → Network 里看）
CAPTCHA_PATH = "/generateVerifyCode.wlan"
LOGIN_PATH = "/portalLogin.wlan"

# 取验证码时带的查询参数（多数门户要带时间戳防缓存，可按需留空）
CAPTCHA_EXTRA = "?_="          # 后面会拼时间戳

# 登录表单字段名（F12 看 POST body 的 key）
FIELD_USER = "userName"        # 账号
FIELD_PASS = "userPwd"         # 密码（原样提交；如果门户要 RSA/加密，先按你学校的算法处理）
FIELD_CODE = "verifyCode"      # 验证码
FIELD_OTHER = {                # 表单里的固定项，照抓包抄
    "loginmode": "static",
}

# 服务器提示语（按你学校的实际文案改，多写几个同义词）
MSG_CAPTCHA_WRONG = ("验证码错误", "验证码不正确", "请输入验证码", "Errorcode: 0001")
MSG_AUTH_FAIL = ("用户名或密码错误", "登录认证失败", "账号或密码错误")

# 假账号：随便一个不存在的用户名，别用真的
FAKE_USER = "zzz_captcha_harvest"
FAKE_PASS = "Test1234"

# 样本输出目录
DATA_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), "data", "captcha")
# ======================================================================


def classify(msg: str) -> str:
    if any(k in msg for k in MSG_CAPTCHA_WRONG):
        return "captcha_wrong"
    if any(k in msg for k in MSG_AUTH_FAIL):
        return "auth_fail"          # 验证码对了
    return "other"


class Portal:
    def __init__(self):
        self.s = requests.Session()
        self.s.headers.update({
            "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                          "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36",
        })
        self.code_id = ""

    def new_captcha(self):
        """取一张验证码，返回 (图片 bytes, code_id)。门户不同，code_id 可能在头/正文里。"""
        ts = int(time.time() * 1000)
        r = self.s.get(BASE + CAPTCHA_PATH + CAPTCHA_EXTRA + str(ts), timeout=10)
        r.raise_for_status()
        # 常见几种：正文直接是 PNG；正文是 JSON 里带 base64 或 id
        if r.content[:8] == b"\x89PNG\r\n\x1a\n":
            return r.content
        try:
            j = r.json()
        except ValueError:
            raise SystemExit("取验证码返回的不是 PNG，也不是 JSON。先看 LOGIN_PATH 抓包："
                             "正文前 300 字节 = " + repr(r.content[:300]))
        # 按你学校门户的字段名改这里
        self.code_id = str(j.get("codeId") or j.get("code_id") or j.get("id") or "")
        import base64
        img = j.get("img") or j.get("image") or j.get("data")
        if not img:
            raise SystemExit(f"JSON 里没找到图片字段，实际字段：{list(j)[:10]}")
        if isinstance(img, str) and img.startswith("data:"):
            img = img.split(",", 1)[1]
        return base64.b64decode(img)

    def submit(self, user: str, pwd: str, code: str) -> str:
        """提交登录，返回服务器提示文字（原样字符串，用于 classify）"""
        data = dict(FIELD_OTHER)
        data[FIELD_USER] = user
        data[FIELD_PASS] = pwd
        data[FIELD_CODE] = code
        if self.code_id:
            data.setdefault("codeId", self.code_id)
        r = self.s.post(BASE + LOGIN_PATH, data=data, timeout=10)
        return r.text


def ensure_dirs(data_dir: str):
    os.makedirs(os.path.join(data_dir, "samples"), exist_ok=True)
    labels = os.path.join(data_dir, "labels.csv")
    if not os.path.exists(labels):
        with open(labels, "w", newline="", encoding="utf-8") as f:
            csv.writer(f).writerow(["file", "code", "sha1", "ts"])


def existing_hashes(labels: str) -> set[str]:
    if not os.path.exists(labels):
        return set()
    with open(labels, encoding="utf-8") as f:
        return {r["sha1"] for r in csv.DictReader(f) if r.get("sha1")}


def stats(data_dir: str):
    labels = os.path.join(data_dir, "labels.csv")
    if not os.path.exists(labels):
        print("还没有任何样本。")
        return
    rows = list(csv.DictReader(open(labels, encoding="utf-8")))
    print(f"样本数: {len(rows)}")
    if not rows:
        return
    from collections import Counter
    print("字符分布:", dict(sorted(Counter(c for r in rows for c in r["code"]).items())))
    print("长度分布:", dict(Counter(len(r["code"]) for r in rows)))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=20, help="本次目标样本数")
    ap.add_argument("--delay", type=float, default=0.5, help="每次间隔秒数（别太快）")
    ap.add_argument("--data", default=DATA_DIR, help="样本输出目录")
    ap.add_argument("--stats", action="store_true", help="只看统计")
    args = ap.parse_args()

    ensure_dirs(args.data)
    if args.stats:
        stats(args.data)
        return 0

    try:
        import ddddocr
    except ImportError:
        print("需要 ddddocr 来产生「猜测」：pip install ddddocr")
        print("（换成别的 OCR 也行，只要能返回 4 位数字）")
        return 2
    ocr = ddddocr.DdddOcr(show_ad=False)

    labels = os.path.join(args.data, "labels.csv")
    samples = os.path.join(args.data, "samples")
    seen = existing_hashes(labels)
    portal = Portal()

    print(f"已有 {len(seen)} 条，本次目标 {args.n} 条")
    print(f"门户 {BASE}  输出 {samples}\n")

    got = tried = 0
    t0 = time.time()
    while got < args.n:
        tried += 1
        try:
            img = portal.new_captcha()
            guess = ocr.classification(img).strip()
            if len(guess) != 4 or not guess.isdigit():
                time.sleep(args.delay)
                continue

            msg = portal.submit(FAKE_USER, FAKE_PASS, guess)
            if classify(msg) == "auth_fail":
                sha = hashlib.sha1(img).hexdigest()
                if sha not in seen:
                    seen.add(sha)
                    name = sha[:16] + ".png"
                    with open(os.path.join(samples, name), "wb") as f:
                        f.write(img)
                    with open(labels, "a", newline="", encoding="utf-8") as f:
                        csv.writer(f).writerow([name, guess, sha, int(time.time())])
                    got += 1
                    print(f"  +{got}/{args.n}  {guess}")
            elif tried % 20 == 0:
                print(f"  尝试 {tried}，命中率 {got / tried * 100:.0f}%")

            time.sleep(args.delay)
        except KeyboardInterrupt:
            print("\n中断。")
            break
        except Exception as e:  # noqa: BLE001
            print(f"  异常：{type(e).__name__}: {e}，2 秒后重试")
            time.sleep(2)

    print(f"\n完成：新增 {got} 条，共尝试 {tried} 次，用时 {time.time() - t0:.0f}s")
    stats(args.data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
