/**
 * @file    types.h
 * @brief   <stdint.h>/<stddef.h>/<stdbool.h> 的聚合入口
 *
 * @layer   user
 * @owner   src/sys/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  ssize_t     有符号大小
 *         off_t       文件偏移
 *         pid_t       进程号
 *         mode_t      文件模式位
 *         uid_t       用户号
 *         gid_t       组号
 *         dev_t       设备号
 *         ino_t       inode 号
 *         nlink_t     硬链接数
 *         time_t      秒级时间戳
 *         useconds_t  微秒数
 *         clock_t     时钟计数
 *         key_t       IPC 键
 *         socklen_t   套接字地址长度
 */
#ifndef HBOS_SYS_TYPES_H
#define HBOS_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef int64_t ssize_t;
typedef int64_t off_t;
typedef int32_t pid_t;
typedef uint32_t mode_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef uint64_t dev_t;
typedef uint64_t ino_t;
typedef uint32_t nlink_t;
typedef int64_t time_t;
typedef uint64_t useconds_t;
typedef uint64_t clock_t;
typedef int32_t key_t;
typedef int32_t socklen_t;

#endif
