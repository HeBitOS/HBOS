#!/usr/bin/env python3
"""Bundle an HPT repository (Packages index + pool payloads) into a single
blob embedded in the kernel image and seeded into ramfs at boot, following
the same incbin-blob pattern as tools/genheaders.py (see
src/tools/tcc_runtime_seed.c).

Blob format (all integers little-endian):
    [u32 count]
    count * {
        [u16 name_len][name bytes, e.g. "Packages" or "pool/cat.hax"]
        [u32 data_len][data bytes]
    }

Names are relative to the repository root, so the boot-time seeder can
recreate the tree under /packages.

Usage: genhptblob.py <repo-dir> <out-blob.bin>
"""

import struct
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: genhptblob.py <repo-dir> <out-blob.bin>", file=sys.stderr)
        return 2
    repo = Path(sys.argv[1])
    out_path = Path(sys.argv[2])

    entries = []
    packages_file = repo / "Packages"
    if not packages_file.is_file():
        print(f"genhptblob: {packages_file} not found", file=sys.stderr)
        return 1
    entries.append(("Packages", packages_file.read_bytes()))
    for payload in sorted(repo.iterdir()):
        if payload.is_file() and payload.name != "Packages":
            entries.append((payload.name, payload.read_bytes()))

    with open(out_path, "wb") as out:
        out.write(struct.pack("<I", len(entries)))
        for name, data in entries:
            name_b = name.encode("utf-8")
            out.write(struct.pack("<H", len(name_b)))
            out.write(name_b)
            out.write(struct.pack("<I", len(data)))
            out.write(data)

    total = sum(len(d) for _, d in entries)
    print(f"[genhptblob] {len(entries)} file(s), {total} bytes -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
