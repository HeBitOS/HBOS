#!/usr/bin/env python3
"""
hbfs-copy.py — Copy files from host into an HBFS partition image.

Usage:
  hbfs-copy.py <image> <host_file> [remote_name] [--offset <bytes>]
  hbfs-copy.py <image> --list [--offset <bytes>]
  hbfs-copy.py <image> --delete <remote_name> [--offset <bytes>]

Examples:
  # Copy into a standalone HBFS image (no offset needed)
  hbfs-copy.py hbfs.img hello.c

  # Copy into an installed disk image (HBFS partition at LBA 2048)
  hbfs-copy.py hbos_installed_bios.img hello.c --offset 1048576

  # List files in the image
  hbfs-copy.py hbfs.img --list

  # Delete a file
  hbfs-copy.py hbfs.img --delete hello.c
"""
import argparse
import struct
import os
import sys

SECTOR_SIZE = 512
MAX_FILES = 64
MAX_FILENAME = 32
DEFAULT_MAX_FILE_SIZE = 65536
HBFS_MAGIC = 0x3146534248

# Superblock layout: magic(8) + version(4) + start_lba(4) + max_files(4) +
# max_file_size(4) + table_lba(4) + table_sectors(4) + data_lba(4) + file_sectors(4)
SUPER_FMT = "<QIIIIIIII"
SUPER_SIZE = struct.calcsize(SUPER_FMT)  # 40 bytes

# File entry layout: used(1) + type(1) + reserved0(2) + size(4) + name(32) + reserved1(24)
ENTRY_FMT = "<BBHI32s24x"
ENTRY_SIZE = 64  # bytes per entry


def find_hbfs_offset(f):
    """Auto-detect HBFS partition offset by scanning for magic at common locations."""
    candidates = [
        0,                    # Standalone HBFS with MBR at LBA 0
        2048 * SECTOR_SIZE,   # mkhbfs.py default (LBA 2048)
        133120 * SECTOR_SIZE, # Installed disk image (after 64 MiB FAT)
    ]
    for off in candidates:
        f.seek(off)
        data = f.read(8)
        if len(data) == 8:
            magic = struct.unpack("<Q", data)[0]
            if magic == HBFS_MAGIC:
                return off
    return None


def read_super(f, offset):
    if offset < 0:
        # Auto-detect
        offset = find_hbfs_offset(f)
        if offset is None:
            raise SystemExit("HBFS partition not found (try --offset)")
    f.seek(offset)
    data = f.read(SECTOR_SIZE)
    if len(data) < SECTOR_SIZE:
        raise SystemExit("Image too small to contain HBFS superblock")
    vals = struct.unpack_from(SUPER_FMT, data, 0)
    magic, version, start_lba, max_files, max_file_size, table_lba, table_sectors, data_lba, file_sectors = vals
    if magic != HBFS_MAGIC:
        raise SystemExit(f"Bad HBFS magic: 0x{magic:X} (expected 0x{HBFS_MAGIC:X})")
    return {
        "version": version,
        "start_lba": start_lba,
        "max_files": max_files,
        "max_file_size": max_file_size,
        "table_lba": table_lba,
        "table_sectors": table_sectors,
        "data_lba": data_lba,
        "file_sectors": file_sectors,
    }


def hbfs_lba_offset(offset, sb, lba):
    """Convert an HBFS on-disk LBA field to a byte offset in the image."""
    if lba >= sb["start_lba"]:
        return lba * SECTOR_SIZE
    return offset + lba * SECTOR_SIZE


def read_entries(f, offset, sb):
    table_offset = hbfs_lba_offset(offset, sb, sb["table_lba"])
    f.seek(table_offset)
    data = f.read(sb["table_sectors"] * SECTOR_SIZE)
    entries = []
    for i in range(sb["max_files"]):
        off = i * ENTRY_SIZE
        used, type_, _, size, name_raw = struct.unpack_from(ENTRY_FMT, data, off)
        name = name_raw.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
        entries.append({"used": used, "type": type_, "size": size, "name": name, "slot": i})
    return entries


def write_entry(f, offset, sb, slot, entry):
    table_offset = hbfs_lba_offset(offset, sb, sb["table_lba"]) + slot * ENTRY_SIZE
    f.seek(table_offset)
    name_bytes = entry["name"].encode("utf-8")[:MAX_FILENAME - 1].ljust(MAX_FILENAME, b"\x00")
    f.write(struct.pack(ENTRY_FMT, entry["used"], entry["type"], 0, entry["size"], name_bytes))


