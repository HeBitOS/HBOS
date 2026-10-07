#!/usr/bin/env python3
"""把内核内嵌的 GUI 资源（字体/壁纸）压成 zlib 流，供构建期嵌入。

背景：build/gui_font.bin（20.4MB）与 build/gui_wall.bin（5.8MB）直接以
.incbin 进内核 .rodata，占了内核体积的大头（也是 ISO 66MB 的主要来源）。
两者都是高冗余的位图数据，zlib 能压到 32~47%。

内核已有 inflate_zlib()（RFC1950，PNG 的 IDAT 就走它），所以运行时解压是
现成能力。输出格式（8 字节头 + zlib 流）：

    magic 'Z' 'B' 'L' '1'   (4B)
    u32  原始长度            (4B, 小端)
    zlib 流                  (剩余字节)

解压方先用头里的原始长度分配目标缓冲（pmm_alloc_blocks 连续物理页），
再 inflate_zlib() 校验解压。原样未压缩时（--raw）输出格式不变，只是没有
zlib 流——解压方据 magic 区分，便于渐进接入或调试。

用法: pack_blob.py <输入> <输出> [--raw]
"""
import sys
import struct
import zlib


MAGIC = b"ZBL1"


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: pack_blob.py <input> <output> [--raw]", file=sys.stderr)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    raw_mode = "--raw" in sys.argv[3:]

    with open(src, "rb") as f:
        data = f.read()

    if raw_mode:
        payload = data
    else:
        payload = zlib.compress(data, 9)

    out = MAGIC + struct.pack("<I", len(data)) + payload
    with open(dst, "wb") as f:
        f.write(out)

    ratio = (100 * len(out) // len(data)) if data else 0
    print(f"[pack] {src}: {len(data)} -> {len(out)} bytes ({ratio}%)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
