/**
 * @file    partition.h
 * @brief   分区表解析（MBR / 扩展分区 / GPT）
 *
 * 与块设备解耦：解析函数只接受一个"按 LBA 读扇区"的回调，因此可以在宿主
 * 机上用内存镜像做单测（tests/host/test_partition.c），不需要启动 QEMU。
 *
 * 为什么不直接用 fs.c 里那份：那份是给"找 HBFS 分区"用的，只保留 4 项、
 * 不处理扩展分区、不校验 GPT CRC，也没有名字。这里做通用的分区视图，
 * 后续安装器（P4）和 exFAT/ext4 挂载（P3）都走这一份。
 */

#ifndef HBOS_PARTITION_H
#define HBOS_PARTITION_H

#include <stdint.h>

#define PART_MAX_ENTRIES   32
#define PART_NAME_MAX      37   /* GPT 名字 36 个 UTF-16 码元 + NUL */

enum {
    PART_SCHEME_NONE = 0,
    PART_SCHEME_MBR  = 1,
    PART_SCHEME_GPT  = 2,
};

typedef struct {
    uint8_t  present;
    uint8_t  bootable;
    uint8_t  scheme;        /* PART_SCHEME_* */
    uint8_t  type;          /* MBR 类型码；GPT 映射后的等价类型码（0xEF=ESP） */
    uint8_t  guid[16];      /* GPT 分区类型 GUID（MBR 全 0） */
    uint64_t start_lba;
    uint64_t sectors;
    char     name[PART_NAME_MAX];  /* GPT 名字；MBR 为空串 */
} partition_entry_t;

/** 读一个 512B 扇区，成功返回 0。ctx 由调用方定义。 */
typedef int (*partition_read_fn)(void *ctx, uint64_t lba, void *buf);

/**
 * @brief 解析分区表。
 * @param out 输出数组，至少 max 项；返回前会清零。
 * @return 分区数量（0 表示无分区表/空表），<0 表示读盘或表损坏。
 */
int partition_parse(partition_read_fn rd, void *ctx,
                    partition_entry_t *out, int max);

/** 表类型名："none" / "MBR" / "GPT"。 */
const char *partition_scheme_name(uint8_t scheme);

/** 分区类型的人类可读名（按 MBR 类型码；GPT 已映射到等价类型码）。 */
const char *partition_type_name(uint8_t type);

#endif /* HBOS_PARTITION_H */
