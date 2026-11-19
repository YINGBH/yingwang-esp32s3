# -*- coding: utf-8 -*-
"""网页 ↔ 固件 契约检查（纯静态，不连板子）

对齐三件事：
  1. 网页保存时发的每个键（index.html 的 kv 列表）
  2. 固件 do_set 是否支持该键（usb_console.c）
  3. GET 返回里该键的位置（cfg 对象 / 顶层）

用法：python tools/web_fw_contract.py
发现"缺控件/缺键/缺字段"会以 ← 问题 标出，方便加新配置时自查。
"""

# ---------------------------------------------------------------------------
# -*- coding: utf-8 -*-
"""契约检查（纯静态）：网页发的键 × 固件 do_set 支持 × GET 返回位置"""
import re

W = r"C:\Users\31658\Documents\deepseek-harness\default-workspace\yingwang-opensource"
html = open(W + r"\web\index.html", encoding="utf-8").read()
fw = open(W + r"\components\usb_console\usb_console.c", encoding="utf-8").read()

# ---- 1) 网页 kv 键 + 控件 id ----
save_block = html[html.index("$('btnSave').onclick"):]
kv = {}
for k, expr in re.findall(r"\['([a-z_0-9]+)', ([^\]]+)\]", save_block):
    m = re.search(r"\$\\?\('([a-z_0-9]+)'\)", expr)
    kv[k] = m.group(1) if m else None
for k, expr in re.findall(r"kv\.push\(\['([a-z_0-9]+)', ([^\]]+)\]\)", save_block):
    m = re.search(r"\$\\?\('([a-z_0-9]+)'\)", expr)
    kv[k] = m.group(1) if m else None

# ---- 2) 固件 do_set 支持键 ----
set_block = fw[fw.index("static void do_set"): fw.index("/* 上传看门狗")]
fw_keys = set(re.findall(r'strcmp\(key, "([a-z_0-9]+)"\)', set_block))
if 'strncmp(key, "alarm"' in set_block:
    fw_keys.add("alarm")

# ---- 3) GET 顶层 / cfg 字段 ----
top = set(re.findall(r'cJSON_Add\w+ToObject\(root, "([a-z_0-9]+)"', fw))
cfg_start = fw.index('cJSON_AddObjectToObject(root, "cfg")')
cfg_block = fw[cfg_start: cfg_start + 6000]
in_cfg = set(re.findall(r'cJSON_Add\w+ToObject\(c, "([a-z_0-9]+)"', cfg_block))

ids = set(re.findall(r'id="([a-z_0-9]+)"', html))

print(f"网页键 {len(kv)} | 固件 SET 键 {len(fw_keys)} | GET 顶层 {len(top)} | GET cfg {len(in_cfg)}\n")
rows, bad = [], []
for k in sorted(kv):
    el = kv[k]
    el_ok = (el is None) or (el in ids)
    fw_ok = (k in fw_keys) or (k == "alarm")
    if k in ("wifi_pass", "portal_pass"):
        where = "不回传(预期)"
    elif k in in_cfg:
        where = "cfg"
    elif k in top:
        where = "顶层"
    else:
        where = "缺失"
    ok = el_ok and fw_ok and where != "缺失"
    rows.append((k, el or "-", el_ok, fw_ok, where, ok))
    if not ok:
        bad.append((k, el_ok, fw_ok, where))

print(f"{'键':16s}{'控件':16s}{'控件':6s}{'SET':6s}{'GET':12s}")
print("-" * 58)
for k, el, el_ok, fw_ok, where, ok in rows:
    mark = "" if ok else "   ← 问题"
    print(f"{k:16s}{el:16s}{('ok' if el_ok else '缺'):6s}"
          f"{('ok' if fw_ok else '缺'):6s}{where:12s}{mark}")

print("\n结论:", "全部对齐 OK" if not bad else f"有 {len(bad)} 处问题: {bad}")

