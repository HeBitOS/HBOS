#!/usr/bin/env python3
"""生成 GUI 基准的确定性 PNG 输入（用宿主 zlib 做独立实现）。

为什么要生成而不是入库一个 PNG：入库的图片是二进制黑盒，改不了尺寸/内容，
也没法解释"为什么是这个数"。这里用固定种子生成，任何机器上重放都是同一
份字节，结果可比。

内容按 3:1 混合"重复模式 + 伪随机"，理由同 tests/host/gen_inflate_vectors.py：
纯随机会让 zlib 退化成 stored block（测不到霍夫曼解码），纯重复又让码表
过度单一。这正好对应真实网页里 logo/截图 的解压分布。

用法：python3 tests/host/gen_gui_png.py <输出.png> [宽] [高]
"""
import struct
import sys
import zlib


def make_rgb(w: int, h: int) -> bytes:
    """RGB888 原始像素：可压缩的重复块 + 伪随机噪声。"""
    out = bytearray()
    rnd = 0x1234ABCD
    pat = bytes(range(0, 64)) * 4
    for y in range(h):
        for x in range(w):
            rnd = (rnd * 1103515245 + 12345) & 0xFFFFFFFF
            if (x // 8 + y // 8) % 4 < 3:
                i = (x * 3 + y * 5) % len(pat)
                out += bytes((pat[i], pat[(i + 21) % len(pat)],
                              pat[(i + 42) % len(pat)]))
            else:
                out += bytes((rnd & 0xFF, (rnd >> 8) & 0xFF,
                              (rnd >> 16) & 0xFF))
    return bytes(out)


def chunk(tag: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + tag + payload +
            struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


def make_png(w: int, h: int) -> bytes:
    """8 位真彩、非隔行 —— 正是 src/png.c 支持的子集。"""
    raw = make_rgb(w, h)
    # 每行前加一个滤波字节 0（None），让 png.c 走"不滤波"路径。
    stride = w * 3
    filtered = b"".join(b"\x00" + raw[y * stride:(y + 1) * stride]
                        for y in range(h))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(filtered, 6)) +
            chunk(b"IEND", b""))


def main() -> int:
    out = sys.argv[1] if len(sys.argv) > 1 else "build/tmp/bench-gui/in.png"
    w = int(sys.argv[2]) if len(sys.argv) > 2 else 540
    h = int(sys.argv[3]) if len(sys.argv) > 3 else 360
    # 对齐 src/png.c 的上限（PNG_MAX_W 560 / PNG_MAX_H 384）：超出会返回 -1，
    # 基准就会变成"测了一个必然失败的解码"。
    if w > 560 or h > 384:
        raise SystemExit(f"尺寸超 png.c 上限 560x384：{w}x{h}")
    data = make_png(w, h)
    with open(out, "wb") as f:
        f.write(data)
    print(f"[gen_gui_png] {out}  {w}x{h}  {len(data)} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
