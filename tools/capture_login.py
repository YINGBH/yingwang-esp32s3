#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
抓校园网登录（电脑端，零依赖）

两条路子：

  1) probe —— 自动探测门户参数，不需要登录、不需要代理
     连上校园网（还没认证）后跑一次，它会把 wlanuserip / wlanacip / SSID /
     门户地址 抓出来，并直接打印能粘进串口/网页后台的 SET 命令，
     顺便试一下门户是不是 *.wlan 那套（决定要不要改代码）。

  2) proxy —— 本地 HTTP 代理，记录浏览器真登录时发的请求
     不知道新学校门户长什么样时用：把浏览器代理设成 127.0.0.1:8888，
     手动登录一次，脚本会把请求行、表单字段、Set-Cookie 全记下来
     （密码字段自动打码），再对照着改 components/portal/。

用法：
    python tools/capture_login.py probe
    python tools/capture_login.py probe --json portal.json
    python tools/capture_login.py proxy --port 8888
    python tools/capture_login.py check

退出码：0 成功 / 1 没抓到（在线或不在校园网）/ 2 参数错误
"""
from __future__ import annotations

import argparse
import json
import os
import re
import socket
import sys
import time
import urllib.parse
import urllib.request

TIMEOUT = 6
UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/124.0 Safari/537.36")

# 这些地址正常情况下应该“直通”，被拦/被跳转就说明有门户
PROBES = [
    "http://connectivitycheck.platform.hicloud.com/generate_204",
    "http://www.msftconnecttest.com/connecttest.txt",
    "http://cn.bing.com/",
    "http://www.baidu.com/",
]

KEYS = ("wlanuserip", "wlanacip", "wlanacname", "ssid", "nasip", "authip")

OUT_DIR = "capture"


def _fix_console_encoding() -> None:
    """让输出编码跟随控制台代码页。

    Windows 中文控制台是 936（GBK），UTF-8 控制台是 65001。Python 默认按 locale
    选编码，遇到 chcp 65001 或打包成 exe 时容易和实际控制台对不上，
    中文就变乱码。这里直接问系统要代码页，然后 reconfigure。
    """
    if os.name != "nt":
        return
    try:
        import ctypes
        cp = ctypes.windll.kernel32.GetConsoleOutputCP()
        if not cp:
            return
        enc = f"cp{cp}"
        for stream in (sys.stdout, sys.stderr):
            try:
                stream.reconfigure(encoding=enc, errors="replace")
            except Exception:
                pass
    except Exception:
        pass


_fix_console_encoding()


# ────────────────────────── 小工具 ──────────────────────────

class NoRedirect(urllib.request.HTTPRedirectHandler):
    """自己处理 302，才能拿到 Location"""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


OPENER = urllib.request.build_opener(NoRedirect)


def http(url, method="GET", body=None, timeout=TIMEOUT):
    """返回 (status, headers, body_bytes)；失败抛异常"""
    req = urllib.request.Request(url, data=body, method=method)
    req.add_header("User-Agent", UA)
    req.add_header("Accept", "*/*")
    if body:
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with OPENER.open(req, timeout=timeout) as r:
            return r.status, dict(r.headers), r.read(65536)
    except urllib.error.HTTPError as e:      # 3xx/4xx/5xx 都从这里出来
        return e.code, dict(e.headers), e.read(65536) if e.fp else b""


def mask(s: str, keep_head=3, keep_tail=4) -> str:
    """账号打码：13800000000 -> 138****0000"""
    if not s:
        return s
    if len(s) <= keep_head + keep_tail:
        return "*" * len(s)
    return s[:keep_head] + "****" + s[-keep_tail:]


def parse_params(text: str) -> dict:
    """从 URL / HTML / JS 里抠门户参数（大小写不敏感）"""
    found = {}
    low = text.lower()
    for key in KEYS:
        # 形如 wlanuserip=1.2.3.4 / "wlanuserip":"1.2.3.4" / wlanuserip%3D1.2.3.4
        pat = re.compile(
            r"(?:^|[?&\"'%3d]|=|:)\s*" + key + r"\s*(?:=|%3d|\"|'|:)\s*([\w\.:\-]+)",
            re.I)
        m = pat.search(text) or pat.search(urllib.parse.unquote(text))
        if m:
            found[key] = m.group(1)
    if not found:
        return found
    return found


def portal_base_from(url: str) -> str:
    u = urllib.parse.urlsplit(url)
    if not u.scheme or not u.netloc:
        return ""
    port = f":{u.port}" if u.port and u.port not in (80, 443) else ""
    return f"{u.scheme}://{u.hostname}{port}"


def is_portalish(text: str) -> bool:
    t = text.lower()
    return any(k in t for k in ("wlanuserip", "portalLogin".lower(), "wlanacip",
                                "portalforceLogin".lower(), "wlan.do"))


# ────────────────────────── probe ──────────────────────────

def probe(json_out: str | None) -> int:
    print("=" * 60)
    print("① 探测当前网络有没有被门户拦")
    print("=" * 60)

    hit = None            # (url, status, headers, body)
    for url in PROBES:
        try:
            st, hd, body = http(url)
        except Exception as e:
            print(f"  {url}\n    -> 连不上（{type(e).__name__}）")
            continue

        loc = hd.get("Location", "")
        text = body.decode("utf-8", "replace")
        print(f"  {url}\n    -> HTTP {st}" + (f"，跳到 {loc}" if loc else ""))

        is_portal = False
        if st in (301, 302, 303, 307, 308) and loc:
            # 同一主机只是升级到 https（bing/baidu 常见）—— 不是门户
            same_host = (urllib.parse.urlsplit(loc).netloc.lower()
                         == urllib.parse.urlsplit(url).netloc.lower())
            if same_host and not is_portalish(loc) and not parse_params(loc):
                print("    -> 只是升级到 https，继续")
            elif is_portalish(loc) or parse_params(loc) or "wlan" in loc.lower():
                is_portal = True
            else:
                print("    -> 跳到别的地址，但看不出门户特征，继续")
        elif st == 200 and is_portalish(text):
            is_portal = True
        elif st == 204:
            print("    -> 204：这条通了，说明此刻是联网状态")

        if is_portal:
            hit = (url, st, hd, body)
            break

    if hit is None:
        print("\n结论：**没抓到门户**。")
        print("  可能原因：① 已经认证过（先断网/换设备再试）")
        print("           ② 不在校园网里（连上校园 WiFi 再来）")
        print("           ③ 这个门户不拦 HTTP，只拦 DNS —— 试试 `proxy` 模式手动登录一次")
        return 1

    url, st, hd, body = hit
    loc = hd.get("Location", "")
    html = body.decode("utf-8", "replace")

    print("\n" + "=" * 60)
    print("② 抓参数")
    print("=" * 60)

    params = {}
    if loc:
        params.update(parse_params(loc))
    params.update(parse_params(html))

    # 有些门户把参数放在登录页里，跟着跳一次能拿全
    if loc and len(params) < 3:
        try:
            st2, hd2, body2 = http(loc)
            params.update(parse_params(body2.decode("utf-8", "replace")))
            print(f"  跟到登录页：HTTP {st2}")
        except Exception as e:
            print(f"  跟到登录页失败：{type(e).__name__}")

    base = portal_base_from(loc) or portal_base_from(url)
    if not base and params.get("wlanacip"):
        base = f"http://{params['wlanacip']}:8090"      # 常见默认
        print(f"  没解析到门户地址，按常见端口猜一个：{base}")

    for k in ("wlanuserip", "wlanacip", "wlanacname", "ssid"):
        print(f"  {k:12s} = {params.get(k, '(没抓到)')}")
    print(f"  {'门户地址':10s} = {base or '(没抓到)'}")

    # ── ③ 门户能力探测：是不是 *.wlan 那套
    print("\n" + "=" * 60)
    print("③ 判断这是不是 *.wlan 那套门户（决定要不要改代码）")
    print("=" * 60)
    caps = {}
    if base:
        for path in ("/index.wlan", "/generateVerifyCode.wlan", "/portalLogin.wlan"):
            try:
                st3, _, _ = http(base + path, timeout=5)
                caps[path] = st3
                # 只有 200 / 3xx 才算"这套接口存在"；404 是"没有这个路径"
                exists = st3 < 400
                print(f"  {base + path:52s} HTTP {st3}  {'存在' if exists else '不存在'}")
            except Exception as e:
                caps[path] = None
                print(f"  {base + path:52s} 失败（{type(e).__name__}）")

        # 抓一张验证码看看形状
        try:
            st4, hd4, png = http(base + "/generateVerifyCode.wlan", timeout=6)
            if st4 == 200 and b"PNG" in png[:8]:
                os.makedirs(OUT_DIR, exist_ok=True)
                fp = os.path.join(OUT_DIR, "captcha.png")
                open(fp, "wb").write(png)
                print(f"  验证码已存到 {fp}（{len(png)} 字节，用图片查看器打开看尺寸/字符集）")
        except Exception:
            pass

    is_wlan = any(v is not None and v < 400 for v in caps.values()) if caps else False

    # ── ④ 输出
    print("\n" + "=" * 60)
    print("④ 结果")
    print("=" * 60)
    if is_wlan:
        print("  ✅ 看着就是 *.wlan 这套：参数填进去就能用，验证码一样才需要重训模型")
    else:
        print("  ⚠️ 没探到 *.wlan 接口：可能不是同一套门户，需要 `proxy` 模式手动登录一次看请求")

    print("\n  串口里这样填（网页后台也能填同样的值）：")
    if base:
        print(f"    SET portal_base={base}")
    if params.get("wlanacip"):
        print(f"    SET portal_ac={params['wlanacip']}")
    if params.get("ssid"):
        print(f"    SET portal_ssid={params['ssid']}")
    print("\n  或者写进 app_config.h：")
    if base:
        print(f'    #define APP_PORTAL_BASE "{base}"')
    if params.get("wlanacip"):
        print(f'    #define APP_PORTAL_AC_IP "{params["wlanacip"]}"')
    if params.get("ssid"):
        print(f'    #define APP_PORTAL_SSID "{params["ssid"]}"')

    if json_out:
        data = {
            "probed_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            "probe_url": url,
            "status": st,
            "location": loc,
            "portal_base": base,
            "params": params,
            "caps": caps,
            "is_wlan_portal": is_wlan,
        }
        with open(json_out, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        print(f"\n  JSON 已写入 {json_out}")

    return 0 if (base or params) else 1


# ────────────────────────── proxy ──────────────────────────

def proxy(port: int, out: str) -> int:
    """本地 HTTP 代理：记录浏览器登录请求（密码打码）"""
    import http.client
    import socketserver
    import threading
    from http.server import BaseHTTPRequestHandler

    os.makedirs(OUT_DIR, exist_ok=True)
    log_path = out or os.path.join(OUT_DIR, f"proxy-{time.strftime('%Y%m%d-%H%M%S')}.log")
    lock = threading.Lock()
    logf = open(log_path, "a", encoding="utf-8")

    SECRET_KEYS = ("userpwd", "password", "passwd", "pwd", "pass")

    def note(text: str):
        with lock:
            print(text)
            logf.write(text + "\n")
            logf.flush()

    def scrub(body: bytes) -> str:
        txt = body.decode("utf-8", "replace")
        for k in SECRET_KEYS:
            txt = re.sub(rf"({k}=)[^&]*", r"\1***", txt, flags=re.I)
        return txt

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass

        def _forward(self):
            url = self.path
            if not url.startswith("http://"):
                self.send_error(400, "只支持明文 HTTP")
                return
            u = urllib.parse.urlsplit(url)
            body = None
            if "Content-Length" in self.headers:
                body = self.rfile.read(int(self.headers["Content-Length"]))

            note(f"\n>>> {time.strftime('%H:%M:%S')} {self.command} {url}")
            interesting = {k: v for k, v in self.headers.items()
                           if k.lower() in ("referer", "cookie", "content-type", "host")}
            for k, v in interesting.items():
                note(f"    {k}: {v}")
            if body:
                note(f"    body: {scrub(body)}")

            conn = http.client.HTTPConnection(u.hostname, u.port or 80, timeout=20)
            path = (u.path or "/") + (("?" + u.query) if u.query else "")
            headers = {k: v for k, v in self.headers.items()
                       if k.lower() not in ("proxy-connection", "connection", "host")}
            headers["Host"] = u.netloc
            try:
                conn.request(self.command, path, body=body, headers=headers)
                resp = conn.getresponse()
                data = resp.read()
            except Exception as e:
                note(f"    !! 上游失败 {e}")
                self.send_error(502, "upstream failed")
                return
            finally:
                conn.close()

            note(f"    <<< HTTP {resp.status}")
            for k, v in resp.getheaders():
                if k.lower() in ("set-cookie", "location"):
                    note(f"    {k}: {v}")

            self.send_response(resp.status)
            for k, v in resp.getheaders():
                if k.lower() in ("transfer-encoding", "connection", "content-length"):
                    continue
                self.send_header(k, v)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        do_GET = _forward
        do_POST = _forward

    class Server(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        daemon_threads = True

    try:
        srv = Server(("0.0.0.0", port), Handler)
    except OSError as e:
        print(f"端口 {port} 起不来：{e}（换 --port 试试）")
        return 2

    print("=" * 60)
    print(f"代理已启动：127.0.0.1:{port}")
    print(f"日志：{log_path}")
    print("=" * 60)
    print("接下来：")
    print("  1. 浏览器/系统代理设成 127.0.0.1:%d（或手机连同一个 WiFi 手动配代理）" % port)
    print("  2. 浏览器随便打开一个 http 网站，等门户跳出来，正常登录一次")
    print("  3. 回到这个窗口看记录（密码字段已经打成 ***）")
    print("  4. Ctrl+C 结束")
    print()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止。日志在", log_path)
    finally:
        logf.close()
    return 0


# ────────────────────────── check ──────────────────────────

def check() -> int:
    for url in PROBES[:2]:
        try:
            st, hd, _ = http(url, timeout=4)
            loc = hd.get("Location", "")
            if st in (301, 302, 303, 307, 308):
                print(f"{url} -> HTTP {st} 跳到 {loc}")
                print("结论：**需要认证**（被门户拦了）")
            elif st == 204:
                print(f"{url} -> HTTP 204")
                print("结论：**已认证 / 没有门户**")
            else:
                print(f"{url} -> HTTP {st}")
            return 0
        except Exception as e:
            print(f"{url} -> 失败（{type(e).__name__}）")
    print("结论：网络不通（没连上任何网，或 DNS 挂了）")
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(
        description="抓校园网登录：probe 探参数 / proxy 记录登录请求")
    ap.add_argument("mode", nargs="?", default="probe",
                    choices=["probe", "proxy", "check"])
    ap.add_argument("--json", help="probe 结果写到这个 JSON 文件")
    ap.add_argument("--port", type=int, default=8888, help="proxy 监听端口")
    ap.add_argument("--out", help="proxy 日志文件")
    a = ap.parse_args()

    if a.mode == "probe":
        return probe(a.json)
    if a.mode == "proxy":
        return proxy(a.port, a.out)
    return check()


if __name__ == "__main__":
    sys.exit(main())
