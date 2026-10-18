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

static int mem_wr(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx;
    if (lba >= IMG_SECS) return -1;
    memcpy(g_img + (size_t)lba * SEC, buf, SEC);
    return 0;
}

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

static int file_wr(void *ctx, uint64_t lba, const void *buf) {
    FILE *f = ((file_ctx_t *)ctx)->f;
    if (fseek(f, (long)(lba * 512), SEEK_SET) != 0) return -1;
    return fwrite(buf, 1, 512, f) == 512 ? 0 : -1;
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
    put32(fat + 20 * 4, 0xFFFFFFFFu);   /* 子目录 */
    put32(fat + 21 * 4, 0xFFFFFFFFu);   /* 子目录里的文件 */

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

    /* 子目录项集合：0x85 + 0xC0 + 0xC1，属性 0x10 */
    uint8_t *de = n + 32;
    de[0] = 0x85; de[1] = 2; de[4] = 0x10;
    put32(de + 20, 20);
    put64(de + 24, 0);
    uint8_t *ds = de + 32;
    ds[0] = 0xC0; ds[1] = 0; ds[3] = 3;
    put32(ds + 20, 20);
    put64(ds + 24, 0);
    uint8_t *dn = ds + 32;
    dn[0] = 0xC1;
    for (int i = 0; i < 3; i++) put16(dn + 2 + i * 2, (uint16_t)"SUB"[i]);

    /* 第二个簇标记结束，防止越界扫描 */
    g_img[(HEAP_LBA + 1) * SEC] = 0x00;

    uint8_t *data = g_img + (HEAP_LBA + 8) * SEC;   /* 簇 10 = 堆 + 8 */
    memcpy(data, "hello exfat\n", FILESIZE);

    /* 子目录（簇 20）里放一个 IN.TXT */
    uint8_t *sub = g_img + (HEAP_LBA + 18) * SEC;   /* 簇 20 = 堆 + 18 */
    uint8_t *fe = sub;
    fe[0] = 0x85; fe[1] = 2; fe[4] = 0x20;
    put32(fe + 20, 21);
    put64(fe + 24, 5);
    uint8_t *fs2 = fe + 32;
    fs2[0] = 0xC0; fs2[1] = 0; fs2[3] = 6;
    put32(fs2 + 20, 21);
    put64(fs2 + 24, 5);
    uint8_t *fn = fs2 + 32;
    fn[0] = 0xC1;
    for (int i = 0; i < 6; i++) put16(fn + 2 + i * 2, (uint16_t)"IN.TXT"[i]);
    uint8_t *subdata = g_img + (HEAP_LBA + 19) * SEC;  /* 簇 21 */
    memcpy(subdata, "sub!\n", 5);

    /* 分配位图（0x81）：簇 30，长度 8 字节，覆盖 64 个簇。
     * 已用：2,3（根目录）、10（HI.TXT）、20（SUB）、21（IN.TXT）、30（位图） */
    uint8_t *bmde = root + 224;
    bmde[0] = 0x81;
    put32(bmde + 20, 30);
    put64(bmde + 24, 8);
    uint8_t *bm = g_img + (HEAP_LBA + 28) * SEC;       /* 簇 30 */
    int used[] = {2, 3, 10, 20, 21, 30};
    for (unsigned i = 0; i < sizeof(used) / sizeof(used[0]); i++) {
        int bit = used[i] - 2;
        bm[bit / 8] |= (uint8_t)(1u << (bit % 8));
    }
}

