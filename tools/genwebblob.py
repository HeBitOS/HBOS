#!/usr/bin/env python3
"""Bundle web-vendor files (Vue runtime etc.) into one blob for seeding into
ramfs at boot (see src/tools/web_vendor_seed.c). Same incbin-blob pattern and
manifest format as tools/genheaders.py / tools/genhptblob.py:

    [u32 count]
    count * {
        [u16 name_len][name bytes, e.g. "vue.global.prod.js"]
        [u32 data_len][data bytes]
    }

Usage: genwebblob.py <src-dir> <out-blob>

Every file under <src-dir> gets bundled; names are relative paths so
third_party/web-vendor/vue.global.prod.js lands at /system/vue.global.prod.js.
"""
import struct
import sys
from pathlib import Path


def main():
    src_dir = Path(sys.argv[1])
    out_path = Path(sys.argv[2])

    entries = {}
    for f in sorted(src_dir.rglob("*")):
        if f.is_file():
            entries[f.relative_to(src_dir).as_posix()] = f.read_bytes()

    with open(out_path, "wb") as out:
        out.write(struct.pack("<I", len(entries)))
        for name, data in sorted(entries.items()):
            name_b = name.encode("utf-8")
            out.write(struct.pack("<H", len(name_b)))
            out.write(name_b)
            out.write(struct.pack("<I", len(data)))
            out.write(data)

    print(f"[genwebblob] {len(entries)} file(s) -> {out_path}")


if __name__ == "__main__":
    main()
