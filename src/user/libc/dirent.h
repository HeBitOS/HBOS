/**
 * @file    dirent.h
 * @brief   目录遍历（用户态 libc）—— 对标 POSIX dirent.h
 *
 * 与内核 sys/dirent.h 的 struct dirent 布局**逐字节一致**：内核 getdents
 * 系统调用按固定 sizeof(struct dirent) 步长写入记录，因此 readdir 也必须
 * 按同一固定步长前进（而非按 d_reclen）。
 */
#ifndef HBOS_USER_LIBC_DIRENT_H
#define HBOS_USER_LIBC_DIRENT_H

#include <stdint.h>
#include <stddef.h>

#define NAME_MAX 255

/** 目录项类型（d_type 取值，与内核一致） */
enum {
    DT_UNKNOWN = 0,
    DT_CHR     = 2,
    DT_DIR     = 4,
    DT_BLK     = 6,
    DT_REG     = 8,
    DT_LNK     = 10,
};

/** POSIX 目录项结构（布局须与内核 struct dirent 完全一致） */
struct dirent {
    uint64_t d_ino;                /**< inode 编号 */
    int64_t  d_off;                /**< 目录偏移（条目序号） */
    uint16_t d_reclen;            /**< 本条记录声明长度（仅参考，勿用于步进） */
    uint8_t  d_type;              /**< 目录项类型，DT_* 之一 */
    char     d_name[NAME_MAX + 1]; /**< 文件名（NUL 结尾） */
};

/** 目录流句柄（不透明） */
typedef struct DIR DIR;

#ifdef __cplusplus
extern "C" {
#endif

/** 打开目录，返回目录流；失败返回 NULL */
DIR *opendir(const char *path);

/** 读取下一个目录项，返回指向内部缓冲的指针；无更多项或出错返回 NULL */
struct dirent *readdir(DIR *d);

/** 关闭目录流，释放资源；成功返回 0，失败返回 -1 */
int closedir(DIR *d);

/** 底层接口：从已打开的目录 fd 一次性读取目录项，返回写入字节数 */
int getdents(int fd, struct dirent *dirp, unsigned int count);

#ifdef __cplusplus
}
#endif

#endif /* HBOS_USER_LIBC_DIRENT_H */
