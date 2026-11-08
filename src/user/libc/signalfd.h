/**
 * @file    signalfd.h
 * @brief   导出 4 个接口（1 类型 / 2 宏 / 1 函数）
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  signalfd_siginfo  （待补）
 *   宏    SFD_NONBLOCK  常量 / 特性宏
 *         SFD_CLOEXEC   常量 / 特性宏
 *   函数  signalfd    （待补）
 */
#ifndef HBOS_USER_LIBC_SIGNALFD_H
#define HBOS_USER_LIBC_SIGNALFD_H

#include <stdint.h>
#include <stddef.h>

#define SFD_NONBLOCK 0x800
#define SFD_CLOEXEC  0x80000

/* 128 字节 signalfd_siginfo（glibc 布局） */
struct signalfd_siginfo {
    uint32_t ssi_signo;
    int32_t  ssi_errno;
    int32_t  ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t  ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t  ssi_status;
    int32_t  ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint16_t ssi_pad2;
    int32_t  ssi_syscall;
    uint64_t ssi_call_addr;
    uint32_t ssi_arch;
    uint8_t  ssi_pad[28];
};

int signalfd(int fd, const void *sigmask, size_t sizemask);

#endif
