#!/usr/bin/env python3
"""Bundle HBOS's user-mode libc headers into one blob for TCC's sysinclude
path (see src/tools/tcc_headers_seed.c). HBOS's ramfs starts empty at boot,
so these need to be seeded from the kernel image, same incbin-blob pattern
as build/tcc/hbos_runtime.o.

Manifest format (all integers little-endian):
    [u32 count]
    count * {
        [u16 name_len][name bytes, e.g. "stdio.h" or "sys/stat.h"]
        [u32 data_len][data bytes]
    }
"""
import struct
import sys
from pathlib import Path

def main():
    # Last arg is the output path; every arg before it is a source dir
    # whose *.h and sys/*.h get bundled (in order — later dirs' files win
    # on a name collision, since none currently exists between HBOS's own
    # libc headers and TinyCC's tiny freestanding compiler-support ones).
    out_path = Path(sys.argv[-1])
    src_dirs = [Path(p) for p in sys.argv[1:-1]]

    entries = {}
    for src_dir in src_dirs:
        files = sorted(src_dir.glob("*.h")) + sorted((src_dir / "sys").glob("*.h"))
        for f in files:
            rel = f.relative_to(src_dir).as_posix()
            entries[rel] = f.read_bytes()

    with open(out_path, "wb") as out:
        out.write(struct.pack("<I", len(entries)))
        for rel, data in sorted(entries.items()):
            name_b = rel.encode("utf-8")
            out.write(struct.pack("<H", len(name_b)))
            out.write(name_b)
            out.write(struct.pack("<I", len(data)))
            out.write(data)

    print(f"[genheaders] {len(entries)} header(s) -> {out_path}")

if __name__ == "__main__":
    main()
