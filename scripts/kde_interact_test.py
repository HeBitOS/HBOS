#!/usr/bin/env python3
"""启动 VM → 选 KDE → 等 Konsole 出现 → 在窗口里敲 `echo hi` → 截图。

用于验证「键鼠可交互」：如果终端里出现回显，说明 wl_keyboard → KWin →
Konsole → pty → bash 整条链是通的。

注意：HBOS 的 QEMU monitor(HMP) 只服务一个客户端，所以这里只开一个
ctl socket 并全程复用；监控面板要单独一个（脚本用两个 -monitor）。
"""
import argparse, os, socket, subprocess, time

ap = argparse.ArgumentParser()
ap.add_argument("--smp", type=int, default=1)
ap.add_argument("--out", default="build/tmp/interact")
ap.add_argument("--wait-konsole", type=int, default=330)
ap.add_argument("--cmd", default="echo hi")
a = ap.parse_args()

OUT = os.path.abspath(a.out); os.makedirs(OUT, exist_ok=True)
mon, ctl, ser = f"{OUT}/mon.sock", f"{OUT}/ctl.sock", f"{OUT}/serial.log"
for p in (mon, ctl, ser):
    if os.path.exists(p): os.unlink(p)

qemu = ["qemu-system-x86_64", "-m", "6144M", "-smp", str(a.smp), "-enable-kvm",
        "-cpu", "host", "-display", "none", "-vga", "std", "-no-reboot",
        "-device", "ich9-ahci,id=ahci",
        "-drive", "id=disk0,file=build/hbos-kde-chromium.img,format=raw,if=none",
        "-device", "ide-hd,drive=disk0,bus=ahci.0",
        "-netdev", "user,id=n1", "-device", "e1000,netdev=n1",
        "-cdrom", "build/hbos-bios.iso", "-boot", "d",
        "-monitor", f"unix:{mon},server,nowait",
        "-monitor", f"unix:{ctl},server,nowait",
        "-serial", f"file:{ser}"]
subprocess.Popen(qemu, stdout=subprocess.DEVNULL, stderr=open(f"{OUT}/err.txt", "w"))
time.sleep(4)

s = None
for _ in range(300):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(20)
        s.connect(ctl); time.sleep(0.2)
        try: s.recv(65536)
        except Exception: pass
        break
    except OSError:
        s = None; time.sleep(0.2)
if s is None:
    print("no ctl monitor", flush=True); raise SystemExit(2)


def cmd(c, w=0.3):
    s.sendall((c + "\n").encode()); time.sleep(w)
    try: s.recv(65536)
    except Exception: pass


def shot(name):
    p = f"{OUT}/{name}.ppm"; cmd(f"screendump {p}", 1.3)
    prev = -1
    for _ in range(250):
        try: cur = os.path.getsize(p)
        except OSError: cur = -1
        if cur > 1000 and cur == prev: break
        prev = cur; time.sleep(0.05)
    from PIL import Image
    Image.open(p).save(f"{OUT}/{name}.png")
    print(f"shot {OUT}/{name}.png", flush=True)


# 1) 开机菜单选 KDE
time.sleep(20); cmd("sendkey k", 0.6)
print("sent K (KDE)", flush=True)

# 2) 等画面真的画出来（非黑）再敲键。
#    不能用 serial 里是否出现 "usr/bin/konsole" 判断：脚本内容本身就会
#    把这段字符串早早打进 serial，会误判（上一版就是因此在全黑画面上
#    敲键，结果什么都看不到）。
def nonblack_pct(path):
    try:
        from PIL import Image
        g = Image.open(path).convert("L"); px = g.load(); w, h = g.size
        nb = sum(1 for y in range(0, h, 12) for x in range(0, w, 12) if px[x, y] > 16)
        return nb * 100 // (len(range(0, h, 12)) * len(range(0, w, 12)))
    except Exception:
        return 0

t0 = time.time()
ready = False
while time.time() - t0 < a.wait_konsole:
    p = f"{OUT}/probe.ppm"
    cmd(f"screendump {p}", 1.0)
    prev = -1
    for _ in range(120):
        try: cur = os.path.getsize(p)
        except OSError: cur = -1
        if cur > 1000 and cur == prev: break
        prev = cur; time.sleep(0.05)
    from PIL import Image
    try:
        Image.open(p).save(f"{OUT}/probe.png")
    except Exception:
        time.sleep(3); continue
    pct = nonblack_pct(f"{OUT}/probe.png")
    if pct >= 5:
        ready = True
        print(f"screen ready after {int(time.time()-t0)}s (nonblack={pct}%)", flush=True)
        break
    time.sleep(5)
if not ready:
    print("screen never became non-black", flush=True)
# 3) 先把指针移进 Konsole 窗口并点击一次：KWin 需要一个焦点窗口，
#    否则键盘事件没有投递目标（实测：键到了 KWin 却不进终端）。
for _ in range(30):
    cmd("mouse_move 30 15", 0.05)
time.sleep(0.5)
cmd("mouse_button 1", 0.15)
cmd("mouse_button 0", 0.3)
time.sleep(0.5)
shot("before_keys")

# 3) 敲命令：逐字符 sendkey
KEYMAP = {" ": "spc"}
for c in "abcdefghijklmnopqrstuvwxyz0123456789-": KEYMAP[c] = c
for ch in a.cmd:
    k = KEYMAP.get(ch)
    if k: cmd(f"sendkey {k}", 0.12)
cmd("sendkey ret", 0.8)
time.sleep(6)
shot("after_keys")
print("DONE", flush=True)
