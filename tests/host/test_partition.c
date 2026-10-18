/* 分区表解析的宿主机单测：MBR / 扩展分区(EBR) / GPT。
 *
 * 镜像在内存里手工构造，读盘回调只做 memcpy，因此不需要 QEMU、不需要
 * 真实磁盘，也不依赖 block 层。GPT 的头部与分区项 CRC 按规范计算，
 * 负例（CRC 被改坏）必须退回 MBR 解释而不是崩掉。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../src/partition.h"

#define SEC 512
#define IMG_LBAS 24576
static uint8_t g_img[IMG_LBAS * SEC];

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static int mem_rd(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba >= IMG_LBAS) return -1;
    memcpy(buf, g_img + (size_t)lba * SEC, SEC);
    return 0;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static uint32_t crc32(const uint8_t *d, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

static void mbr_entry(uint8_t *mbr, int i, uint8_t boot, uint8_t type,
                      uint32_t start, uint32_t count) {
    uint8_t *e = mbr + 446 + i * 16;
    memset(e, 0, 16);
    e[0] = boot ? 0x80 : 0x00;
    e[4] = type;
    put32(e + 8, start);
    put32(e + 12, count);
}

static void gpt_entry(uint8_t *ent_base, int i, const uint8_t guid[16],
                      uint64_t first, uint64_t last, const char *name) {
    uint8_t *e = ent_base + (size_t)i * 128;
    memcpy(e, guid, 16);
    put64(e + 32, first);
    put64(e + 40, last);
    for (int k = 0; k < 36; k++) {
        uint16_t c = (uint16_t)(name && name[k] ? (unsigned char)name[k] : 0);
        put16(e + 56 + k * 2, c);
    }
}

static const uint8_t GUID_ESP[16] = {0xC1,0x2A,0x73,0x28,0xF8,0x1F,0x11,0xD2,
                                     0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};
static const uint8_t GUID_LINUX[16] = {0x0F,0xC6,0x3D,0xAF,0x84,0x83,0x47,0x72,
                                       0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4};
static const uint8_t GUID_MS[16] = {0xEB,0xD0,0xA0,0xA2,0xB9,0xE5,0x44,0x33,
                                    0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7};

static void build_mbr(void) {
    memset(g_img, 0, sizeof(g_img));
    uint8_t *m = g_img;
    mbr_entry(m, 0, 1, 0x83, 2048, 4096);     /* bootable Linux */
    mbr_entry(m, 1, 0, 0x0C, 6144, 8192);     /* FAT32 LBA */
    m[510] = 0x55; m[511] = 0xAA;
}

static void build_extended(void) {
    memset(g_img, 0, sizeof(g_img));
    uint8_t *m = g_img;
    mbr_entry(m, 0, 0, 0x05, 10000, 20000);   /* 扩展分区容器 */
    m[510] = 0x55; m[511] = 0xAA;

    uint8_t *ebr0 = g_img + (size_t)10000 * SEC;
    mbr_entry(ebr0, 0, 0, 0x83, 2048, 4096);  /* 逻辑分区 1 */
    mbr_entry(ebr0, 1, 0, 0x05, 8192, 4096);  /* 下一个 EBR（相对扩展区起点） */
    ebr0[510] = 0x55; ebr0[511] = 0xAA;

    uint8_t *ebr1 = g_img + (size_t)(10000 + 8192) * SEC;
    mbr_entry(ebr1, 0, 0, 0x83, 2048, 4096);  /* 逻辑分区 2 */
    ebr1[510] = 0x55; ebr1[511] = 0xAA;
}

