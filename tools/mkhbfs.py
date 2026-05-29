#!/usr/bin/env python3
import argparse
import struct

SECTOR_SIZE = 512
MAX_FILES = 64
MAX_FILE_SIZE = 8192
HBFS_START_LBA = 2048
HBFS_TABLE_SECTORS = 8
HBFS_FILE_SECTORS = MAX_FILE_SIZE // SECTOR_SIZE
HBFS_DATA_LBA = HBFS_START_LBA + 1 + HBFS_TABLE_SECTORS
HBFS_MAGIC = 0x3146534248
HBFS_VERSION = 1
HBFS_PARTITION_TYPE = 0xEB


def main() -> int:
    parser = argparse.ArgumentParser(description="Create a blank HBFS disk image")
    parser.add_argument("image")
    parser.add_argument("--size-mib", type=int, default=16)
    parser.add_argument("--no-mbr", action="store_true")
    args = parser.parse_args()

    size = args.size_mib * 1024 * 1024
    needed = (HBFS_DATA_LBA + MAX_FILES * HBFS_FILE_SECTORS) * SECTOR_SIZE
    if size < needed:
        raise SystemExit(f"image too small, need at least {needed // (1024 * 1024) + 1} MiB")

    superblock = bytearray(SECTOR_SIZE)
    struct.pack_into(
        "<QIIIIIIII",
        superblock,
        0,
        HBFS_MAGIC,
        HBFS_VERSION,
        HBFS_START_LBA,
        MAX_FILES,
        MAX_FILE_SIZE,
        HBFS_START_LBA + 1,
        HBFS_TABLE_SECTORS,
        HBFS_DATA_LBA,
        HBFS_FILE_SECTORS,
    )

    with open(args.image, "wb") as f:
        f.truncate(size)
        if not args.no_mbr:
            mbr = bytearray(SECTOR_SIZE)
            part = 446
            mbr[part + 0] = 0x80
            mbr[part + 1:part + 4] = b"\x01\x01\x00"
            mbr[part + 4] = HBFS_PARTITION_TYPE
            mbr[part + 5:part + 8] = b"\xfe\xff\xff"
            sectors = size // SECTOR_SIZE - HBFS_START_LBA
            struct.pack_into("<II", mbr, part + 8, HBFS_START_LBA, sectors)
            mbr[510:512] = b"\x55\xaa"
            f.seek(0)
            f.write(mbr)
        f.seek(HBFS_START_LBA * SECTOR_SIZE)
        f.write(superblock)
        f.write(bytes(HBFS_TABLE_SECTORS * SECTOR_SIZE))

    print(f"HBFS image: {args.image} ({args.size_mib} MiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
