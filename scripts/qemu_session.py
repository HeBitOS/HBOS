#!/usr/bin/env python3
"""Drive HBOS in QEMU (serial) through a scripted shell session.

usage: qemu_session.py <iso> [--disk img] [--mem 2048M] [--accel auto|kvm|tcg]
                       [--timeout N] [--log out.txt] -- cmd1 cmd2 ...

Each command is sent once the shell prompt ("# ") is seen; all serial output
is echoed to stdout (and to --log).  Exit status 0 if the session completed.

Acceleration: KVM when /dev/kvm exists (default), otherwise TCG so the same
script works in containers without nested virtualisation.
"""
import argparse
import os
import subprocess
import sys
import time

ap = argparse.ArgumentParser()
ap.add_argument("iso")
ap.add_argument("--disk")
ap.add_argument("--mem", default="1024M")
ap.add_argument("--accel", default="auto", choices=("auto", "kvm", "tcg"))
ap.add_argument("--timeout", type=float, default=30.0, help="per-command seconds")
ap.add_argument("--boot-timeout", type=float, default=90.0)
ap.add_argument("--log", help="also write the serial transcript here")
ap.add_argument("--smp", default="1")
ap.add_argument("--timings", action="store_true",
                help="emit per-command wall time on stderr (opt-in; the "
                     "default output stays byte-identical for existing callers)")
ap.add_argument("cmds", nargs="*")
args = ap.parse_args()

accel = args.accel
if accel == "auto":
    accel = "kvm" if os.path.exists("/dev/kvm") else "tcg"

qemu = ["qemu-system-x86_64", "-m", args.mem, "-smp", args.smp,
        "-display", "none", "-serial", "stdio", "-no-reboot", "-net", "none"]
if accel == "kvm":
    qemu += ["-enable-kvm", "-cpu", "host"]
else:
    qemu += ["-accel", "tcg", "-cpu", "max"]
if args.disk:
    qemu += ["-device", "ahci,id=ahci",
             "-drive", f"id=disk0,file={args.disk},format=raw,if=none",
             "-device", "ide-hd,drive=disk0,bus=ahci.0"]
qemu += ["-cdrom", args.iso, "-boot", "d"]

print(f"[qemu_session] accel={accel} mem={args.mem}", file=sys.stderr)
p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, bufsize=0)
os.set_blocking(p.stdout.fileno(), False)
buf = b""
log = open(args.log, "wb") if args.log else None


def emit(chunk: bytes):
    sys.stdout.buffer.write(chunk)
    sys.stdout.buffer.flush()
    if log:
        log.write(chunk)
        log.flush()


def pump(token, timeout: float) -> bool:
    """Echo output until a token (bytes, or tuple of bytes) is seen."""
    global buf
    tokens = token if isinstance(token, tuple) else (token,)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            chunk = p.stdout.read(4096)
        except BlockingIOError:
            chunk = None
        if chunk:
            emit(chunk)
            buf += chunk
            hit = [t for t in tokens if t in buf]
            if hit:
                pump.hit = hit[0]
                buf = b""
                return True
        else:
            if p.poll() is not None:
                return False
            time.sleep(0.02)
    return False


ok = pump((b"Select boot mode", b"# "), args.boot_timeout)
if ok and pump.hit == b"Select boot mode":
    time.sleep(0.2)
    p.stdin.write(b"t\n")
    ok = pump(b"# ", 30)
if ok:
    time.sleep(1.0)  # let the shell finish its post-prompt init before typing
for cmd in args.cmds:
    if not ok:
        break
    t0 = time.monotonic()
    p.stdin.write(cmd.encode() + b"\n")
    ok = pump(b"# ", args.timeout)
    # 只有显式要 --timings 才打：默认 stderr 保持原样，现有调用方
    # （perf_baseline.py 之外的脚本）不受影响。
    if args.timings:
        print(f"[qemu_session] cmd {time.monotonic() - t0:.3f}s: {cmd}",
              file=sys.stderr)
if not ok:
    print("\n[qemu_session] TIMEOUT or early exit", file=sys.stderr)
p.terminate()
try:
    p.wait(timeout=3)
except subprocess.TimeoutExpired:
    p.kill()
if log:
    log.close()
sys.exit(0 if ok else 1)
