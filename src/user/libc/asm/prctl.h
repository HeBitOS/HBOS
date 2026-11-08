/**
 * @file    prctl.h
 * @brief   导出 4 个接口（0 类型 / 4 宏 / 0 函数）
 *
 * @layer   user
 * @owner   src/user/libc/asm/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    ARCH_SET_GS  ARCH设置GS
 *         ARCH_SET_FS  ARCH设置FS
 *         ARCH_GET_FS  ARCH取FS
 *         ARCH_GET_GS  ARCH取GS
 */
#ifndef HBOS_USER_LIBC_ASM_PRCTL_H
#define HBOS_USER_LIBC_ASM_PRCTL_H

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

#endif
