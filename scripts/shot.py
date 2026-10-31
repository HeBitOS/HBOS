#!/usr/bin/env python3
"""Boot HBOS headless and grab framebuffer screenshots via QEMU monitor.

usage: shot.py <iso> --at 12,20,28 --out shots/ [--boot gui|shell]
Pure monitor-based: uses `screendump` so no X display is needed.
"""
import argparse, os, subprocess, sys, time, socket

ap = argparse.ArgumentParser()
ap.add_argument("iso")
ap.add_argument("--at", default="15,25,35", help="comma separated seconds")
ap.add_argument("--out", default="shots")
ap.add_argument("--mem", default="1024M")
ap.add_argument("--disk", default=None)
ap.add_argument("--mon", default="/tmp/hbos-mon.sock")
args = ap.parse_args()

os.makedirs(args.out, exist_ok=True)
if os.path.exists(args.mon):
    os.unlink(args.mon)

qemu = ["qemu-system-x86_64", "-m", args.mem, "-smp", "1",
        "-display", "none", "-no-reboot", "-net", "none",
        "-accel", "tcg", "-cpu", "max",
        "-monitor", f"unix:{args.mon},server,nowait",
        "-serial", "file:/tmp/hbos-serial.log",
        "-cdrom", args.iso, "-boot", "d"]
if args.disk:
    qemu += ["-device", "ahci,id=ahci",
             "-drive", f"id=disk0,file={args.disk},format=raw,if=none",
             "-device", "ide-hd,drive=disk0,bus=ahci.0"]

print("[shot] " + " ".join(qemu), file=sys.stderr)
p = subprocess.Popen(qemu, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def mon(cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(50):
        try:
            s.connect(args.mon); break
        except OSError:
            time.sleep(0.1)
    else:
        raise SystemExit("monitor connect failed")
    time.sleep(0.2)
    try:
        s.recv(65536)
    except Exception:
        pass
    s.sendall((cmd + "\n").encode())
    time.sleep(0.4)
    try:
        s.recv(65536)
    except Exception:
        pass
    s.close()

t0 = time.time()
for sec in [float(x) for x in args.at.split(",")]:
    wait = sec - (time.time() - t0)
    if wait > 0:
        time.sleep(wait)
    fn = os.path.join(args.out, f"f{int(sec):03d}.ppm")
    if os.path.exists(fn):
        os.unlink(fn)
    mon(f"screendump {fn}")
    print(f"[shot] {sec}s -> {fn}", file=sys.stderr)

mon("quit")
time.sleep(0.5)
p.terminate()
try:
    p.wait(timeout=5)
except Exception:
    p.kill()
print("[shot] done", file=sys.stderr)
