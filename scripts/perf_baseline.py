#!/usr/bin/env python3
"""性能基线：测到 shell ready 的耗时 + 固定负载的耗时。

为什么要有这个脚本：改代码前后没有同一把尺子，"优化了"就只能是感觉。
这里测两个客观量：

  1. **启动到 shell ready**：从 QEMU 进程启动到串口出现 "[KERN] Shell ready"。
     覆盖内核初始化、VFS 挂载、seed 全部落盘、命令注册。
  2. **固定工作负载**：跑一个固定的用户程序（Wayland surface 测试），
     测它从 run 到进程退出的墙钟时间。覆盖 syscall 分发、内存映射、
     socket 收发、memfd 读像素。只计 `run ...` 那一条命令，不含启动和
     qemu_session.py 的固定 1s 等待（见 measure_workload 的注释）。

跑三次取中位数 —— TCG 下第一次往往被页缓存/译码预热拖慢，直接取
第一次会得出误导性的结论。失败的样本返回 None 并被排除在统计之外，
结果里同时记录 requested 与 n，丢样本不会被藏起来。

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


def measure_boot(iso: Path, log: Path) -> float | None:
    """返回从启动 QEMU 到 "Shell ready" 出现的秒数；超时返回 None。"""
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
        # 超时不是"0 秒启动"：返回 None，让统计侧排除，而不是塞一个
        # 看起来很快的数值进去。
        return None
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


def measure_workload(iso: Path, log: Path, disk: Path | None) -> float | None:
    """跑 Wayland surface 测试，返回**负载本身**的秒数；失败返回 None。

    计时口径：只算 `run /bin/wayland_surface` 这一条命令的墙钟，不含
    QEMU 启动、引导器、以及 qemu_session.py 在拿到 prompt 之后那个固定的
    1.0s 等待。原来的实现把整个子进程计时，于是"workload=9.97s"里
    其实有约 3s 的启动和 1s 的固定 sleep，既和 boot 指标重复计，
    又让 1s 噪声淹没真实的负载变化。

    失败一律返回 None 而不是 -1.0：-1.0 是个"看起来很快"的数值，
    一旦混进 median 就会把结论拉向"变快了"。过滤失败样本的责任在
    统计侧（见 summarize），这里只负责如实标注"这次没测到"。
    """
    script = ROOT / "scripts/qemu_session.py"
    cmd = [sys.executable, str(script), str(iso),
           "--mem", "2048", "--boot-timeout", "90", "--timeout", "90",
           "--timings", "--log", str(log)]
    # 测试程序装在磁盘镜像里（/bin/wayland_surface），不挂盘就找不到。
    if disk and disk.exists():
        cmd += ["--disk", str(disk)]
    cmd += ["--", "wayland --probe-input", "run /bin/wayland_surface"]
    proc = subprocess.run(cmd, stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE, text=True, check=False)
    # 测试本身是否通过也要确认，不然测的是"跑了个失败的用例"
    try:
        text = log.read_text(errors="ignore")
    except OSError:
        text = ""
    if "wl-surface: PASS" not in text:
        return None
    # 从 --timings 的 stderr 里取 `run ...` 那一条的耗时。
    for line in reversed(proc.stderr.splitlines()):
        marker = "[qemu_session] cmd "
        if marker not in line:
            continue
        rest = line.split(marker, 1)[1]
        seconds, _, command = rest.partition("s: ")
        if not command.startswith("run "):
            continue
        try:
            return float(seconds)
        except ValueError:
            break
    # 走到了 PASS 却没有计时行（老版本 qemu_session.py）——宁可报失败，
    # 也不要拿一个口径不对的整段耗时冒充负载耗时。
    return None


def summarize(samples: list[float], requested: int, label: str) -> dict:
    """把样本汇总成 {median, n, spread}，并把"丢了几次"显式写进结果。

    为什么要 n：原来只写 `runs: 3`，但失败样本被静默丢弃，实际可能只有
    1 个样本参与中位数——读者会以为那是 3 次的中位数。这里把 requested
    和 n 分开报，丢样本这件事就藏不住了。

    为什么要 spread：TCG 下同一次改动的抖动可达 10%，不给 max-min
    就没法判断"两个数字的差"是不是噪声。
    """
    if not samples:
        return {"median_s": None, "n": 0, "requested": requested,
                "min_s": None, "max_s": None, "spread_s": None}
    return {
        "median_s": round(statistics.median(samples), 2),
        "n": len(samples),
        "requested": requested,
        "min_s": round(min(samples), 2),
        "max_s": round(max(samples), 2),
        "spread_s": round(max(samples) - min(samples), 2),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default=str(ROOT / "build/hbos-bios.iso"))
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--disk", default=str(ROOT / "build/c3-chromium.img"))
    ap.add_argument("--json", default="")
    ap.add_argument("--min-samples", type=int, default=2,
                    help="至少要有几次成功样本才出结论（默认 2："
                         "1 个样本没有中位数可言）")
    args = ap.parse_args()

    iso = Path(args.iso)
    if not iso.exists():
        print(f"[perf] 找不到 ISO: {iso}")
        return 1

    tmp = ROOT / "build/tmp"
    tmp.mkdir(parents=True, exist_ok=True)

    boots: list[float] = []
    works: list[float] = []
    for i in range(args.runs):
        b = measure_boot(iso, tmp / "perf-boot.log")
        w = measure_workload(iso, tmp / "perf-work.log", Path(args.disk))
        fmt = lambda v: "FAIL" if v is None else f"{v:.2f}s"
        print(f"[perf] run {i + 1}/{args.runs}: boot={fmt(b)} workload={fmt(w)}")
        # 失败样本根本不进列表，不存在"-1.0 参与统计"这条路径。
        if b is not None:
            boots.append(b)
        if w is not None:
            works.append(w)

    boot = summarize(boots, args.runs, "boot")
    work = summarize(works, args.runs, "workload")
    result = {"boot": boot, "workload": work, "runs": args.runs}

    if boot["median_s"] is not None:
        print(f"[perf] boot     median={boot['median_s']}s "
              f"(n={boot['n']}/{args.runs}, spread={boot['spread_s']}s)")
    else:
        print("[perf] boot     无有效样本")
    if work["median_s"] is not None:
        print(f"[perf] workload median={work['median_s']}s "
              f"(n={work['n']}/{args.runs}, spread={work['spread_s']}s)")
    else:
        print("[perf] workload 无有效样本")

    # 结论只在样本足够时给：1 个样本的"中位数"就是它自己，没有抗抖动意义。
    ok = True
    for name, s in (("boot", boot), ("workload", work)):
        if s["n"] == 0:
            print(f"[perf] {name} 全部失败（超时或测试未通过）")
            ok = False
        elif s["n"] < args.min_samples:
            print(f"[perf] {name} 只有 {s['n']} 个有效样本，"
                  f"低于 --min-samples={args.min_samples}，不给结论")
            ok = False

    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=2))
        print(f"[perf] 已写入 {args.json}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
