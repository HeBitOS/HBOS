#!/usr/bin/env python3
import argparse
import os
import struct
import subprocess
import uuid
import zlib

SECTOR_SIZE = 512
IMAGE_MIB = 128
FAT_START = 2048
FAT_SECTORS = 131072  # 64 MiB
HBFS_START = FAT_START + FAT_SECTORS
HBFS_PARTITION_TYPE = 0xEB
EFI_SYSTEM_TYPE = 0xEF
FAT32_LBA_TYPE = 0x0C
PROTECTIVE_MBR_TYPE = 0xEE

GPT_ENTRY_COUNT = 128
GPT_ENTRY_SIZE = 128
GPT_ENTRY_SECTORS = (GPT_ENTRY_COUNT * GPT_ENTRY_SIZE) // SECTOR_SIZE
GPT_FIRST_USABLE = 2 + GPT_ENTRY_SECTORS
GPT_BACKUP_SECTORS = 1 + GPT_ENTRY_SECTORS
GPT_ESP_TYPE = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
GPT_HBFS_TYPE = uuid.UUID("48424653-0000-4000-8000-48424f530001")

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


def utf16_name(name: str) -> bytes:
    raw = name.encode("utf-16le")[:72]
    return raw + bytes(72 - len(raw))


def gpt_entry(ptype: uuid.UUID, first: int, last: int, name: str) -> bytes:
    entry = bytearray(GPT_ENTRY_SIZE)
    entry[0:16] = ptype.bytes_le
    entry[16:32] = uuid.uuid4().bytes_le
    struct.pack_into("<QQQ", entry, 32, first, last, 0)
    entry[56:128] = utf16_name(name)
    return bytes(entry)


def gpt_header(current_lba: int, backup_lba: int, first_usable: int, last_usable: int,
               disk_guid: uuid.UUID, entries_lba: int, entries_crc: int) -> bytes:
    header = bytearray(SECTOR_SIZE)
    struct.pack_into(
        "<8sIIIIQQQQ16sQIII",
        header,
        0,
        b"EFI PART",
        0x00010000,
        92,
        0,
        0,
        current_lba,
        backup_lba,
        first_usable,
        last_usable,
        disk_guid.bytes_le,
        entries_lba,
        GPT_ENTRY_COUNT,
        GPT_ENTRY_SIZE,
        entries_crc,
    )
    crc = zlib.crc32(header[:92]) & 0xFFFFFFFF
    struct.pack_into("<I", header, 16, crc)
    return bytes(header)


def write_gpt(image: str, total_sectors: int, hbfs_sectors: int) -> None:
    last_lba = total_sectors - 1
    last_usable = total_sectors - GPT_BACKUP_SECTORS - 1
    fat_last = FAT_START + FAT_SECTORS - 1
    hbfs_last = HBFS_START + hbfs_sectors - 1
    if FAT_START < GPT_FIRST_USABLE or hbfs_last > last_usable:
        raise SystemExit("image layout does not fit GPT usable range")

    entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
    entries[0:GPT_ENTRY_SIZE] = gpt_entry(GPT_ESP_TYPE, FAT_START, fat_last, "HBOSBOOT")
    entries[GPT_ENTRY_SIZE:GPT_ENTRY_SIZE * 2] = gpt_entry(GPT_HBFS_TYPE, HBFS_START, hbfs_last, "HBFS")
    entries_crc = zlib.crc32(entries) & 0xFFFFFFFF
    disk_guid = uuid.uuid4()

    primary = gpt_header(1, last_lba, GPT_FIRST_USABLE, last_usable, disk_guid, 2, entries_crc)
    backup_entries_lba = total_sectors - GPT_BACKUP_SECTORS
    backup = gpt_header(last_lba, 1, GPT_FIRST_USABLE, last_usable, disk_guid, backup_entries_lba, entries_crc)

    with open(image, "r+b") as f:
        mbr = bytearray(SECTOR_SIZE)
        put_partition(mbr, 0, PROTECTIVE_MBR_TYPE, 1, min(total_sectors - 1, 0xFFFFFFFF), False)
        mbr[510:512] = b"\x55\xaa"
        f.seek(0)
        f.write(mbr)
        f.seek(SECTOR_SIZE)
        f.write(primary)
        f.seek(2 * SECTOR_SIZE)
        f.write(entries)
        f.seek(backup_entries_lba * SECTOR_SIZE)
        f.write(entries)
        f.seek(last_lba * SECTOR_SIZE)
        f.write(backup)


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
    parser.add_argument("--mode", choices=["bios", "uefi"], default="bios")
    args = parser.parse_args()

    size = args.size_mib * 1024 * 1024
    total_sectors = size // SECTOR_SIZE
    hbfs_sectors = total_sectors - HBFS_START
    if args.mode == "uefi":
        hbfs_sectors -= GPT_BACKUP_SECTORS
    if hbfs_sectors <= 1 + HBFS_TABLE_SECTORS + MAX_FILES * HBFS_FILE_SECTORS:
        raise SystemExit("image too small for FAT boot partition + HBFS")

    with open(args.image, "wb") as f:
        f.truncate(size)

    if args.mode == "uefi":
        write_gpt(args.image, total_sectors, hbfs_sectors)
    else:
        with open(args.image, "r+b") as f:
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
    run(["mcopy", "-i", fat_img, args.limine_conf, "::/EFI/BOOT/limine.conf"])
    run(["mcopy", "-i", fat_img, args.limine_conf, "::/boot/limine.conf"])
    run(["mcopy", "-i", fat_img, args.kernel, "::/boot/hbos.bin"])

    write_hbfs(args.image, HBFS_START, hbfs_sectors)
    if args.mode == "bios":
        run([os.path.join(args.limine_dir, "limine"), "bios-install", args.image])

    print(f"HBOS {args.mode.upper()} boot disk: {args.image} ({args.size_mib} MiB)")
    print(f"  FAT boot: start={FAT_START}, sectors={FAT_SECTORS}")
    print(f"  HBFS:     start={HBFS_START}, sectors={hbfs_sectors}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
