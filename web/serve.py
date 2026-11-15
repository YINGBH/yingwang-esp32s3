# -*- coding: utf-8 -*-
"""给 USB 网页工具起一个本地 HTTP 服务。

为什么需要：
  Chrome/Edge 的 Web Serial API 只在**安全上下文**里可用。
  直接双击 index.html（file:// 协议）时，有些浏览器会禁用 navigator.serial，
  表现为页面能打开但"连接串口"按钮点了没反应，或者页面功能不全。
  用 http://127.0.0.1:8765 打开就没这个问题。

用法：
  双击 web\启动网页.cmd（Windows 免安装，用 PowerShell 起服务）
  或手动运行（任何装了 Python 的系统）：
    python web/serve.py
  然后浏览器打开 http://127.0.0.1:8765
"""
import http.server
import os
import socketserver
import sys

PORT = 8765
ROOT = os.path.dirname(os.path.abspath(__file__))


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def end_headers(self):
        # 不缓存，改了页面刷新就能看到
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        super().end_headers()

    def log_message(self, fmt, *args):
        return      # 安静点，别刷屏


def main():
    os.chdir(ROOT)
    socketserver.TCPServer.allow_reuse_address = True
    try:
        with socketserver.TCPServer(("127.0.0.1", PORT), Handler) as httpd:
            print("=" * 56)
            print(" USB 网页工具已启动")
            print(" 浏览器打开： http://127.0.0.1:%d" % PORT)
            print(" 目录：      %s" % ROOT)
            print(" 关闭：      按 Ctrl+C 或直接关掉这个窗口")
            print("=" * 56)
            httpd.serve_forever()
    except OSError as e:
        print("端口 %d 起不来：%s" % (PORT, e))
        print("可能已经有服务在跑，直接用 http://127.0.0.1:%d 试试" % PORT)
        sys.exit(1)


if __name__ == "__main__":
    main()
