/**
 * @file    tmpfs.h
 * @brief   内存文件系统：/tmp、/run、/dev/shm 的真实落点（按 4 KiB 页分配）
 *
 * @layer   fs
 * @owner   src/fs/
 * @stable  internal
 *
 * 依赖    `vfs.h`（vfs_node_t / vfs_ops_t）与 `errno.h`；
 *         不依赖 fs.c 的平铺文件表、不依赖块设备，也不依赖 core/pmm.h
 *         —— 分页后端由调用方注入，因此本模块可以在宿主机上完整单测。
 * 被依赖  src/vfs.c（路由 + 挂载）、tests/host/test_tmpfs.c
 *
 * 为什么需要它：
 *   ext2/exFAT 上的 /tmp 曾是"父目录用内存旁路 + 普通文件走 memfd 覆盖层"
 *   拼出来的假文件系统，语义分裂（rename 看不到、目录项不持久、权限位丢失、
 *   /dev/shm 与 /tmp 两套路径判定各写一遍）。tmpfs 用一个真正的内存
 *   inode 树替掉这两层旁路：同一份 lookup/readdir/rename/unlink 语义，
 *   目录与文件都是节点，重启即清空 —— 这正是 tmpfs 的语义。
 *
 * 分配粒度：数据按 4 KiB 页逐页申请、释放即归还；指针表按需整块扩容。
 *   内核注入 PMM（pmm_alloc_blocks/pmm_free_blocks），于是单个文件上限由
 *   物理内存与挂载配额决定，而不是 2 MiB 的 kmalloc 堆。宿主注入
 *   malloc/free（count 页连续内存）。
 *
 * 索引头文件
 *   类型  tmpfs_alloc_t   分配器注入（alloc_pages/free_pages，按 4 KiB 页）
 *   函数  tmpfs_set_allocator  设定分配器（必须在使用前调用一次）
 *         tmpfs_mount          在 path 挂一个容量上限为 max_bytes 的 tmpfs
 *         tmpfs_is_mounted     路径是否落在某个 tmpfs 挂载点内
 *         tmpfs_mount_path     第 index 个挂载点的路径（枚举用）
 *         tmpfs_mount_count    已挂载的 tmpfs 数量
 *         tmpfs_lookup         路径 → vfs 节点
 *         tmpfs_create         按路径创建文件/目录节点
 *         tmpfs_symlink        创建符号链接
 *         tmpfs_mkdir          建目录（含 mkdir -p 逐级补建）
 *         tmpfs_rmdir          删空目录
 *         tmpfs_unlink         删文件/符号链接
 *         tmpfs_rename         改名/移动（目录项级，不搬数据页）
 *         tmpfs_readdir_at     按索引列目录
 *         tmpfs_bytes_used     某挂载点已用字节数（配额诊断）
 */
#ifndef HBOS_FS_TMPFS_H
#define HBOS_FS_TMPFS_H

#include <stdint.h>
#include "vfs.h"

#define TMPFS_MAX_MOUNTS     8
#define TMPFS_PAGE_BYTES     4096u

/**
 * 分页分配器。
 *   alloc_pages(count) 取 count 个**连续且可直接访问**的 4 KiB 页，
 *                      返回页对齐指针，失败 NULL（内核用 PMM 的恒等映射地址）。
 *   free_pages(ptr,n)  归还同一块（n 必须与申请时一致）。
 * 不变量：alloc_pages 返回的内存在交给 tmpfs 前无需清零（tmpfs 自己 memset）。
 */
typedef struct {
    void *(*alloc_pages)(uint32_t count);
    void (*free_pages)(void *ptr, uint32_t count);
} tmpfs_alloc_t;

void tmpfs_set_allocator(const tmpfs_alloc_t *alloc);
int tmpfs_mount(const char *path, uint32_t max_bytes);
int tmpfs_is_mounted(const char *path);
uint32_t tmpfs_mount_count(void);
const char *tmpfs_mount_path(uint32_t index);
uint32_t tmpfs_bytes_used(const char *path);

vfs_node_t *tmpfs_lookup(const char *path);
vfs_node_t *tmpfs_create(const char *path, int type, uint32_t mode);
vfs_node_t *tmpfs_symlink(const char *path, const char *target);
int tmpfs_mkdir(const char *path);
int tmpfs_rmdir(const char *path);
int tmpfs_unlink(const char *path);
int tmpfs_rename(const char *old_path, const char *new_path, int replace);
int tmpfs_readdir_at(const char *path, uint32_t index, char *name, uint32_t *type);

#endif /* HBOS_FS_TMPFS_H */
