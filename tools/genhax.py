#!/usr/bin/env python3
"""
genhax.py —— HAX 应用打包器
============================
扫描已编译的 .hax 应用（标准 ELF64），读取每个文件的 .haxmeta 自描述段，
生成：
  * <blob>     ：所有 .hax 二进制按 16 字节对齐拼接成的总 blob（incbin 进内核）
  * <manifest> ：C 源文件，定义 hax_app_table[] / hax_app_table_count

用法：
  genhax.py --blob build/hax_blob.bin --manifest build/hax_manifest.c app/*.hax
"""

import argparse
import os
import struct
import sys

HAX_META_MAGIC = 0x4D584148   # 'HAXM'
META_SIZE = 4 + 4 + 32 + 64   # magic + kind + name[32] + desc[64]


def read_haxmeta(elf):
    """从 ELF64 字节中提取 .haxmeta 段，返回 (kind, name, desc) 或 None。"""
    if len(elf) < 64 or elf[:4] != b'\x7fELF' or elf[4] != 2:  # ELFCLASS64
        return None
    e_shoff = struct.unpack_from('<Q', elf, 0x28)[0]
    e_shentsize = struct.unpack_from('<H', elf, 0x3a)[0]
    e_shnum = struct.unpack_from('<H', elf, 0x3c)[0]
    e_shstrndx = struct.unpack_from('<H', elf, 0x3e)[0]
    if e_shoff == 0 or e_shnum == 0 or e_shstrndx >= e_shnum:
        return None

    def shdr(i):
        base = e_shoff + i * e_shentsize
        name = struct.unpack_from('<I', elf, base + 0)[0]
        off = struct.unpack_from('<Q', elf, base + 24)[0]
        size = struct.unpack_from('<Q', elf, base + 32)[0]
        return name, off, size

    _, str_off, str_size = shdr(e_shstrndx)
    strtab = elf[str_off:str_off + str_size]

    def secname(name_off):
        end = strtab.find(b'\x00', name_off)
        return strtab[name_off:end].decode('latin1')

    for i in range(e_shnum):
        nm, off, size = shdr(i)
        if secname(nm) == '.haxmeta' and size >= META_SIZE:
            blob = elf[off:off + META_SIZE]
            magic, kind = struct.unpack_from('<II', blob, 0)
            if magic != HAX_META_MAGIC:
                return None
            name = blob[8:40].split(b'\x00', 1)[0].decode('utf-8', 'replace')
            desc = blob[40:104].split(b'\x00', 1)[0].decode('utf-8', 'replace')
            return kind, name, desc
    return None


def c_string(s):
    out = []
    for ch in s.encode('utf-8'):
        if ch == ord('"'):
            out.append('\\"')
        elif ch == ord('\\'):
            out.append('\\\\')
        elif 0x20 <= ch < 0x7f:
            out.append(chr(ch))
        else:
            out.append('\\%03o' % ch)
    return '"' + ''.join(out) + '"'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--blob', required=True)
    ap.add_argument('--manifest', required=True)
    ap.add_argument('inputs', nargs='*')
    args = ap.parse_args()

    entries = []   # (name, desc, kind, offset, size)
    blob = bytearray()
    seen = set()

    for path in sorted(args.inputs):
        with open(path, 'rb') as f:
            elf = f.read()
        meta = read_haxmeta(elf)
        if meta is None:
            # 回退：用文件名做应用名，默认 TUI
            name = os.path.splitext(os.path.basename(path))[0]
            kind, desc = 1, '(no .haxmeta; treated as TUI)'
            print(f"[genhax] WARN {path}: 缺少有效 .haxmeta，按 TUI/{name} 处理",
                  file=sys.stderr)
        else:
            kind, name, desc = meta
        if name in seen:
            print(f"[genhax] WARN 应用名重复: {name}（跳过 {path}）", file=sys.stderr)
            continue
        seen.add(name)

        if len(blob) % 16:
            blob.extend(b'\x00' * (16 - len(blob) % 16))
        offset = len(blob)
        blob.extend(elf)
        entries.append((name, desc, kind, offset, len(elf)))
        print(f"[genhax] + {name:<16} kind={kind} size={len(elf)} @ {offset}")

    if not blob:
        blob.append(0)   # 保证 incbin 符号有效，避免空文件

    with open(args.blob, 'wb') as f:
        f.write(blob)

    lines = [
        '/* 自动生成于 tools/genhax.py —— 请勿手改 */',
        '#include "user/hax_app.h"',
        '',
        'const hax_app_entry_t hax_app_table[] = {',
    ]
    for name, desc, kind, offset, size in entries:
        lines.append('    { %s, %s, %uu, %uu, %uu },' %
                     (c_string(name), c_string(desc), kind, offset, size))
    lines.append('};')
    lines.append('const uint32_t hax_app_table_count = %u;' % len(entries))
    lines.append('')
    with open(args.manifest, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))

    print(f"[genhax] {len(entries)} app(s), blob={len(blob)} bytes -> {args.blob}")


if __name__ == '__main__':
    main()