def write_file_data(f, offset, sb, slot, data):
    data_offset = hbfs_lba_offset(offset, sb, sb["data_lba"]) + slot * sb["file_sectors"] * SECTOR_SIZE
    f.seek(data_offset)
    # Pad to full sector
    padded = data + b"\x00" * (sb["file_sectors"] * SECTOR_SIZE - len(data))
    f.write(padded[:sb["file_sectors"] * SECTOR_SIZE])


def cmd_list(image, offset):
    with open(image, "rb") as f:
        sb = read_super(f, offset)
        entries = read_entries(f, offset, sb)
    print(f"HBFS v{sb['version']}  max_files={sb['max_files']}  max_file_size={sb['max_file_size']}")
    print(f"{'Slot':>4}  {'Used':>4}  {'Size':>6}  Name")
    print("-" * 50)
    count = 0
    for e in entries:
        if e["used"]:
            print(f"{e['slot']:>4}  {'Y':>4}  {e['size']:>6}  {e['name']}")
            count += 1
    print(f"\n{count}/{sb['max_files']} files used")


def cmd_copy(image, offset, host_file, remote_name):
    if not os.path.exists(host_file):
        raise SystemExit(f"Host file not found: {host_file}")

    data = open(host_file, "rb").read()

    if remote_name is None:
        remote_name = os.path.basename(host_file)

    if len(remote_name) >= MAX_FILENAME:
        raise SystemExit(f"Remote name too long: {len(remote_name)} chars (max {MAX_FILENAME - 1})")

    with open(image, "r+b") as f:
        sb = read_super(f, offset)
        max_file_size = min(sb["max_file_size"] or DEFAULT_MAX_FILE_SIZE,
                            sb["file_sectors"] * SECTOR_SIZE)
        if len(data) > max_file_size:
            raise SystemExit(f"File too large: {len(data)} bytes (max {max_file_size})")
        entries = read_entries(f, offset, sb)

        # Check if file already exists
        for e in entries:
            if e["used"] and e["name"] == remote_name:
                # Overwrite existing
                print(f"Overwriting '{remote_name}' in slot {e['slot']} ({len(data)} bytes)")
                write_file_data(f, offset, sb, e["slot"], data)
                e["size"] = len(data)
                write_entry(f, offset, sb, e["slot"], e)
                print("Done.")
                return

        # Find a free slot
        free_slot = None
        for e in entries:
            if not e["used"]:
                free_slot = e["slot"]
                break

        if free_slot is None:
            raise SystemExit("No free slots in HBFS partition")

        print(f"Writing '{remote_name}' to slot {free_slot} ({len(data)} bytes)")
        write_file_data(f, offset, sb, free_slot, data)
        entry = {"used": 1, "type": 0, "size": len(data), "name": remote_name, "slot": free_slot}
        write_entry(f, offset, sb, free_slot, entry)
        print("Done.")


def cmd_delete(image, offset, remote_name):
    with open(image, "r+b") as f:
        sb = read_super(f, offset)
        entries = read_entries(f, offset, sb)

        for e in entries:
            if e["used"] and e["name"] == remote_name:
                print(f"Deleting '{remote_name}' from slot {e['slot']}")
                e["used"] = 0
                e["size"] = 0
                write_entry(f, offset, sb, e["slot"], e)
                print("Done.")
                return

        raise SystemExit(f"File not found: {remote_name}")


def main():
    parser = argparse.ArgumentParser(description="Copy files into HBFS partition images")
    parser.add_argument("image", help="Path to the disk/HBFS image")
    parser.add_argument("host_file", nargs="?", help="Local file to copy")
    parser.add_argument("remote_name", nargs="?", help="Remote filename (default: basename of host_file)")
    parser.add_argument("--offset", type=int, default=-1,
                        help="Byte offset to HBFS partition in image (default: auto-detect)")
    parser.add_argument("--list", action="store_true", help="List files in the image")
    parser.add_argument("--delete", metavar="NAME", help="Delete a file from the image")
    args = parser.parse_args()

    if args.list:
        cmd_list(args.image, args.offset)
    elif args.delete:
        cmd_delete(args.image, args.offset, args.delete)
    elif args.host_file:
        cmd_copy(args.image, args.offset, args.host_file, args.remote_name)
    else:
        parser.print_help()
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
