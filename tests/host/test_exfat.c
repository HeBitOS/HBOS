/* exFAT 只读驱动的宿主机单测。
 *
 * 两个来源：
 *   1) 手工构造的最小 exFAT 卷（引导扇区 + FAT + 根目录 + 一个文件），
 *      用来验证目录项状态机与簇链读取；
 *   2) 若命令行给了镜像路径（CI/本地可用 mkfs.exfat 生成），额外验证
 *      真实工具产出的引导扇区能被挂载。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "exfat.h"

#define SEC 512
#define IMG_SECS 256
static uint8_t g_img[IMG_SECS * SEC];
static int failures;
#define CHECK(e) do { if (!(e)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #e); failures++; } } while (0)

static int mem_rd(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba >= IMG_SECS) return -1;
    memcpy(buf, g_img + (size_t)lba * SEC, SEC);
    return 0;
}

typedef struct { FILE *f; } file_ctx_t;

static int file_rd(void *ctx, uint64_t lba, void *buf) {
    FILE *f = ((file_ctx_t *)ctx)->f;
    if (fseek(f, (long)(lba * 512), SEEK_SET) != 0) return -1;
    return fread(buf, 1, 512, f) == 512 ? 0 : -1;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* 布局：FAT@32（8 扇区），堆@40，簇 2 = 根目录，簇 10 = 文件 */
#define FAT_LBA   32
#define HEAP_LBA  40
#define FILESIZE  12

static void build_volume(void) {
    memset(g_img, 0, sizeof(g_img));
    uint8_t *bs = g_img;
    memcpy(bs + 3, "EXFAT   ", 8);
    put64(bs + 72, IMG_SECS);
    put32(bs + 80, FAT_LBA);
    put32(bs + 84, 8);
    put32(bs + 88, HEAP_LBA);
    put32(bs + 92, 64);
    put32(bs + 96, 2);
    bs[108] = 9;    /* 512 B */
    bs[109] = 0;    /* 1 扇区/簇 */
    bs[110] = 1;

    uint8_t *fat = g_img + FAT_LBA * SEC;
    put32(fat + 2 * 4, 3);            /* 根目录跨两个簇 */
    put32(fat + 3 * 4, 0xFFFFFFFFu);
    put32(fat + 10 * 4, 0xFFFFFFFFu);

    uint8_t *root = g_img + HEAP_LBA * SEC;
    /* 卷标 */
    root[0] = 0x83;
    root[1] = 7;
    const char *label = "TESTVOL";
    for (int i = 0; i < 7; i++) put16(root + 2 + i * 2, (uint16_t)label[i]);

    /* 文件项集合：0x85 + 0xC0 + 0xC1 */
    uint8_t *e = root + 32;
    e[0] = 0x85; e[1] = 2; e[4] = 0x20;
    put32(e + 20, 10);
    put64(e + 24, FILESIZE);

    uint8_t *s = e + 32;
    s[0] = 0xC0; s[1] = 0; s[3] = 6;
    put32(s + 20, 10);
    put64(s + 24, FILESIZE);

    uint8_t *n = s + 32;
    n[0] = 0xC1;
    const char *name = "HI.TXT";
    for (int i = 0; i < 6; i++) put16(n + 2 + i * 2, (uint16_t)name[i]);

    /* 第二个簇标记结束，防止越界扫描 */
    g_img[(HEAP_LBA + 1) * SEC] = 0x00;

    uint8_t *data = g_img + (HEAP_LBA + 8) * SEC;   /* 簇 10 = 堆 + 8 */
    memcpy(data, "hello exfat\n", FILESIZE);
}

int main(int argc, char **argv) {
    exfat_fs_t fs;
    exfat_dirent_t list[8];
    char label[32];

    build_volume();
    printf("[EXFAT] 挂载合成卷\n");
    CHECK(exfat_mount(&fs, mem_rd, NULL, 0) == 0);
    CHECK(fs.bytes_per_sector == 512);
    CHECK(fs.root_cluster == 2);

    exfat_volume_label(&fs, label, sizeof(label));
    CHECK(strcmp(label, "TESTVOL") == 0);

    printf("[EXFAT] 列根目录\n");
    int n = exfat_list_root(&fs, list, 8);
    CHECK(n == 1);
    CHECK(strcmp(list[0].name, "HI.TXT") == 0);
    CHECK(list[0].size == FILESIZE);
    CHECK(list[0].first_cluster == 10);
    CHECK((list[0].attr & 0x10) == 0);

    printf("[EXFAT] 读文件\n");
    char buf[32] = {0};
    CHECK(exfat_read_file(&fs, 10, FILESIZE, 0, buf, FILESIZE) == FILESIZE);
    CHECK(memcmp(buf, "hello exfat\n", FILESIZE) == 0);
    memset(buf, 0, sizeof(buf));
    CHECK(exfat_read_file(&fs, 10, FILESIZE, 6, buf, 5) == 5);
    CHECK(memcmp(buf, "exfat", 5) == 0);
    CHECK(exfat_read_file(&fs, 10, FILESIZE, FILESIZE, buf, 4) == 0);

    printf("[EXFAT] 非 exFAT 镜像被拒绝\n");
    memset(g_img, 0, sizeof(g_img));
    CHECK(exfat_mount(&fs, mem_rd, NULL, 0) != 0);

    if (argc > 1) {
        printf("[EXFAT] 真实镜像：%s\n", argv[1]);
        FILE *f = fopen(argv[1], "rb");
        if (!f) {
            printf("  [SKIP] 打不开\n");
        } else {
            file_ctx_t fc = { f };
            exfat_fs_t rfs;
            if (exfat_mount(&rfs, file_rd, &fc, 0) == 0) {
                exfat_dirent_t rl[16];
                int rn = exfat_list_root(&rfs, rl, 16);
                printf("  [ok] 挂载成功，根目录 %d 项\n", rn);
                CHECK(rn >= 0);
            } else {
                printf("  [FAIL] 真实镜像挂载失败\n");
                failures++;
            }
            fclose(f);
        }
    }

    if (failures) {
        printf("[EXFAT] FAILED: %d\n", failures);
        return 1;
    }
    printf("[EXFAT] all tests passed\n");
    return 0;
}
