#!/usr/bin/env python3
"""Build the built-in HPT repository from the HAX apps bundled in the kernel
image (build/app/*.hax), with a FLAT layout that fits HBOS's ramfs naming
limit (full normalized path <= MAX_FILENAME-1 == 31 chars):

    /packages/Packages
    /packages/<name>.hax        (payload, short name instead of pool/x_y_z.hax)

Only apps with a valid .haxmeta whose payload fits RAMFS_MAX_FILE_SIZE
(64 KiB) are included — larger resident tools (tcc.hax) and hpt.hax itself
don't need to be (re)installable.

Usage: genhpt_repo.py <apps-dir> <out-repo-dir>
"""

import hashlib
import shutil
import struct
import sys
from pathlib import Path

HAX_META_MAGIC = 0x4D584148
RAMFS_MAX_FILE_SIZE = 131072
VERSION = "0.1.0"
NAME = "x86_64"


def read_hax_meta(path: Path):
    data = path.read_bytes()
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01":
        return None
    section_offset = struct.unpack_from("<Q", data, 0x28)[0]
    section_size = struct.unpack_from("<H", data, 0x3A)[0]
    section_count = struct.unpack_from("<H", data, 0x3C)[0]
    names_index = struct.unpack_from("<H", data, 0x3E)[0]
    if (
        section_size < 64
        or names_index >= section_count
        or section_offset + section_size * section_count > len(data)
    ):
        return None

    def section(index):
        offset = section_offset + section_size * index
        return (
            struct.unpack_from("<I", data, offset)[0],
            struct.unpack_from("<Q", data, offset + 24)[0],
            struct.unpack_from("<Q", data, offset + 32)[0],
        )

    _, names_offset, names_size = section(names_index)
    names = data[names_offset : names_offset + names_size]
    for index in range(section_count):
        name_offset, payload_offset, payload_size = section(index)
        if name_offset >= len(names):
            continue
        end = names.find(b"\0", name_offset)
        if end < 0 or names[name_offset:end] != b".haxmeta":
            continue
        if payload_size < 104 or payload_offset + payload_size > len(data):
            return None
        metadata = data[payload_offset : payload_offset + 104]
        magic, kind = struct.unpack_from("<II", metadata)
        if magic != HAX_META_MAGIC or kind == 0:
            return None
        name = metadata[8:40].split(b"\0", 1)[0].decode("utf-8", "replace")
        description = metadata[40:104].split(b"\0", 1)[0].decode("utf-8", "replace")
        return name, description
    return None


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: genhpt_repo.py <apps-dir> <out-repo-dir>", file=sys.stderr)
        return 2
    apps_dir = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    records = []
    for hax in sorted(apps_dir.glob("*.hax")):
        size = hax.stat().st_size
        if size > RAMFS_MAX_FILE_SIZE:
            print(f"[genhpt_repo] skip {hax.name}: {size} B > {RAMFS_MAX_FILE_SIZE} B")
            continue
        meta = read_hax_meta(hax)
        if meta is None:
            print(f"[genhpt_repo] skip {hax.name}: no .haxmeta")
            continue
        name, description = meta
        # flat payload name must keep the full path under MAX_FILENAME-1 (31)
        payload_name = f"{name}.hax"
        if len(f"packages/{payload_name}") > 31:
            print(f"[genhpt_repo] skip {name}: payload path too long")
            continue
        payload = hax.read_bytes()
        digest = hashlib.sha256(payload).hexdigest()
        dst = out_dir / payload_name
        shutil.copyfile(hax, dst)
        records.append(
            f"{name}|{VERSION}|{NAME}|{len(payload)}|{digest}|"
            f"{payload_name}||{description}\n"
        )

    if not records:
        print("[genhpt_repo] no bundled packages (core-only build)")
        (out_dir / "Packages").write_text("", encoding="utf-8")
        return 0
    (out_dir / "Packages").write_text("".join(records), encoding="utf-8")
    print(f"[genhpt_repo] {len(records)} packages -> {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
