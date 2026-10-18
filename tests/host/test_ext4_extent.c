/* ext4 extent 映射单测：depth 0/1、空洞、unwritten、坏 magic。 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "lib/ext4_extent.h"

#define BS 1024
static uint8_t g_blocks[8][BS];
static int failures;
#define CHECK(e) do { if (!(e)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #e); failures++; } } while (0)

static int rd(void *ctx, uint32_t block, void *buf) {
    (void)ctx;
    if (block >= 8) return -1;
    memcpy(buf, g_blocks[block], BS);
    return 0;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void hdr(uint8_t *p, uint16_t entries, uint16_t max, uint16_t depth) {
    put16(p + 0, 0xF30A);
    put16(p + 2, entries);
    put16(p + 4, max);
    put16(p + 6, depth);
    put32(p + 8, 0);
}

static void extent(uint8_t *p, uint32_t logical, uint16_t len, uint64_t start) {
    put32(p + 0, logical);
    put16(p + 4, len);
    put16(p + 6, (uint16_t)(start >> 32));
    put32(p + 8, (uint32_t)start);
}

static void index_entry(uint8_t *p, uint32_t logical, uint64_t leaf) {
    put32(p + 0, logical);
    put32(p + 4, (uint32_t)leaf);
    put16(p + 8, (uint16_t)(leaf >> 32));
    put16(p + 10, 0);
}

int main(void) {
    uint8_t inode[60];
    memset(g_blocks, 0, sizeof(g_blocks));

    printf("[EXT4] depth 0 + 空洞\n");
    memset(inode, 0, sizeof(inode));
    hdr(inode, 2, 4, 0);
    extent(inode + 12, 0, 3, 100);
    extent(inode + 24, 3, 2, 200);
    CHECK(ext4_extent_map_raw(rd, NULL, inode, BS, 0) == 100);
    CHECK(ext4_extent_map_raw(rd, NULL, inode, BS, 2) == 102);
    CHECK(ext4_extent_map_raw(rd, NULL, inode, BS, 3) == 200);
    CHECK(ext4_extent_map_raw(rd, NULL, inode, BS, 4) == 201);
    CHECK(ext4_extent_map_raw(rd, NULL, inode, BS, 5) == 0);   /* 空洞 */

    printf("[EXT4] unwritten 视为空洞\n");
    memset(inode, 0, sizeof(inode));
    hdr(inode, 1, 4, 0);
    extent(inode + 12, 0, (uint16_t)(0x8000 | 2), 300);
    CHECK(ext4_extent_map_raw(rd, NULL, inode, BS, 0) == 0);
    CHECK(ext4_extent_map_raw(rd, NULL, inode, BS, 1) == 0);

    printf("[EXT4] depth 1 索引\n");
    memset(inode, 0, sizeof(inode));
    hdr(inode, 1, 4, 1);
    index_entry(inode + 12, 0, 5);          /* 叶子在块 5 */
    hdr(g_blocks[5], 2, 4, 0);
    extent(g_blocks[5] + 12, 0, 4, 1000);
    extent(g_blocks[5] + 24, 4, 4, 2000);
    CHECK(ext4_extent_map_raw(rd, NULL, inode, BS, 0) == 1000);
    CHECK(ext4_extent_map_raw(rd, NULL, inode, BS, 3) == 1003);
    CHECK(ext4_extent_map_raw(rd, NULL, inode, BS, 4) == 2000);
    CHECK(ext4_extent_map_raw(rd, NULL, inode, BS, 7) == 2003);
    CHECK(ext4_extent_map_raw(rd, NULL, inode, BS, 8) == 0);

    printf("[EXT4] 坏 magic / 越界\n");
    memset(inode, 0, sizeof(inode));
    put16(inode, 0x1234);
    CHECK(ext4_extent_map_raw(rd, NULL, inode, BS, 0) == 0);

    memset(inode, 0, sizeof(inode));
    hdr(inode, 1, 4, 1);
    index_entry(inode + 12, 0, 0x100000000ull);   /* 48 位块号，超出支持 */
    CHECK(ext4_extent_map_raw(rd, NULL, inode, BS, 0) == 0);

    if (failures) { printf("[EXT4] FAILED: %d\n", failures); return 1; }
    printf("[EXT4] all tests passed\n");
    return 0;
}
