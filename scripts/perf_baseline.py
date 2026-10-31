#!/usr/bin/env python3
"""性能基线：测到 shell ready 的耗时 + 固定负载的耗时。

为什么要有这个脚本：改代码前后没有同一把尺子，"优化了"就只能是感觉。
这里测两个客观量：

  1. **启动到 shell ready**：从 QEMU 进程启动到串口出现 "[KERN] Shell ready"。
     覆盖内核初始化、VFS 挂载、seed 全部落盘、命令注册。
  2. **固定工作负载**：跑一个固定的用户程序（Wayland surface 测试），
     测它从 run 到进程退出的墙钟时间。覆盖 syscall 分发、内存映射、
     socket 收发、memfd 读像素。

跑三次取中位数 —— TCG 下第一次往往被页缓存/译码预热拖慢，直接取
第一次会得出误导性的结论。

用法：python3 scripts/perf_baseline.py [--runs 3] [--json out.json]
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def measure_boot(iso: Path, log: Path) -> float:
    """返回从启动 QEMU 到 "Shell ready" 出现的秒数。"""
    if log.exists():
        log.unlink()
    cmd = [
        "qemu-system-x86_64",
        "-cdrom", str(iso),
        "-m", "2048M",
        "-boot", "d",
        "-display", "none",
        "-serial", f"file:{log}",
        "-monitor", "none",
        "-no-reboot",
    ]
    start = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        while time.monotonic() - start < 180:
            if log.exists():
                try:
                    text = log.read_text(errors="ignore")
                except OSError:
                    text = ""
                if "Shell ready" in text:
                    return time.monotonic() - start
            time.sleep(0.1)
        return -1.0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


def measure_workload(iso: Path, log: Path, disk: Path | None) -> float:
    """跑 Wayland surface 测试，返回 run 到退出的秒数。"""
    script = ROOT / "scripts/qemu_session.py"
    cmd = [sys.executable, str(script), str(iso),
           "--mem", "2048", "--boot-timeout", "90", "--timeout", "90",
           "--log", str(log)]
    # 测试程序装在磁盘镜像里（/bin/wayland_surface），不挂盘就找不到。
    if disk and disk.exists():
        cmd += ["--disk", str(disk)]
    cmd += ["--", "wayland --probe-input", "run /bin/wayland_surface"]
    start = time.monotonic()
    subprocess.run(cmd, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, check=False)
    elapsed = time.monotonic() - start
    # 测试本身是否通过也要确认，不然测的是"跑了个失败的用例"
    try:
        text = log.read_text(errors="ignore")
    except OSError:
        text = ""
    if "wl-surface: PASS" not in text:
        return -1.0
    return elapsed


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default=str(ROOT / "build/hbos-bios.iso"))
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--disk", default=str(ROOT / "build/c3-chromium.img"))
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    iso = Path(args.iso)
    if not iso.exists():
        print(f"[perf] 找不到 ISO: {iso}")
        return 1

    tmp = ROOT / "build/tmp"
    tmp.mkdir(parents=True, exist_ok=True)

    boots, works = [], []
    for i in range(args.runs):
        b = measure_boot(iso, tmp / "perf-boot.log")
        w = measure_workload(iso, tmp / "perf-work.log", Path(args.disk))
        print(f"[perf] run {i + 1}: boot={b:.1f}s workload={w:.1f}s")
        if b > 0:
            boots.append(b)
        if w > 0:
            works.append(w)

    if not boots or not works:
        print("[perf] 测量失败（超时或测试未通过）")
        return 1

    result = {
        "boot_median_s": round(statistics.median(boots), 2),
        "workload_median_s": round(statistics.median(works), 2),
        "runs": args.runs,
    }
    print(f"[perf] 中位数: boot={result['boot_median_s']}s "
          f"workload={result['workload_median_s']}s")

    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=2))
        print(f"[perf] 已写入 {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
