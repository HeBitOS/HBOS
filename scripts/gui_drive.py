#!/usr/bin/env python3
"""Drive the HIVE GUI in QEMU and capture screenshots.

Input goes through the human monitor's `mouse_move` (relative deltas to the
guest PS/2 mouse).  HBOS's USB HID path only understands RELATIVE X/Y too, so
`usb-tablet` absolute coordinates are silently ignored — scripted absolute
positioning therefore has to be built from measured deltas.

The first thing the script does is a calibration run: move a known delta and
see how far the pointer actually travelled (diffing the two screenshots).  The
resulting gain is applied to every later `move`/`click`.

usage: gui_drive.py <iso> --script plan.json --out build/gdrive [--calib]

plan.json is a list of [action, ...]:
  ["boot"]                         wait for the GUI prompt, then press 'g'
  ["wait", secs]
  ["shot", name]                   screendump -> <out>/<name>.png
  ["move", x, y]
  ["click", x, y]
  ["dblclick", x, y]
  ["down", x, y] / ["up"]
  ["drag", x1, y1, x2, y2, steps]
  ["key", qcode]
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import time
import zlib


def read_ppm(path):
    d = open(path, "rb").read()
    i, toks = 0, []
    while len(toks) < 4:
        while d[i:i + 1].isspace():
            i += 1
        if d[i:i + 1] == b"#":
            while d[i:i + 1] != b"\n":
                i += 1
            continue
        s = i
        while not d[i:i + 1].isspace():
            i += 1
        toks.append(d[s:i])
    i += 1
    w, h = int(toks[1]), int(toks[2])
    return w, h, d[i:i + w * h * 3]


def write_png(path, w, h, px):
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, data):
        c = t + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def find_cursor(w, h, px, bg, near=None, radius=160):
    """Locate the cursor sprite by diffing against a cursor-free baseline.

    The cursor is a small light-blue arrow, so a diff against `bg` finds it even
    when it overlaps the wallpaper.  `near` restricts the search to a window
    around a predicted position (the pointer only ever moves a little between
    calibration steps).
    """
    x0, x1, y0, y1 = 0, w, 0, h
    if near:
        x0 = max(0, near[0] - radius)
        x1 = min(w, near[0] + radius)
        y0 = max(0, near[1] - radius)
        y1 = min(h, near[1] + radius)
    best, bx, by = 0, -1, -1
    for y in range(y0, y1, 2):
        row = y * w * 3
        for x in range(x0, x1, 2):
            o = row + x * 3
            d = abs(px[o] - bg[o]) + abs(px[o + 1] - bg[o + 1]) + abs(px[o + 2] - bg[o + 2])
            if d > best:
                best, bx, by = d, x, y
    return bx, by, best


class Mon:
    def __init__(self, path):
        for _ in range(120):
            try:
                self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.s.connect(path)
                time.sleep(0.3)
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

    def cmd(self, c, settle=0.1):
        self.s.sendall((c + "\n").encode())
        time.sleep(settle)
        try:
            self.s.setblocking(False)
            self.s.recv(65536)
            self.s.setblocking(True)
        except Exception:
            self.s.setblocking(True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso")
    ap.add_argument("--script", required=True)
    ap.add_argument("--out", default="build/gdrive")
    ap.add_argument("--mem", default="1024M")
    ap.add_argument("--calib", action="store_true", help="run pointer gain calibration")
    ap.add_argument("--gain", type=float, default=1.0)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    sock = "/tmp/hbos-drive.sock"
    if os.path.exists(sock):
        os.unlink(sock)
    serial = "/tmp/hbos-drive-serial.log"
    qemu = ["qemu-system-x86_64", "-m", args.mem, "-smp", "2",
            "-display", "none", "-no-reboot", "-net", "none",
            "-accel", "tcg", "-cpu", "max",
            "-monitor", f"unix:{sock},server,nowait",
            "-serial", f"file:{serial}",
            "-cdrom", args.iso, "-boot", "d"]
    p = subprocess.Popen(qemu, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    m = Mon(sock)

    scratch = os.path.join(args.out, "_scratch.ppm")

    def grab():
        if os.path.exists(scratch):
            os.unlink(scratch)
        m.cmd(f"screendump {scratch}", settle=0.35)
        for _ in range(40):
            if os.path.exists(scratch) and os.path.getsize(scratch) > 5000:
                break
            time.sleep(0.05)
        return read_ppm(scratch)

    def shot(name):
        w, h, px = grab()
        write_png(os.path.join(args.out, name + ".png"), w, h, px)
        print(f"[drive] {name} ({w}x{h})", file=sys.stderr)
        return w, h, px

    cur = [960, 540]
    gain = [args.gain, args.gain]

    def nudge(dx, dy):
        """Emit relative deltas, chunked so a big jump doesn't get dropped."""
        while dx or dy:
            sx = max(-24, min(24, int(round(dx))))
            sy = max(-24, min(24, int(round(dy))))
            if sx == 0 and dx:
                sx = 1 if dx > 0 else -1
            if sy == 0 and dy:
                sy = 1 if dy > 0 else -1
            m.cmd(f"mouse_move {sx} {sy}", settle=0.012)
            dx -= sx
            dy -= sy

    def move_to(x, y, settle=0.06):
        dx = (x - cur[0]) * gain[0]
        dy = (y - cur[1]) * gain[1]
        nudge(dx, dy)
        cur[0], cur[1] = x, y
        time.sleep(settle)

    booted = [False]

    def boot():
        if booted[0]:
            return
        time.sleep(12)
        m.cmd("sendkey g")
        booted[0] = True

    if args.calib:
        boot()
        time.sleep(9)
        w, h, bg = grab()                       # 指针在系统初始位置
        start = find_cursor(w, h, bg, bg, radius=0)
        print(f"[calib] 初始光标 ~ ({start[0]},{start[1]}) score={start[2]}", file=sys.stderr)
        nudge(240, 0)
        time.sleep(0.5)
        w, h, px = grab()
        end = find_cursor(w, h, px, bg, near=(start[0] + 240, start[1]))
        print(f"[calib] 送 240 px 后光标 ~ ({end[0]},{end[1]}) score={end[2]}", file=sys.stderr)
        if end[0] > 0 and start[0] >= 0:
            trav = end[0] - start[0]
            if trav > 10:
                g = 240.0 / trav
                print(f"[calib] 移动 {trav}px → gain={g:.3f}", file=sys.stderr)
            else:
                print(f"[calib] 位移过小 ({trav}px)，无法定标", file=sys.stderr)
        cur[0], cur[1] = end[0], end[1]
        shot("calib_after")

    for st in json.load(open(args.script)):
        op = st[0]
        if op == "boot":
            boot()
        elif op == "wait":
            time.sleep(st[1])
        elif op == "shot":
            shot(st[1])
        elif op == "move":
            move_to(st[1], st[2])
        elif op == "click":
            move_to(st[1], st[2])
            m.cmd("mouse_button 1")
            time.sleep(0.05)
            m.cmd("mouse_button 0", settle=0.35)
        elif op == "dblclick":
            move_to(st[1], st[2])
            for _ in range(2):
                m.cmd("mouse_button 1")
                time.sleep(0.05)
                m.cmd("mouse_button 0", settle=0.06)
            time.sleep(0.45)
        elif op == "down":
            move_to(st[1], st[2])
            m.cmd("mouse_button 1")
        elif op == "up":
            m.cmd("mouse_button 0", settle=0.3)
        elif op == "drag":
            x1, y1, x2, y2, n = st[1], st[2], st[3], st[4], st[5]
            move_to(x1, y1)
            m.cmd("mouse_button 1")
            time.sleep(0.05)
            for k in range(1, n + 1):
                move_to(x1 + (x2 - x1) * k // n, y1 + (y2 - y1) * k // n, settle=0.03)
            m.cmd("mouse_button 0", settle=0.4)
        elif op == "key":
            m.cmd(f"sendkey {st[1]}")
        else:
            raise SystemExit(f"unknown action {op}")

    m.cmd("quit")
    time.sleep(0.4)
    p.terminate()
    try:
        p.wait(timeout=5)
    except Exception:
        p.kill()
    print("[drive] done", file=sys.stderr)


if __name__ == "__main__":
    main()
