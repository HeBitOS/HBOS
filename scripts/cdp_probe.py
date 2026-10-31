#!/usr/bin/env python3
"""在 HBOS 里起 Chromium（GUI 模式 + ozone headless），从宿主机走 CDP 探活。

为什么要这样：Debian 的 chromium 没编 headless shell，`--headless --dump-dom`
只会让浏览器"无窗口→退出"。但 GUI 模式 + `--ozone-platform=headless` 能让浏览器
一直活着，此时用 `--remote-debugging-port` 暴露 CDP，宿主机就能取 DOM/截图。

用法：
  python3 scripts/cdp_probe.py --disk build/c3-chromium.img --wait 75
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

ap = argparse.ArgumentParser()
ap.add_argument("--iso", default="build/hbos-bios.iso")
ap.add_argument("--disk", default="build/c3-chromium.img")
ap.add_argument("--mem", default="4096M")
ap.add_argument("--log", default="build/tmp/cdp-probe.log")
ap.add_argument("--port", type=int, default=19222)
ap.add_argument("--wait", type=int, default=75, help="启动后等待秒数")
ap.add_argument("--url", default="about:blank")
ap.add_argument("--extra", default="")
args = ap.parse_args()

qemu = [
    "qemu-system-x86_64", "-m", args.mem, "-smp", "1",
    "-display", "none", "-serial", "stdio", "-no-reboot",
    "-accel", "tcg", "-cpu", "max",
    "-device", "ahci,id=ahci",
    "-drive", f"id=disk0,file={args.disk},format=raw,if=none",
    "-device", "ide-hd,drive=disk0,bus=ahci.0",
    "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{args.port}-:{args.port}",
    "-device", "virtio-net-pci,netdev=n0",
    "-cdrom", args.iso, "-boot", "d",
]

log = open(args.log, "wb")
p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, bufsize=0)
os.set_blocking(p.stdout.fileno(), False)


def read_until(needle, timeout):
    deadline = time.time() + timeout
    tail = b""
    while time.time() < deadline:
        try:
            chunk = p.stdout.read(65536)
        except Exception:
            chunk = None
        if chunk:
            log.write(chunk)
            log.flush()
            tail = (tail + chunk)[-8192:]
            if needle in tail:
                return True
        time.sleep(0.05)
    return False


chrome = ("run /opt/chromium/usr/lib/chromium/chromium --no-sandbox --no-zygote "
          "--single-process --disable-gpu --disable-crashpad-for-testing "
          f"--ozone-platform=headless --remote-debugging-port={args.port} "
          "--remote-allow-origins=* " + args.extra + " " + args.url)

if not read_until(b"# ", 120):
    print("[probe] boot timeout")
elif b"Select boot mode" in open(args.log, "rb").read()[-200000:]:
    time.sleep(0.3)
    p.stdin.write(b"t\n")
    read_until(b"# ", 30)

time.sleep(1.0)
p.stdin.write(chrome.encode() + b"\n")
print(f"[probe] chrome started, waiting {args.wait}s")
time.sleep(args.wait)

for path in ("/json/version", "/json/list"):
    url = f"http://127.0.0.1:{args.port}{path}"
    try:
        with urllib.request.urlopen(url, timeout=10) as r:
            body = r.read().decode("utf-8", "replace")
        print(f"[probe] {path} -> {r.status}")
        print(body[:600])
    except Exception as exc:  # noqa: BLE001
        print(f"[probe] {path} FAILED: {exc}")

p.terminate()
try:
    p.wait(timeout=5)
except subprocess.TimeoutExpired:
    p.kill()
log.close()
