#!/usr/bin/env bash
#
# HBOS 冷启动基准入口（BIOS + UEFI）。
#
#   scripts/bench_boot.sh              # 各跑 3 次
#   RUNS=5 scripts/bench_boot.sh       # 各跑 5 次
#   ONLY=bios scripts/bench_boot.sh    # 只测 BIOS
#
# 目标是让"启动时间"成为可回归的数字，而不是凭感觉：改动引导配置、内核
# 初始化顺序或链接进来的大 blob 之后，重跑本脚本对比中位数即可。
#
# 参考基线（QEMU 512M，宿主机为普通 x86_64 开发机）：
#   2026-10-01 之前：GRUB 菜单 timeout=5 → BIOS ISO 冷启动 ≈ 7.1s
#   本次改动之后：   BOOT_MENU_TIMEOUT=1  → BIOS ISO 冷启动 ≈ 3.0s（默认）
#                    BOOT_MENU_TIMEOUT=0  → BIOS ISO 冷启动 ≈ 1.9s
# 其中"固件+引导器"约占 3.0s 里的 2.98s（含菜单倒计时与加载 31MB 内核），
# 内核阶段只有约 0.04s。详细拆分见 scripts/boot_time.py 的输出与
# docs/PERFORMANCE.md。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RUNS="${RUNS:-3}"
ONLY="${ONLY:-both}"

measure() {
    local label="$1" flag="$2" iso="$3"
    echo
    echo "=== $label ==="
    python3 scripts/boot_time.py "$flag" "$iso" --runs "$RUNS"
}

case "$ONLY" in
    bios) measure "BIOS ISO" --iso build/hbos-bios.iso ;;
    uefi) measure "UEFI ISO" --iso-uefi build/hbos-uefi.iso ;;
    both)
        measure "BIOS ISO" --iso build/hbos-bios.iso
        measure "UEFI ISO" --iso-uefi build/hbos-uefi.iso
        ;;
    *) echo "ONLY 只能是 bios/uefi/both" >&2; exit 2 ;;
esac
