#!/usr/bin/env python3
"""HBOS 冷启动时间测量（QEMU 墙钟）。

为什么单独做一把尺子
====================
"启动时间"在 HBOS 里有两个来源，必须分开看，否则优化会打错靶：

  1. **引导器阶段**：固件（SeaBIOS/OVMF）+ 引导器（GRUB/Limine）加载内核，
     以及开机菜单的倒计时。这段时间内核一行代码都没跑。
  2. **内核阶段**：从 `boot.asm` 跳到 `kmain` 到 shell 起来（串口打出
     `[KERN] Shell ready`）。

本工具把两者分别计时：

  * `kernel` 起点 —— 串口出现内核启动横幅（`===== HBOS ... Starting =====`）。
    这一行是 `kmain` 在 `serial_init()` 之后立刻打印的，所以它的时刻≈引导器
    把控制权交给内核的时刻，也就是"引导器阶段"的终点。
    **注意**：不能拿"串口第一个字节"当这个时刻——UEFI 下 OVMF 自己也会往
    串口写控制台输出（BdsDxe 等），会把固件阶段误算进内核阶段。
  * `ready`   —— 串口出现 `[KERN] Shell ready` 的时刻，即内核阶段终点。

计时方式：QEMU 的 `-serial file:` 写文件，本进程以 ~2ms 周期轮询文件增长并
给新出现的整行打上墙钟时间戳（因此不受串口波特率累积延迟影响，也不会因为
一行没写完而误判）。每次测量都重启一个全新的 QEMU 进程，属于真正的冷启动。

用法
====
    python3 scripts/boot_time.py --iso build/hbos-bios.iso --runs 3
    python3 scripts/boot_time.py --iso-uefi build/hbos-uefi.iso --runs 3
    python3 scripts/boot_time.py --iso build/hbos-bios.iso --timeline

输出：每次运行的"引导器阶段/内核阶段/总计"与中位数（中位数比平均值更抗单次
抖动，例如宿主机换页或 QEMU 启动噪声）。
"""

from __future__ import annotations

import argparse
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

# 内核启动横幅里稳定的一段（见 src/kernel.c：kmain 第一件事就是打印它）
KERNEL_BANNER = "Starting ====="

# 内核阶段的完成标记（kmain → shell_run 之前的最后一行串口输出）
READY_MARKER = "[KERN] Shell ready"

# 默认固件路径（OVMF 4M 版本，Debian/Ubuntu 与 Arch 的常见位置）
OVMF_CODE_CANDIDATES = (
    "/usr/share/OVMF/OVMF_CODE_4M.fd",
    "/usr/share/edk2/x64/OVMF_CODE.4m.fd",
    "/usr/share/edk2-ovmf/x64/OVMF_CODE.4m.fd",
)


def find_ovmf() -> str:
    """定位 OVMF 固件；找不到就抛出，避免静默跳过 UEFI 测量。"""
    for path in OVMF_CODE_CANDIDATES:
        if os.path.isfile(path):
            return path
    raise SystemExit("找不到 OVMF 固件（试过：%s）" % ", ".join(OVMF_CODE_CANDIDATES))


class BootRun:
    """一次冷启动的测量结果。"""

    def __init__(self) -> None:
        self.kernel_start: float | None = None  # 内核横幅时刻（引导器阶段终点）
        self.ready: float | None = None         # Shell ready 的时刻
        self.timeline: list[tuple[float, str]] = []
        self.total: float = 0.0

    @property
    def kernel_span(self) -> float | None:
        """内核阶段耗时（横幅 → ready）；缺任一时刻则为 None。"""
        if self.kernel_start is None or self.ready is None:
            return None
        return self.ready - self.kernel_start


def build_command(args: argparse.Namespace, ovmf_vars: str | None) -> list[str]:
    """拼出本次运行要用的 QEMU 命令行。"""
    cmd = [
        args.qemu,
        "-m", args.memory,
        "-serial", "file:{log}",
        "-monitor", "none",
        "-display", "none",
        "-no-reboot",
    ]
    if args.uefi:
        cmd += [
            "-machine", "q35",
            "-drive", "if=pflash,format=raw,readonly=on,file=" + find_ovmf(),
            "-drive", "if=pflash,format=raw,file=" + str(ovmf_vars),
            "-cdrom", args.iso, "-boot", "d",
        ]
    else:
        cmd += ["-cdrom", args.iso, "-boot", "d"]
    return cmd


