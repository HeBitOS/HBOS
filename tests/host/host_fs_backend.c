/* tests/host/host_fs_backend.c — 文件版块设备，替身 src/block.c。
 *
 * 只实现被测文件系统真正用到的那几个入口，其余内核依赖（rtc_read_time）
 * 也给个固定返回，保证同一输入下行为完全确定。
 * 同时统计「底层命令数 / 传输扇区数」，用来量化 I/O 合并的收益。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "hbos_shim.h"

#include "../../src/block.h"

uint64_t hbostest_reads, hbostest_writes, hbostest_read_cmds, hbostest_write_cmds;

static FILE    *g_disk;
static uint64_t g_sectors;
static long     g_base_offset;   /* 分区起始字节偏移（模拟 partition_lba） */

void hbostest_open(const char *path, long base_lba) {
    g_disk = fopen(path, "r+b");
    if (!g_disk) { perror("open disk image"); exit(2); }
    fseek(g_disk, 0, SEEK_END);
    long sz = ftell(g_disk);
    g_sectors = (uint64_t)sz / BLOCK_SECTOR_SIZE;
    g_base_offset = base_lba * BLOCK_SECTOR_SIZE;
}

int block_init(void) { return 0; }
uint32_t block_sector_count(void) { return (uint32_t)(g_sectors - (uint64_t)(g_base_offset / BLOCK_SECTOR_SIZE)); }
block_backend_t block_backend(void) { return BLOCK_BACKEND_AHCI; }
const char *block_backend_name(void) { return "hostfile"; }

static int rw(uint32_t lba, uint8_t *buf, uint32_t count, int write) {
    if (!g_disk) return -1;
    long off = g_base_offset + (long)lba * BLOCK_SECTOR_SIZE;
    size_t bytes = (size_t)count * BLOCK_SECTOR_SIZE;
    int ok;
    if (write) {
        hbostest_write_cmds++;
        hbostest_writes += count;
        ok = (fseek(g_disk, off, SEEK_SET) == 0 && fwrite(buf, 1, bytes, g_disk) == bytes);
    } else {
        hbostest_read_cmds++;
        hbostest_reads += count;
        ok = (fseek(g_disk, off, SEEK_SET) == 0 && fread(buf, 1, bytes, g_disk) == bytes);
    }
    return ok ? 0 : -1;
}

int block_read_sector(uint32_t lba, uint8_t *buf) { return rw(lba, buf, 1, 0); }
int block_read_sectors(uint32_t lba, uint8_t *buf, uint32_t count) { return rw(lba, buf, count, 0); }
int block_write_sector(uint32_t lba, const uint8_t *buf) { return rw(lba, (uint8_t *)buf, 1, 1); }
int block_write_sectors(uint32_t lba, const uint8_t *buf, uint32_t count) { return rw(lba, (uint8_t *)buf, count, 1); }

/* 文件系统格式化路径会读 RTC；返回固定值，保证同一输入下 old/new 两个实现
 * 写出的镜像逐字节可比。 */
#include "../../src/rtc.h"
int rtc_read_time(rtc_time_t *time) {
    time->second = 5; time->minute = 4; time->hour = 3;
    time->day = 2;    time->month = 1;  time->year = 2026;
    time->century = 20; time->weekday = 6;
    return 0;
}
