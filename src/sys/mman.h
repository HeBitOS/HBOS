/**
 * @file    sys/mman.h
 * @brief   Linux 内存映射头文件 — HBOS 实现
 *
 * 对标 Linux sys/mman.h，定义 mmap/munmap/mprotect 的
 * 保护标志（prot）和映射标志（flags）。
 *
 * @layer   user
 * @owner   src/sys/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    PROT_NONE      不可访问
 *         PROT_READ      可读
 *         PROT_WRITE     可写
 *         PROT_EXEC      可执行
 *         MAP_SHARED     共享映射，对映射区的写入写回文件
 *         MAP_PRIVATE    私有映射（COW），对映射区的写入不写回文件
 *         MAP_FIXED      必须映射到 addr 指定的地址
 *         MAP_ANONYMOUS  匿名映射（不对应文件，fd 忽略）
 *         MAP_FAILED     映射FAILED
 *   函数  mmap        映射内存/文件到地址空间
 *         munmap      解除映射
 *         mprotect    改映射权限
 *         brk         调整数据段末尾
 */
#ifndef HBOS_SYS_MMAN_H
#define HBOS_SYS_MMAN_H

#include "types.h"

/** — mmap 保护标志 prot (对标 Linux) — */

#define PROT_NONE  0x0       /**< 不可访问 */
#define PROT_READ  0x1       /**< 可读 */
#define PROT_WRITE 0x2       /**< 可写 */
#define PROT_EXEC  0x4       /**< 可执行 */

/** — mmap 类型标志 flags (对标 Linux) — */

#define MAP_SHARED     0x01   /**< 共享映射，对映射区的写入写回文件 */
#define MAP_PRIVATE    0x02   /**< 私有映射（COW），对映射区的写入不写回文件 */
#define MAP_FIXED      0x10   /**< 必须映射到 addr 指定的地址 */
#define MAP_ANONYMOUS  0x20   /**< 匿名映射（不对应文件，fd 忽略） */

/** — mmap 失败返回值 — */
#define MAP_FAILED ((void *)(uintptr_t)-1)

/** — brk 相关 — */
#ifdef __cplusplus
extern "C" {
#endif

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
void *brk(void *addr);

#ifdef __cplusplus
}
#endif

#endif