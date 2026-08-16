#!/usr/bin/env python3
"""HBOS LiteJS 的 Bilibili GUI 验收。

宿主获取一份真实 Bilibili 视频页（或使用 HBOS_BILIBILI_FIXTURE），通过 QEMU
NAT 的 10.0.2.2 提供给客机。客机 GUI 浏览器必须完成：HTTP 加载、87KB
__INITIAL_STATE__ 执行、Bilibili 语义 DOM 增强、封面抓取和最终重绘。

该测试不声称 H.264/AAC 已播放；它验证的是当前阶段的视频详情页能力。
"""
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

ROOT = Path("/media/data/hbosv2")
ISO = ROOT / "build/hbos-bios.iso"
SOCK = ROOT / "build/qemu-bilibili-monitor.sock"
PPM = ROOT / "build/gui-bilibili.ppm"
PNG = ROOT / "build/gui-bilibili.png"
URL = os.environ.get("HBOS_BILIBILI_URL", "https://www.bilibili.com/video/BV1GJ411x7h7")


def wait_text(text, needle, timeout):
    deadline = time.time() + timeout
    while needle not in text[0] and time.time() < deadline:
        time.sleep(0.2)
    return needle in text[0]


def screendump():
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.connect(str(SOCK)); time.sleep(0.3); c.recv(4096)
    c.sendall(f"screendump {PPM}\n".encode()); time.sleep(1.5); c.recv(4096)
    c.sendall(b"quit\n"); c.close()


def analyze(path):
    data = Path(path).read_bytes()
    e = 0
    for _ in range(3): e = data.index(b"\n", e) + 1
    h = data[:e].decode().split(); w, hh = int(h[1]), int(h[2]); pix = data[e:]
    # 左上浏览器内容区；真实页面应有白底、封面产生的彩色像素和深色文本。
    white = dark = color = total = 0
    for y in range(175, min(hh, 490), 2):
        for x in range(130, min(w, 930), 2):
            o = (y * w + x) * 3
            if o + 2 >= len(pix): continue
            r, g, b = pix[o:o+3]; total += 1
            if r > 240 and g > 240 and b > 240: white += 1
            if r < 100 and g < 100 and b < 100: dark += 1
            if max(r,g,b) - min(r,g,b) > 35 and max(r,g,b) > 100: color += 1
    fw, fd, fc = white/max(total,1), dark/max(total,1), color/max(total,1)
    print(f"[gui-bilibili] {w}x{hh} white={fw:.3f} dark={fd:.3f} color={fc:.3f}")
    return fw > .20 and fd > .002 and fc > .01


def main():
    if not ISO.exists():
        print("run: make bios-iso HBOS_COMPAT_SMOKE=1", file=sys.stderr); return 2
    for p in (SOCK, PPM, PNG):
        try: p.unlink()
        except FileNotFoundError: pass

    with tempfile.TemporaryDirectory(prefix="hbos-bili-") as td:
        fixture = os.environ.get("HBOS_BILIBILI_FIXTURE")
        dst = Path(td) / "bili.html"
        if fixture:
            dst.write_bytes(Path(fixture).read_bytes())
        else:
            req = urllib.request.Request(URL, headers={
                "User-Agent":"Mozilla/5.0 (X11; Linux x86_64) Chrome/151 Safari/537.36",
                "Accept-Encoding":"identity"})
            with urllib.request.urlopen(req, timeout=40) as r: dst.write_bytes(r.read())
        html = dst.read_text(errors="replace")
        if "__INITIAL_STATE__" not in html or "videoData" not in html:
            print("[gui-bilibili] fixture has no Bilibili video state"); return 1

        server = subprocess.Popen([sys.executable, "-m", "http.server", "18766",
            "--bind", "0.0.0.0", "--directory", td], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        qemu = subprocess.Popen(["qemu-system-x86_64", "-m", "512M", "-cdrom", str(ISO),
            "-boot", "d", "-serial", "stdio", "-vga", "std",
            "-monitor", f"unix:{SOCK},server,nowait", "-no-reboot"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1)
        text=[""]; lines=[]
        def read_serial():
            for line in qemu.stdout: lines.append(line); text[0] += line
        threading.Thread(target=read_serial, daemon=True).start()
        try:
            if not wait_text(text, "[KERN] Shell ready", 45):
                print("[gui-bilibili] shell timeout"); return 1
            qemu.stdin.write("T\n"); qemu.stdin.flush(); time.sleep(2)
            qemu.stdin.write("gui http://10.0.2.2:18766/bili.html\n"); qemu.stdin.flush()
            # 页面 + 大 initial-state + QuickJS + 封面图片；留足后台任务时间。
            time.sleep(22)
            screendump(); time.sleep(1)
        finally:
            qemu.terminate(); server.terminate()
            try: qemu.wait(timeout=5)
            except subprocess.TimeoutExpired: qemu.kill()
            try: server.wait(timeout=3)
            except subprocess.TimeoutExpired: server.kill()

    if not PPM.exists(): print("[gui-bilibili] no screenshot"); return 1
    try:
        from PIL import Image
        Image.open(PPM).save(PNG)
    except Exception: pass
    ok = analyze(PPM)
    print("[gui-bilibili] " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__": sys.exit(main())