static void build_gpt(int corrupt_crc) {
    memset(g_img, 0, sizeof(g_img));
    uint8_t *m = g_img;
    mbr_entry(m, 0, 0, 0xEE, 1, 0xFFFFFFFFu); /* 保护性 MBR */
    m[510] = 0x55; m[511] = 0xAA;

    uint8_t *h = g_img + SEC;
    memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x00010000u);
    put32(h + 12, 92);
    put32(h + 16, 0);                         /* CRC 占位 */
    put64(h + 24, 1);                         /* current LBA */
    put64(h + 32, IMG_LBAS - 1);              /* backup LBA */
    put64(h + 40, 34);                        /* first usable */
    put64(h + 48, IMG_LBAS - 34);
    put64(h + 72, 2);                         /* entries LBA */
    put32(h + 80, 128);
    put32(h + 84, 128);

    uint8_t *ents = g_img + 2 * SEC;
    gpt_entry(ents, 0, GUID_ESP, 2048, 4095, "EFI");
    gpt_entry(ents, 1, GUID_LINUX, 4096, 12287, "root");
    gpt_entry(ents, 2, GUID_MS, 12288, 20479, "data");

    put32(h + 88, crc32(ents, 128 * 128));
    put32(h + 16, crc32(h, 92));
    if (corrupt_crc) put32(h + 16, crc32(h, 92) ^ 1u);
}

int main(void) {
    partition_entry_t p[PART_MAX_ENTRIES];
    int n;

    printf("[PART] 无分区表\n");
    memset(g_img, 0, sizeof(g_img));
    n = partition_parse(mem_rd, NULL, p, PART_MAX_ENTRIES);
    CHECK(n == 0);

    printf("[PART] MBR 主分区\n");
    build_mbr();
    n = partition_parse(mem_rd, NULL, p, PART_MAX_ENTRIES);
    CHECK(n == 2);
    CHECK(p[0].scheme == PART_SCHEME_MBR);
    CHECK(p[0].bootable == 1);
    CHECK(p[0].type == 0x83);
    CHECK(p[0].start_lba == 2048 && p[0].sectors == 4096);
    CHECK(p[1].bootable == 0 && p[1].type == 0x0C);
    CHECK(p[1].start_lba == 6144 && p[1].sectors == 8192);
    CHECK(strcmp(partition_type_name(p[0].type), "Linux") == 0);
    CHECK(strcmp(partition_scheme_name(p[0].scheme), "MBR") == 0);

    printf("[PART] 扩展分区 EBR 链\n");
    build_extended();
    n = partition_parse(mem_rd, NULL, p, PART_MAX_ENTRIES);
    CHECK(n == 2);                                  /* 容器本身不计入 */
    CHECK(p[0].start_lba == 10000 + 2048 && p[0].sectors == 4096);
    CHECK(p[1].start_lba == 10000 + 8192 + 2048 && p[1].sectors == 4096);

    printf("[PART] GPT\n");
    build_gpt(0);
    n = partition_parse(mem_rd, NULL, p, PART_MAX_ENTRIES);
    CHECK(n == 3);
    CHECK(p[0].scheme == PART_SCHEME_GPT);
    CHECK(p[0].type == 0xEF && p[0].bootable == 1);
    CHECK(strcmp(p[0].name, "EFI") == 0);
    CHECK(p[1].type == 0x83 && p[1].start_lba == 4096 &&
          p[1].sectors == 12287 - 4096 + 1);
    CHECK(strcmp(p[1].name, "root") == 0);
    CHECK(p[2].type == 0x07 && strcmp(p[2].name, "data") == 0);
    CHECK(memcmp(p[1].guid, GUID_LINUX, 16) == 0);
    CHECK(strcmp(partition_type_name(p[2].type), "NTFS/exFAT") == 0);

    printf("[PART] GPT CRC 损坏 → 退回保护性 MBR\n");
    build_gpt(1);
    n = partition_parse(mem_rd, NULL, p, PART_MAX_ENTRIES);
    CHECK(n == 1);
    CHECK(p[0].type == 0xEE);

    if (failures) {
        printf("[PART] FAILED: %d 处断言失败\n", failures);
        return 1;
    }
    printf("[PART] all tests passed\n");
    return 0;
}
