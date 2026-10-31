#!/usr/bin/env bash
#
# GUI 侧宿主基准：gui_blit_argb8888 的帧成本 + PNG 解码吞吐。
#
#   bash scripts/bench_gui.sh            # 默认 200 帧
#   FRAMES=500 bash scripts/bench_gui.sh
#
# 为什么是宿主基准：QEMU/TCG 下这些数字被串口输出和译码预热淹没，且要独占
# QEMU。这里测的是同一份内核源码（src/tools/gui.c 的 blit、src/png.c +
# src/inflate.c 的解码），输入确定性、无 I/O，重复跑稳定。
#
# 怎么只测到 blit 而不把整个 GUI 链进来：
#   gui.c 有 291 个外部符号（应用、文件系统、控制台…），全打桩不现实。
#   用 -ffunction-sections 编译 + --gc-sections 链接，最终产物里只保留
#   被测函数真正用到的那几个段，其余符号根本不需要解析。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD="${BUILD_DIR:-build}"
OUT="$BUILD/tmp/bench-gui"
FRAMES="${FRAMES:-200}"
PNG_W="${PNG_W:-540}"
PNG_H="${PNG_H:-360}"

mkdir -p "$OUT"

# 和内核一致的代码生成选项里，只保留影响被测函数性能的那几条
# （-O2、-mno-sse2 等）。-ffreestanding/-mcmodel=kernel 不必要且会和
# 宿主 glibc 打架，所以不加。
GUI_CFLAGS=(-O2 -fno-stack-protector -mno-sse -mno-sse2 -mno-mmx
            -mno-80387 -ffunction-sections -fdata-sections
            -Isrc -Isrc/gui -Isrc/gui/apps -Isrc/gui/wayland -Isrc/ui
            -include tests/host/hbos_shim.h)

echo "[bench-gui] 编译 gui.c（只保留被测函数所在的段）"
gcc -c "${GUI_CFLAGS[@]}" src/tools/gui.c -o "$OUT/gui.o"

echo "[bench-gui] 编译 png.c + inflate.c（真实内核实现）"
gcc -c -O2 -Isrc -include tests/host/hbos_shim.h \
    src/png.c -o "$OUT/png.o"
gcc -c -O2 -Isrc -include tests/host/hbos_shim.h \
    src/inflate.c -o "$OUT/inflate.o"

echo "[bench-gui] 生成确定性 PNG 输入"
python3 tests/host/gen_gui_png.py "$OUT/in.png" "$PNG_W" "$PNG_H"

echo "[bench-gui] 链接"
gcc -O2 tests/host/bench_gui.c "$OUT/gui.o" "$OUT/png.o" "$OUT/inflate.o" \
    tests/host/hbos_string_wrap.c \
    -Wl,--gc-sections -o "$OUT/bench_gui"

echo
"$OUT/bench_gui" "$OUT/in.png" "$FRAMES"
