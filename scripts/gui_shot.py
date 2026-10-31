#!/usr/bin/env python3
"""Boot HBOS to the GUI and screendump via QEMU monitor.

Sends 'g' at the boot prompt to pick the HIVE GUI, then takes screenshots at
the requested times.  Optionally runs a sequence of monitor `sendkey` actions
to open menus / click, so we can verify interactive UI without a display.

usage: gui_shot.py <iso> [--at 30,40] [--out build/shots] [--keys "..." ]
"""
import argparse, os, socket, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("iso")
ap.add_argument("--at", default="30,40,50")
ap.add_argument("--out", default="build/shots")
ap.add_argument("--mem", default="1024M")
ap.add_argument("--disk", default=None)
ap.add_argument("--mon", default="/tmp/hbos-gmon.sock")
ap.add_argument("--serial", default="/tmp/hbos-gui-serial.log")
args = ap.parse_args()

os.makedirs(args.out, exist_ok=True)
if os.path.exists(args.mon):
    os.unlink(args.mon)

qemu = ["qemu-system-x86_64", "-m", args.mem, "-smp", "2",
        "-display", "none", "-no-reboot", "-net", "none",
        "-accel", "tcg", "-cpu", "max",
        "-monitor", f"unix:{args.mon},server,nowait",
        "-serial", f"file:{args.serial}",
        "-cdrom", args.iso, "-boot", "d"]
if args.disk:
    qemu += ["-device", "ahci,id=ahci",
             "-drive", f"id=disk0,file={args.disk},format=raw,if=none",
             "-device", "ide-hd,drive=disk0,bus=ahci.0"]
print("[gui] " + " ".join(qemu), file=sys.stderr)
p = subprocess.Popen(qemu, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class Mon:
    def __init__(self, path):
        for _ in range(80):
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                time.sleep(0.25)
                self.s.setblocking(False)
                try:
                    self.s.recv(65536)
                except Exception:
                    pass
                self.s.setblocking(True)
                return
            except OSError:
                time.sleep(0.1)
        raise SystemExit("monitor connect failed")

    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        time.sleep(0.25)
        try:
            self.s.setblocking(False)
            self.s.recv(65536)
            self.s.setblocking(True)
        except Exception:
            self.s.setblocking(True)


m = Mon(args.mon)


def shot(name):
    fn = os.path.join(args.out, name + ".ppm")
    if os.path.exists(fn):
        os.unlink(fn)
    m.cmd(f"screendump {fn}")
    print(f"[gui] {name}", file=sys.stderr)


t0 = time.time()
pending = sorted(float(x) for x in args.at.split(","))
sent_boot = False
for sec in pending:
    while time.time() - t0 < sec:
        if not sent_boot and time.time() - t0 > 12:
            m.cmd("sendkey g")      # 选 GUI
            sent_boot = True
        time.sleep(0.2)
    if not sent_boot:
        m.cmd("sendkey g")
        sent_boot = True
    shot(f"g{int(sec):03d}")

m.cmd("quit")
time.sleep(0.5)
p.terminate()
try:
    p.wait(timeout=5)
except Exception:
    p.kill()
print("[gui] done", file=sys.stderr)
