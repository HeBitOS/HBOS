#!/usr/bin/env python3
"""向正在运行的 HBOS VM 注入按键，并对比前后画面，验证「键鼠可交互」。

用法:
    python3 scripts/kde_input_test.py --mon build/tmp/accept/mon.sock \
        --out build/tmp/accept --keys "h,i,ret"

判定：按键前后像素差 > 0 即认为有响应；同时打印差异区域 bbox 与像素数。
注意：早期版本用「整屏百分比」当阈值，16x16 的变化约 0.01% 会被四舍五入成 0.0，
      造成「明明响应了却报 0 响应」的假阴性。这里改用绝对像素数。
"""
import argparse
import os
import socket
import time

ap = argparse.ArgumentParser()
ap.add_argument("--mon", default="build/tmp/accept/mon.sock")
ap.add_argument("--out", default="build/tmp/accept")
ap.add_argument("--keys", default="h,i,ret")
args = ap.parse_args()

def mon_cmd(s, cmd, wait=0.6):
    s.sendall((cmd + "\n").encode())
    time.sleep(wait)
    try:
        s.recv(65536)
    except Exception:
        pass

def shot(s, name):
    path = os.path.join(args.out, name + ".ppm")
    mon_cmd(s, f"screendump {path}", 0.8)
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

before = shot(s, "keys_before")
for k in args.keys.split(","):
    k = k.strip()
    if k:
        mon_cmd(s, f"sendkey {k}", 0.25)
time.sleep(1.0)
after = shot(s, "keys_after")

from PIL import Image, ImageChops
a = Image.open(before).convert("RGB")
b = Image.open(after).convert("RGB")
diff = ImageChops.difference(a, b)
bbox = diff.getbbox()
px = diff.load()
w, h = diff.size
changed = 0
for y in range(h):
    for x in range(0, w, 1):
        r, g, bl = px[x, y]
        if r or g or bl:
            changed += 1

print(f"before: {before}")
print(f"after : {after}")
print(f"差异 bbox      = {bbox}")
print(f"变化像素数     = {changed} / {w*h}")
print(f"响应           = {'是' if changed > 0 else '否'}")
s.close()
raise SystemExit(0 if changed > 0 else 1)
