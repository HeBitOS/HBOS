#!/usr/bin/env python3
"""把 HBOS 串口日志里的用户态地址翻译成 Chromium 符号。

用法:
    python3 scripts/symbolize_rip.py 0x1108C3434D 0x1107465B1A ...

地址约定（见 docs/ROADMAP-C1-C3-RESULT.md）:
  Chromium 主程序（PIE）加载在 0x1100000000，符号文件里是相对偏移；
  共享库与 ld.so 落在 0x100000000000 起，需要各自的 debug 包，这里跳过。

符号表来源（一次性准备，约 176MB）:
  curl -O http://deb.debian.org/debian-debug/pool/main/c/chromium/\
chromium-dbgsym_<版本>_amd64.deb
  ar x 该包 && tar -xf data.tar.xz
  nm -n -C --defined-only usr/lib/debug/.build-id/*/*.debug > build/tmp/chromium.syms
"""
import bisect
import sys

PIE_BASE = 0x1100000000
SYMS = "build/tmp/chromium.syms"


def load(path):
    addrs, names = [], []
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            parts = line.rstrip("\n").split(" ", 2)
            if len(parts) < 3:
                continue
            try:
                addr = int(parts[0], 16)
            except ValueError:
                continue
            addrs.append(addr)
            names.append(parts[2])
    return addrs, names


def main():
    addrs, names = load(SYMS)
    for token in sys.argv[1:]:
        value = int(token, 16)
        if PIE_BASE <= value < PIE_BASE + 0x12000000:
            offset = value - PIE_BASE
        else:
            print(f"{token}  非 Chromium PIE 地址（共享库），跳过")
            continue
        index = bisect.bisect_right(addrs, offset) - 1
        if index < 0:
            print(f"{token}  无符号")
            continue
        print(f"{token}  +0x{offset - addrs[index]:x}  {names[index]}")


if __name__ == "__main__":
    main()
