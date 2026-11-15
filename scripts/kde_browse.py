#!/usr/bin/env python3
"""在正在运行的 HBOS VM 里，用 QEMU monitor 的 sendkey 往 Chromium 地址栏输入网址。

因为重打镜像 + 重启一轮要 10 分钟，而地址栏输入只需要几秒。

用法:
    python3 scripts/kde_browse.py --url www.bilibili.com \
        --mon build/tmp/accept/mon.sock --out build/tmp/accept

流程：点击 Chromium 窗口 -> Ctrl+L 聚焦地址栏 -> 逐字符 sendkey -> 回车 -> 等加载 -> 截图。
"""
import argparse
import os
import socket
import time

ap = argparse.ArgumentParser()
ap.add_argument("--url", default="www.bilibili.com")
ap.add_argument("--mon", default="build/tmp/accept/mon.sock")
ap.add_argument("--out", default="build/tmp/accept")
ap.add_argument("--click", default="960,300", help="先点击这个坐标让窗口获得焦点，空串表示不点")
ap.add_argument("--wait", type=float, default=90.0, help="回车后等待加载的秒数")
args = ap.parse_args()

# QEMU sendkey 的键名映射（只覆盖输入网址需要的字符）
KEYMAP = {".": "dot", "/": "slash", "-": "minus", ":": "shift-semicolon", "_": "shift-minus"}
for c in "abcdefghijklmnopqrstuvwxyz":
    KEYMAP[c] = c
for d in "0123456789":
    KEYMAP[d] = d


def cmd(s, c, wait=0.06):
    s.sendall((c + "\n").encode())
    time.sleep(wait)
    try:
        s.recv(65536)
    except Exception:
        pass


def shot(s, name):
    path = os.path.join(args.out, name + ".ppm")
    cmd(s, f"screendump {path}", 0.8)
    prev = -1
    for _ in range(120):
        try:
            cur = os.path.getsize(path)
        except OSError:
            cur = -1
        if cur > 1000 and cur == prev:
            break
        prev = cur
        time.sleep(0.03)
    png = os.path.join(args.out, name + ".png")
    from PIL import Image
    Image.open(path).save(png)
    return png


s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(8)
s.connect(args.mon)
time.sleep(0.2)
try:
    s.recv(65536)
except Exception:
    pass

if args.click:
    x, y = args.click.split(",")
    # HMP mouse_move 是相对位移；先移到屏幕中央再点击比较麻烦，
    # 这里直接发一个左键点击（很多情况下 Chromium 已经在焦点上）。
    cmd(s, "mouse_button 1", 0.1)
    cmd(s, "mouse_button 0", 0.2)
    print(f"[browse] clicked at (existing pointer)")

cmd(s, "sendkey ctrl-l", 0.5)
print("[browse] ctrl+l (focus address bar)")
time.sleep(0.4)
for ch in args.url:
    key = KEYMAP.get(ch)
    if not key:
        print(f"[browse] 跳过未映射字符: {ch!r}")
        continue
    cmd(s, key, 0.09)
print(f"[browse] typed: {args.url}")
time.sleep(0.5)
cmd(s, "sendkey ret", 0.5)
print(f"[browse] enter, waiting {args.wait}s for load…")
time.sleep(args.wait)
png = shot(s, "bilibili")
print(f"[browse] screenshot: {png}")

from PIL import Image
im = Image.open(png).convert("L")
px = im.load(); w, h = im.size
nb = sum(1 for y in range(0, h, 8) for x in range(0, w, 8) if px[x, y] > 16)
tot = len(range(0, h, 8)) * len(range(0, w, 8))
print(f"[browse] 非黑像素 = {nb*100//max(tot,1)}%")
s.close()
