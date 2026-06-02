/**
 * @file    sys/mman.h
 * @brief   Linux 内存映射头文件 — HBOS 实现
 *
 * 对标 Linux sys/mman.h，定义 mmap/munmap/mprotect 的
 * 保护标志（prot）和映射标志（flags）。
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