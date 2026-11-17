#!/usr/bin/env python3
"""启动 -smp N 的 VM，选 KDE，并按时采样截图。

关键点（踩过的坑）：
  * **开两个 monitor socket**：面板持一个、脚本持一个。
    QEMU 的 HMP 只服务一个客户端，共用一个会让 screendump 全部失败。
  * 输出目录与脚本目录分开，避免清理输出时把脚本本身删掉。
"""
import argparse, os, socket, subprocess, time

ap = argparse.ArgumentParser()
ap.add_argument("--smp", type=int, default=2)
ap.add_argument("--mem", default="6144M")
ap.add_argument("--out", default="build/tmp/smp")
ap.add_argument("--marks", default="60,120,180,300,420,600")
ap.add_argument("--iso", default="build/hbos-bios.iso")
ap.add_argument("--img", default="build/hbos-kde-chromium.img")
a = ap.parse_args()

OUT = os.path.abspath(a.out)
os.makedirs(OUT, exist_ok=True)
mon, ctl, ser = f"{OUT}/mon.sock", f"{OUT}/ctl.sock", f"{OUT}/serial.log"
for p in (mon, ctl, ser):
    if os.path.exists(p):
        os.unlink(p)

qemu = ["qemu-system-x86_64", "-m", a.mem, "-smp", str(a.smp),
        "-enable-kvm", "-cpu", "host", "-display", "none", "-vga", "std", "-no-reboot",
        "-device", "ich9-ahci,id=ahci",
        "-drive", f"id=disk0,file={a.img},format=raw,if=none",
        "-device", "ide-hd,drive=disk0,bus=ahci.0",
        "-netdev", "user,id=n1", "-device", "e1000,netdev=n1",
        "-cdrom", a.iso, "-boot", "d",
        "-monitor", f"unix:{mon},server,nowait",
        "-monitor", f"unix:{ctl},server,nowait",
        "-serial", f"file:{ser}"]
subprocess.Popen(qemu, stdout=subprocess.DEVNULL, stderr=open(f"{OUT}/err.txt", "w"))
time.sleep(4)


def conn(path):
    for _ in range(300):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(15)
            s.connect(path)
            time.sleep(0.2)
            try:
                s.recv(65536)
            except Exception:
                pass
            return s
        except OSError:
            time.sleep(0.2)
    return None


s = conn(ctl)
if s is None:
    print("no ctl monitor", flush=True)
    raise SystemExit(2)


def cmd(c, w=0.3):
    s.sendall((c + "\n").encode())
    time.sleep(w)
    try:
        s.recv(65536)
    except Exception:
        pass


def shot(name):
    p = f"{OUT}/{name}.ppm"
    cmd(f"screendump {p}", 1.3)
    prev = -1
    for _ in range(250):
        try:
            cur = os.path.getsize(p)
        except OSError:
            cur = -1
        if cur > 1000 and cur == prev:
            break
        prev = cur
        time.sleep(0.05)
    try:
        from PIL import Image
        im = Image.open(p)
        im.save(f"{OUT}/{name}.png")
        g = im.convert("L"); px = g.load(); w, h = g.size
        pan = sum(1 for y in range(h - 45, h, 3) for x in range(0, w, 10) if px[x, y] > 16)
        pt = len(range(h - 45, h, 3)) * len(range(0, w, 10))
        nb = sum(1 for y in range(0, h, 10) for x in range(0, w, 10) if px[x, y] > 16)
        nt = len(range(0, h, 10)) * len(range(0, w, 10))
        print(f"{name} panel={pan*100//max(pt,1)}% screen={nb*100//max(nt,1)}%", flush=True)
    except Exception as e:
        print("shotfail", name, e, flush=True)


time.sleep(20)
shot("menu")
cmd("sendkey k", 0.6)
print("sent K", flush=True)
t0 = time.time()
for mark in [int(x) for x in a.marks.split(",")]:
    while time.time() - t0 < mark:
        time.sleep(1)
    shot(f"s{mark:03d}")
print("DONE", flush=True)