def run_once(args: argparse.Namespace, ovmf_vars: str | None) -> BootRun:
    """启动一次 QEMU 并测量到 ready 标记（或超时）。"""
    result = BootRun()
    log_fd, log_path = tempfile.mkstemp(prefix="hbos-boot-", suffix=".log")
    os.close(log_fd)
    try:
        cmd = [c.format(log=log_path) for c in build_command(args, ovmf_vars)]
        start = time.perf_counter()
        proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        seen = 0
        try:
            deadline = start + args.timeout
            while time.perf_counter() < deadline:
                time.sleep(0.002)                      # 2ms 轮询：够快且不吃满 CPU
                try:
                    with open(log_path, "rb") as handle:
                        lines = handle.read().decode("utf-8", "replace").splitlines()
                except OSError:
                    continue
                if len(lines) > seen:
                    now = time.perf_counter() - start
                    for line in lines[seen:]:
                        text = line.rstrip()
                        if text.strip():
                            result.timeline.append((now, text))
                            if result.kernel_start is None and KERNEL_BANNER in text:
                                result.kernel_start = now   # 内核横幅 = 引导器交接
                            if READY_MARKER in text:
                                result.ready = now
                    seen = len(lines)
                if result.ready is not None:
                    break
                if proc.poll() is not None:            # QEMU 自己退了（崩溃/关机）
                    break
        finally:
            result.total = time.perf_counter() - start
            proc.kill()
            proc.wait()
    finally:
        try:
            os.unlink(log_path)
        except OSError:
            pass
    return result


def fmt(value: float | None) -> str:
    return "  n/a  " if value is None else f"{value:6.3f}s"


def main() -> int:
    parser = argparse.ArgumentParser(description="测量 HBOS 冷启动时间")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--iso", help="BIOS ISO 路径")
    group.add_argument("--iso-uefi", help="UEFI ISO 路径")
    parser.add_argument("--runs", type=int, default=3, help="运行次数（默认 3）")
    parser.add_argument("--memory", default="512M", help="QEMU 内存（默认 512M）")
    parser.add_argument("--timeout", type=float, default=60.0, help="单次超时秒数")
    parser.add_argument("--qemu", default="qemu-system-x86_64", help="QEMU 可执行文件")
    parser.add_argument("--timeline", action="store_true",
                        help="打印每次运行中每一行串口输出的相对时刻")
    args = parser.parse_args()

    args.uefi = args.iso_uefi is not None
    args.iso = args.iso_uefi if args.uefi else args.iso

    if not os.path.isfile(args.iso):
        raise SystemExit(f"ISO 不存在：{args.iso}（先 make bios-iso / uefi-iso）")
    if not shutil.which(args.qemu):
        raise SystemExit(f"找不到 QEMU：{args.qemu}")

    # UEFI 需要一块可写的 NVRAM，每次运行从只读 CODE 配一份临时 VARS，
    # 避免上一轮的启动项污染下一轮。
    ovmf_vars = None
    if args.uefi:
        candidates = ("/usr/share/OVMF/OVMF_VARS_4M.fd",
                      "/usr/share/edk2/x64/OVMF_VARS.4m.fd",
                      "/usr/share/edk2-ovmf/x64/OVMF_VARS.4m.fd")
        src = next((p for p in candidates if os.path.isfile(p)), None)
        if src is None:
            raise SystemExit("找不到 OVMF VARS 模板")
        fd, ovmf_vars = tempfile.mkstemp(prefix="hbos-ovmf-vars-", suffix=".fd")
        os.close(fd)
        shutil.copyfile(src, ovmf_vars)

    label = "UEFI" if args.uefi else "BIOS"
    print(f"# HBOS 冷启动基准（{label}，{args.runs} 次，ISO={args.iso}）")
    print("# run   固件+引导器    内核       总计")
    runs: list[BootRun] = []
    rc = 0
    try:
        for index in range(1, args.runs + 1):
            result = run_once(args, ovmf_vars)
            runs.append(result)
            status = "ok" if result.ready is not None else "TIMEOUT"
            print(f"  {index:<4} {fmt(result.kernel_start)}"
                  f"    {fmt(result.kernel_span)}  {fmt(result.total)}  {status}")
            if args.timeline:
                for stamp, text in result.timeline:
                    print(f"        {stamp:7.3f}s  {text[:100]}")
            if result.ready is None:
                rc = 1
    except KeyboardInterrupt:
        rc = 130
    finally:
        if ovmf_vars:
            os.unlink(ovmf_vars)

    if runs:
        def median(getter):
            values = [v for v in (getter(r) for r in runs) if v is not None]
            return statistics.median(values) if values else None

        start = median(lambda r: r.kernel_start)
        kernel = median(lambda r: r.kernel_span)
        total = median(lambda r: r.total)
        print(f"  median {fmt(start)}    {fmt(kernel)}  {fmt(total)}")
        if start is not None and total:
            print(f"# 固件+引导器占冷启动的 {100.0 * start / total:.0f}%"
                  f"（内核阶段 {fmt(kernel).strip()}）")
    return rc


if __name__ == "__main__":
    sys.exit(main())
