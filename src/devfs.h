/**
 * @file    devfs.h
 * @brief   /dev + /proc 伪文件系统 — 设备节点 + 进程信息
 *
 * 提供合成 VFS 节点:
 *   /dev/null    — 空设备 (read=EOF, write=/dev/null)
 *   /dev/zero    — 零设备 (read=0x00, write=/dev/null)
 *   /dev/console — 控制台设备 (read=键盘, write=屏幕)
 *   /proc/uptime — 系统运行时间
 *   /proc/meminfo — 内存信息
 *   /proc/cpuinfo — CPU 信息
 *   /proc/<pid>/cmdline — 进程命令行
 *   /proc/<pid>/status  — 进程状态
 */

#ifndef HBOS_DEVFS_H
#define HBOS_DEVFS_H

#include "vfs.h"

/** 初始化 /dev 和 /proc 伪文件系统节点 */
void devfs_init(void);

/** 尝试解析伪文件系统路径，返回合成 VFS 节点或 NULL */
vfs_node_t *devfs_lookup(const char *path);

/** 枚举 /dev、/proc 和 /proc/<pid> 目录项 */
int devfs_readdir(const char *path, uint32_t index, char *name, uint32_t *type);

/** 注册伪文件系统节点到全局表中 */
void devfs_register_nodes(void);

#endif