int main(int argc, char **argv) {
    exfat_fs_t fs;
    exfat_dirent_t list[8];
    char label[32];

    build_volume();
    printf("[EXFAT] 挂载合成卷\n");
    CHECK(exfat_mount(&fs, mem_rd, NULL, 0) == 0);
    fs.wr = mem_wr;
    CHECK(fs.bytes_per_sector == 512);
    CHECK(fs.root_cluster == 2);

    exfat_volume_label(&fs, label, sizeof(label));
    CHECK(strcmp(label, "TESTVOL") == 0);

    printf("[EXFAT] 列根目录\n");
    int n = exfat_list_root(&fs, list, 8);
    CHECK(n == 2);
    CHECK(strcmp(list[0].name, "HI.TXT") == 0);
    CHECK(list[0].size == FILESIZE);
    CHECK(list[0].first_cluster == 10);
    CHECK((list[0].attr & 0x10) == 0);
    CHECK(strcmp(list[1].name, "SUB") == 0);
    CHECK((list[1].attr & 0x10) != 0);
    CHECK(list[1].first_cluster == 20);

    printf("[EXFAT] 读文件\n");
    char buf[32] = {0};
    CHECK(exfat_read_file(&fs, 10, FILESIZE, 0, buf, FILESIZE) == FILESIZE);
    CHECK(memcmp(buf, "hello exfat\n", FILESIZE) == 0);
    memset(buf, 0, sizeof(buf));
    CHECK(exfat_read_file(&fs, 10, FILESIZE, 6, buf, 5) == 5);
    CHECK(memcmp(buf, "exfat", 5) == 0);
    CHECK(exfat_read_file(&fs, 10, FILESIZE, FILESIZE, buf, 4) == 0);

    printf("[EXFAT] 子目录\n");
    exfat_dirent_t sublist[4];
    int sn = exfat_list_dir(&fs, 20, sublist, 4);
    CHECK(sn == 1);
    CHECK(strcmp(sublist[0].name, "IN.TXT") == 0);
    CHECK(sublist[0].size == 5);
    memset(buf, 0, sizeof(buf));
    CHECK(exfat_read_file(&fs, 21, 5, 0, buf, 5) == 5);
    CHECK(memcmp(buf, "sub!\n", 5) == 0);

    printf("[EXFAT] 在子目录里创建\n");
    CHECK(exfat_create_in_dir(&fs, 20, "IN2.TXT", "abc\n", 4) == 0);
    exfat_dirent_t sub2[8];
    int sn2 = exfat_list_dir(&fs, 20, sub2, 8);
    CHECK(sn2 == 2);
    int f2 = -1;
    for (int i = 0; i < sn2; i++)
        if (strcmp(sub2[i].name, "IN2.TXT") == 0) f2 = i;
    CHECK(f2 >= 0);
    if (f2 >= 0) {
        memset(buf, 0, sizeof(buf));
        CHECK(exfat_read_file(&fs, sub2[f2].first_cluster, sub2[f2].size,
                              0, buf, 4) == 4);
        CHECK(memcmp(buf, "abc\n", 4) == 0);
    }

    printf("[EXFAT] 非 exFAT 镜像被拒绝\n");
    memset(g_img, 0, sizeof(g_img));
    CHECK(exfat_mount(&fs, mem_rd, NULL, 0) != 0);

    if (argc > 1) {
        printf("[EXFAT] 真实镜像：%s\n", argv[1]);
        FILE *f = fopen(argv[1], "r+b");
        if (!f) {
            printf("  [SKIP] 打不开\n");
        } else {
            file_ctx_t fc = { f };
            exfat_fs_t rfs;
            CHECK(exfat_mount(&rfs, file_rd, &fc, 0) == 0);
            rfs.wr = file_wr;

            printf("[EXFAT] 创建文件\n");
            const char *payload = "written by hbos\n";
            int pr = exfat_create_file(&rfs, "HBOS.TXT", payload, 16);
            if (pr != 0) {
                printf("  [FAIL] 创建失败（可能没有分配位图/空间不足）\n");
                failures++;
            } else {
                /* 重新挂载再读，确保不是读到了内存里的残留 */
                exfat_fs_t rf2;
                CHECK(exfat_mount(&rf2, file_rd, &fc, 0) == 0);
                exfat_dirent_t rl[32];
                int rn = exfat_list_root(&rf2, rl, 32);
                int found = -1;
                for (int i = 0; i < rn; i++)
                    if (strcmp(rl[i].name, "HBOS.TXT") == 0) found = i;
                CHECK(found >= 0);
                if (found >= 0) {
                    char rb[64] = {0};
                    CHECK(exfat_read_file(&rf2, rl[found].first_cluster,
                                          rl[found].size, 0, rb, 16) == 16);
                    CHECK(memcmp(rb, payload, 16) == 0);
                    printf("  [ok] 写入后读回一致（%u 字节）\n",
                           (unsigned)rl[found].size);
                }

                /* 非 ASCII 名字：验证 upcase 表解析与 UTF-16 名字项 */
                printf("[EXFAT] 中文文件名\n");
                CHECK(exfat_create_file(&rfs, "中文.TXT", "nihao\n", 6) == 0);
                exfat_fs_t rfu;
                CHECK(exfat_mount(&rfu, file_rd, &fc, 0) == 0);
                exfat_dirent_t rlu[32];
                int rnu = exfat_list_root(&rfu, rlu, 32);
                int fu = -1;
                for (int i = 0; i < rnu; i++)
                    if (strcmp(rlu[i].name, "中文.TXT") == 0) fu = i;
                CHECK(fu >= 0);
                if (fu >= 0) {
                    char ru[16] = {0};
                    CHECK(exfat_read_file(&rfu, rlu[fu].first_cluster,
                                          rlu[fu].size, 0, ru, 6) == 6);
                    CHECK(memcmp(ru, "nihao\n", 6) == 0);
                    printf("  [ok] 中文名读回：%s\n", rlu[fu].name);
                }

                /* 追加 + 覆盖：验证簇扩展与目录项大小回写 */
                printf("[EXFAT] 追加/覆盖写入\n");
                CHECK(exfat_write_file(&rfs, "HBOS.TXT", 16, "APPENDED\n", 9) == 0);
                CHECK(exfat_write_file(&rfs, "HBOS.TXT", 0, "HBOS", 4) == 0);

                exfat_fs_t rf3;
                CHECK(exfat_mount(&rf3, file_rd, &fc, 0) == 0);
                exfat_dirent_t rl3[32];
                int rn3 = exfat_list_root(&rf3, rl3, 32);
                int f3 = -1;
                for (int i = 0; i < rn3; i++)
                    if (strcmp(rl3[i].name, "HBOS.TXT") == 0) f3 = i;
                CHECK(f3 >= 0);
                if (f3 >= 0) {
                    char rb[64] = {0};
                    CHECK(rl3[f3].size == 25);
                    CHECK(exfat_read_file(&rf3, rl3[f3].first_cluster,
                                          rl3[f3].size, 0, rb, 25) == 25);
                    CHECK(memcmp(rb, "HBOS", 4) == 0);
                    CHECK(memcmp(rb + 16, "APPENDED\n", 9) == 0);
                    printf("  [ok] 追加后大小 %u，内容一致\n",
                           (unsigned)rl3[f3].size);
                }
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
