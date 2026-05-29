#!/usr/bin/env python3
import argparse
import os
import struct
import subprocess

SECTOR_SIZE = 512
IMAGE_MIB = 128
FAT_START = 2048
FAT_SECTORS = 131072  # 64 MiB
HBFS_START = FAT_START + FAT_SECTORS
HBFS_PARTITION_TYPE = 0xEB
FAT32_LBA_TYPE = 0x0C

MAX_FILES = 64
MAX_FILE_SIZE = 8192
HBFS_TABLE_SECTORS = 8
HBFS_FILE_SECTORS = MAX_FILE_SIZE // SECTOR_SIZE
HBFS_MAGIC = 0x3146534248
HBFS_VERSION = 1


def run(cmd: list[str]) -> None:
    subprocess.run(cmd, check=True)


def put_partition(mbr: bytearray, index: int, ptype: int, start: int, sectors: int, bootable: bool) -> None:
    off = 446 + index * 16
    mbr[off + 0] = 0x80 if bootable else 0x00
    mbr[off + 1:off + 4] = b"\x01\x01\x00"
    mbr[off + 4] = ptype
    mbr[off + 5:off + 8] = b"\xfe\xff\xff"
    struct.pack_into("<II", mbr, off + 8, start, sectors)


def write_hbfs(image: str, start: int, sectors: int) -> None:
    superblock = bytearray(SECTOR_SIZE)
    struct.pack_into(
        "<QIIIIIIII",
        superblock,
        0,
        HBFS_MAGIC,
        HBFS_VERSION,
        start,
        MAX_FILES,
        MAX_FILE_SIZE,
        start + 1,
        HBFS_TABLE_SECTORS,
        start + 1 + HBFS_TABLE_SECTORS,
        HBFS_FILE_SECTORS,
    )
    with open(image, "r+b") as f:
        f.seek(start * SECTOR_SIZE)
        f.write(superblock)
        f.write(bytes(HBFS_TABLE_SECTORS * SECTOR_SIZE))
    _ = sectors


def main() -> int:
    parser = argparse.ArgumentParser(description="Create bootable HBOS BIOS/UEFI disk image")
    parser.add_argument("image")
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--limine-conf", required=True)
    parser.add_argument("--limine-dir", default="limine-bin/bin")
    parser.add_argument("--size-mib", type=int, default=IMAGE_MIB)
    args = parser.parse_args()

    size = args.size_mib * 1024 * 1024
    total_sectors = size // SECTOR_SIZE
    hbfs_sectors = total_sectors - HBFS_START
    if hbfs_sectors <= 1 + HBFS_TABLE_SECTORS + MAX_FILES * HBFS_FILE_SECTORS:
        raise SystemExit("image too small for FAT boot partition + HBFS")

    with open(args.image, "wb") as f:
        f.truncate(size)
        mbr = bytearray(SECTOR_SIZE)
        put_partition(mbr, 0, FAT32_LBA_TYPE, FAT_START, FAT_SECTORS, True)
        put_partition(mbr, 1, HBFS_PARTITION_TYPE, HBFS_START, hbfs_sectors, False)
        mbr[510:512] = b"\x55\xaa"
        f.seek(0)
        f.write(mbr)

    fat_blocks_1k = FAT_SECTORS // 2
    offset_arg = f"--offset={FAT_START}"
    run(["mkfs.fat", "-F", "32", "-n", "HBOSBOOT", offset_arg, args.image, str(fat_blocks_1k)])

    fat_img = f"{args.image}@@{FAT_START * SECTOR_SIZE}"
    run(["mmd", "-i", fat_img, "::/EFI", "::/EFI/BOOT", "::/boot"])
    run(["mcopy", "-i", fat_img, os.path.join(args.limine_dir, "BOOTX64.EFI"), "::/EFI/BOOT/BOOTX64.EFI"])
    run(["mcopy", "-i", fat_img, os.path.join(args.limine_dir, "limine-bios.sys"), "::/limine-bios.sys"])
    run(["mcopy", "-i", fat_img, args.limine_conf, "::/limine.conf"])
    run(["mcopy", "-i", fat_img, args.kernel, "::/boot/hbos.bin"])

    write_hbfs(args.image, HBFS_START, hbfs_sectors)
    run([os.path.join(args.limine_dir, "limine"), "bios-install", args.image])

    print(f"HBOS boot disk: {args.image} ({args.size_mib} MiB)")
    print(f"  FAT boot: start={FAT_START}, sectors={FAT_SECTORS}")
    print(f"  HBFS:     start={HBFS_START}, sectors={hbfs_sectors}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
