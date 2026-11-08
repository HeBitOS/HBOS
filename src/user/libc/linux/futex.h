/**
 * @file    futex.h
 * @brief   导出 9 个接口（0 类型 / 9 宏 / 0 函数）
 *
 * @layer   user
 * @owner   src/user/libc/linux/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    FUTEX_WAIT              FUTEX等待
 *         FUTEX_WAKE              常量 / 特性宏
 *         FUTEX_WAIT_BITSET       FUTEX等待BITSET
 *         FUTEX_WAKE_BITSET       常量 / 特性宏
 *         FUTEX_PRIVATE_FLAG      FUTEXPRIVATE标志
 *         FUTEX_CLOCK_REALTIME    FUTEX时钟REALTIME
 *         FUTEX_BITSET_MATCH_ANY  常量 / 特性宏
 *         FUTEX_WAIT_PRIVATE      FUTEX等待PRIVATE
 *         FUTEX_WAKE_PRIVATE      常量 / 特性宏
 */
#ifndef HBOS_USER_LIBC_LINUX_FUTEX_H
#define HBOS_USER_LIBC_LINUX_FUTEX_H

#define FUTEX_WAIT             0
#define FUTEX_WAKE             1
#define FUTEX_WAIT_BITSET      9
#define FUTEX_WAKE_BITSET     10
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_BITSET_MATCH_ANY 0xffffffffU
#define FUTEX_WAIT_PRIVATE    (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE    (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

#endif
