/**
 * @file    fd.h
 * @brief   文件描述符表
 *
 * @layer   fs
 * @owner   src/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  pipe_t      结构体 / 类型别名
 *         fd_entry_t  结构体 / 类型别名
 *         fd_table_t  结构体 / 类型别名
 *   宏    POSIX_MAX_FDS  常量 / 特性宏
 *         FD_FILE        常量 / 特性宏
 *         FD_SOCKET      FD套接字
 *         FD_PIPE        常量 / 特性宏
 *         FD_EVENT       FD事件
 *         FD_EPOLL       常量 / 特性宏
 *         FD_UNIX        常量 / 特性宏
 *         FD_MEMFD       常量 / 特性宏
 *         FD_INOTIFY     常量 / 特性宏
 *         FD_TIMER       常量 / 特性宏
 *         FD_SIGNALFD    常量 / 特性宏
 *         PIPE_BUF_SIZE  PIPE缓冲区
 */
#ifndef HBOS_FD_H
#define HBOS_FD_H

#include <stdbool.h>
#include <stdint.h>
#include "vfs.h"

/* 每个进程的 fd 表容量。
 *
 * 保持 128：fd_entry_t 带 path[256]，单表约 37KB，×64 任务是 2.3MB 静态
 * BSS；提到 512 会变成 9.2MB，对内核不可接受。
 * Chromium 子进程会 F_DUPFD_CLOEXEC(fd, 512) 想要一个高位 fd —— 那个 512
 * 只是"希望不小于"的偏好，不是容量要求。解法是在 fcntl 里回退到任意可用
 * fd（见 src/syscall.c 的 case 1030），而不是把表撑大。 */
#define POSIX_MAX_FDS 128

#define FD_FILE    1
#define FD_SOCKET  2
#define FD_PIPE    3
#define FD_EVENT   4
#define FD_EPOLL   5
#define FD_UNIX    6
#define FD_MEMFD   7
#define FD_INOTIFY 8
#define FD_TIMER   9
#define FD_SIGNALFD 10

#define PIPE_BUF_SIZE 4096

typedef struct {
    uint8_t buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
    int ref_count;
} pipe_t;

typedef struct {
    bool used;
    vfs_node_t *node;
    uint32_t offset;
    uint32_t compat_id;
    int flags;
    int type;
    uint16_t local_port;
    pipe_t *pipe;
    /* Resolved path this fd was opened with. vfs_node_t itself has no
     * parent/children links (see vfs.h) -- listing a directory's contents
     * only works via the path-string-based fs_readdir()/vfs_readdir_at(),
     * not a per-node op, so a directory fd needs this to answer getdents()
     * (see HBOS_SYS_GETDENTS in syscall.c). */
    char path[256];
} fd_entry_t;

typedef struct {
    fd_entry_t entries[POSIX_MAX_FDS];
    uint32_t refs;
} fd_table_t;

#endif
