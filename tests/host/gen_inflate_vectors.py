#!/usr/bin/env python3
"""生成 inflate 差分测试的真值向量（C 头文件）。

真值由宿主 Python 的 zlib 生成（RFC 1950 的标准实现），因此和内核里自己写的
inflate 是**独立实现**，逐字节比对才有意义 —— 这正是 docs/PERFORMANCE.md §1
里"解压正确性"那一栏描述的方法。

向量按构建时生成而不是入库：4 MiB 档的原始+压缩数据接近 20 MB，入库会让仓库
无谓膨胀，而这些数据完全可由本脚本 + zlib 确定性地重放。

用法：python3 tests/host/gen_inflate_vectors.py <输出.h>
"""
import sys
import zlib


def make_raw(seed: int, n: int) -> bytes:
    """混合内容：可压缩的重复模式 + 伪随机。

    纯随机会让 zlib 退化成 stored block，测不到霍夫曼解码路径；纯重复又会
    让码表极度单一。按 3:1 混合既能压出动态霍夫曼块，又能覆盖长匹配复制。
    """
    out = bytearray()
    rnd = (seed * 2654435761 + 1) & 0xFFFFFFFF
    pat = b"HBOS-inflate-benchmark-pattern-0123456789abcdef"
    for i in range(n):
        rnd = (rnd * 1103515245 + 12345) & 0xFFFFFFFF
        if i % 64 < 48:
            out.append(pat[i % len(pat)])
        else:
            out.append((rnd >> 16) & 0xFF)
    return bytes(out)


def emit_array(f, name: str, data: bytes) -> None:
    f.write(f"static const uint8_t {name}[{len(data)}] = {{")
    f.write(",".join(str(b) for b in data))
    f.write("};\n")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    sizes = [(4 << 10, 200), (256 << 10, 8), (4 << 20, 1)]
    cases = []
    for idx, (n, _iters) in enumerate(sizes):
        raw = make_raw(idx + 7, n)
        comp = zlib.compress(raw, 6)
        cases.append((f"dynamic-huffman-{n}", raw, comp))

    with open(sys.argv[1], "w") as f:
        f.write("/* 自动生成，请勿手改：由 tests/host/gen_inflate_vectors.py 生成。\n"
                " * 真值来自宿主 Python zlib（独立实现），用于逐字节差分。 */\n")
        f.write("#ifndef HBOS_TEST_INFLATE_VECTORS_H\n#define HBOS_TEST_INFLATE_VECTORS_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write("typedef struct {\n"
                "    const char *name;\n"
                "    uint32_t raw_len;\n"
                "    const uint8_t *raw;\n"
                "    uint32_t comp_len;\n"
                "    const uint8_t *comp;\n"
                "    int iterations;\n"
                "} inflate_vector_t;\n\n")
        for i, (_name, raw, comp) in enumerate(cases):
            emit_array(f, f"vec{i}_raw", raw)
            emit_array(f, f"vec{i}_comp", comp)
            f.write("\n")
        f.write("static const inflate_vector_t inflate_vectors[] = {\n")
        for i, (name, raw, comp) in enumerate(cases):
            f.write(f'    {{ "{name}", {len(raw)}u, vec{i}_raw, {len(comp)}u, '
                    f'vec{i}_comp, {sizes[i][1]} }},\n')
        f.write("};\n")
        f.write(f"#define INFLATE_VECTOR_COUNT {len(cases)}\n\n#endif\n")

    total = sum(len(c) for _n, _r, c in cases)
    print(f"gen_inflate_vectors: {len(cases)} 组向量，压缩输入合计 {total} B -> {sys.argv[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
