/**
 * @file    memfd.h
 * @brief   导出 3 个接口（0 类型 / 2 宏 / 1 函数）
 *
 * @layer   user
 * @owner   src/user/libc/sys/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    MFD_CLOEXEC        常量 / 特性宏
 *         MFD_ALLOW_SEALING  常量 / 特性宏
 *   函数  memfd_create  （待补）
 */
#ifndef HBOS_USER_LIBC_SYS_MEMFD_H
#define HBOS_USER_LIBC_SYS_MEMFD_H

#define MFD_CLOEXEC       0x0001U
#define MFD_ALLOW_SEALING 0x0002U

int memfd_create(const char *name, unsigned int flags);

#endif
