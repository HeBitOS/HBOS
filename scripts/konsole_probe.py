#!/usr/bin/env python3
"""Konsole PTY probe: boot one HBOS VM, screenshot, and poke keys at Konsole.

Must be run under `flock /tmp/hbos-vm.lock` (this script does not take the lock).

Produces under <out>/:
    serial.log              raw guest serial
    shots/NNN_<tag>.png     timed screenshots
    summary.txt             grepped evidence (EXEC-FD, bash errors, konsole warnings)

Exit codes:
    0  Konsole prompt seen on the pty (or keys proved input works)
    1  no decisive evidence either way
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST_T0 = time.time()


def log(msg):
    print(f"[probe +{time.time() - HOST_T0:7.2f}s] {msg}", flush=True)


class Monitor:
    """QEMU HMP over a unix socket."""

    def __init__(self, path, wait=180.0):
        deadline = time.time() + wait
        self.sock = None
        while time.time() < deadline:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                time.sleep(0.3)
                s.setblocking(False)
                try:
                    s.recv(65536)
                except BlockingIOError:
                    pass
                self.sock = s
                log(f"monitor connected: {path}")
                return
            except OSError:
                time.sleep(1.0)
        raise RuntimeError(f"monitor socket never appeared: {path}")

    def cmd(self, text, settle=0.6):
        self.sock.sendall(text.encode() + b"\n")
        time.sleep(settle)
        out = b""
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                out += chunk
        except (BlockingIOError, OSError):
            pass
        return out.decode(errors="replace")

    def screenshot(self, ppm_path, png_path):
        self.cmd(f"screendump {ppm_path}", settle=1.2)
        if os.path.exists(ppm_path):
            try:
                from PIL import Image
                Image.open(ppm_path).save(png_path)
                log(f"shot -> {png_path}")
                return True
            except Exception as exc:                      # noqa: BLE001
                log(f"ppm->png failed: {exc}")
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default="build/hbos-bios.iso")
    ap.add_argument("--img", default="build/hbos-kde-chromium.img")
    ap.add_argument("--out", default="build/tmp/konsole_probe")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--shots", default="120,240,360,470,560,650,740")
    ap.add_argument("--keys-at", type=int, default=700)
    ap.add_argument("--keys", default="r,o,o,t,ret")
    args = ap.parse_args()

    iso = os.path.join(ROOT, args.iso)
    img = os.path.join(ROOT, args.img)
    out = os.path.join(ROOT, args.out)
    shots_dir = os.path.join(out, "shots")
    os.makedirs(shots_dir, exist_ok=True)

    mon_dir = os.path.join(ROOT, "build", "tmp", "monitor")
    os.makedirs(mon_dir, exist_ok=True)
    mon_sock = os.path.join(mon_dir, "mon.sock")
    serial = os.path.join(out, "serial.log")
    for stale in (mon_sock, serial):
        if os.path.exists(stale):
            os.unlink(stale)

    cmd = [
        "qemu-system-x86_64", "-m", "6144M", "-smp", "1", "-enable-kvm",
        "-cpu", "host", "-display", "none", "-no-reboot",
        "-device", "ich9-ahci,id=ahci",
        "-drive", f"id=disk0,file={img},format=raw,if=none",
        "-device", "ide-hd,drive=disk0,bus=ahci.0",
        "-netdev", "user,id=hbosnet",
        "-device", "virtio-net-pci,netdev=hbosnet,disable-modern=on",
        "-cdrom", iso, "-boot", "d",
        "-monitor", f"unix:{mon_sock},server,nowait",
        "-serial", f"file:{serial}",
    ]
    log("starting QEMU")
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)
    try:
        mon = Monitor(mon_sock)
        shot_times = sorted({int(x) for x in args.shots.split(",") if x.strip()})
        keys_sent = False
        deadline = HOST_T0 + args.timeout
        pending = list(shot_times)
        while time.time() < deadline and proc.poll() is None:
            elapsed = time.time() - HOST_T0
            while pending and elapsed >= pending[0]:
                t = pending.pop(0)
                mon.screenshot(os.path.join(out, f"screen_{t}.ppm"),
                               os.path.join(shots_dir, f"{t:03d}_t{t}.png"))
            if not keys_sent and elapsed >= args.keys_at:
                keys_sent = True
                log(f"sending keys: {args.keys}")
                for k in args.keys.split(","):
                    mon.cmd(f"sendkey {k.strip()}", settle=0.4)
                time.sleep(3)
                mon.screenshot(os.path.join(out, "screen_keys.ppm"),
                               os.path.join(shots_dir, "keys_after.png"))
            time.sleep(2)
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=20)
        except Exception:                                  # noqa: BLE001
            proc.kill()
        log("QEMU stopped")

    # ---- evidence summary ----
    text = ""
    if os.path.exists(serial):
        with open(serial, "r", errors="replace") as fh:
            text = fh.read()

    pats = [
        (r"\[EXEC-FD\].*", "EXEC-FD"),
        (r"\[EXEC\] pid=.*path=/usr/bin/bash", "EXEC bash"),
        (r"\[EXEC-ARGV\] \[/usr/bin/bash\].*", "EXEC-ARGV bash"),
        (r"bash: cannot set terminal process group.*", "bash pgrp fail"),
        (r"bash: no job control.*", "bash no job control"),
        (r"\[FDERR\] nr=72 fd=0.*", "FDERR fd0"),
        (r"org\.kde\.konsole: Failed to get foreground.*", "konsole pgrp warn"),
        (r"root@hbos[^\n]*", "PROMPT root@hbos"),
        (r"\[EXIT\] pid=(\d+) code=(\d+).*", "EXIT"),
        (r"\[EXEC\] pid=(\d+) t=(\d+) path=/usr/bin/konsole", "EXEC konsole"),
    ]
    lines = []
    for pat, tag in pats:
        found = re.findall(pat, text)
        lines.append(f"### {tag}: {len(found)} hit(s)")
        for item in found[:25]:
            lines.append(f"    {item if isinstance(item, str) else item}")
    summary = "\n".join(lines)
    with open(os.path.join(out, "summary.txt"), "w") as fh:
        fh.write(summary + "\n")
    print(summary)

    prompt = len(re.findall(r"root@hbos", text))
    bash_exec = len(re.findall(r"\[EXEC\].*path=/usr/bin/bash", text))
    fderr0 = len(re.findall(r"\[FDERR\] nr=72 fd=0", text))
    log(f"prompt={prompt} bash_exec={bash_exec} fderr_fd0={fderr0}")
    return 0 if prompt else 1


if __name__ == "__main__":
    sys.exit(main())
