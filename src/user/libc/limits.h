/**
 * @file    limits.h
 * @brief   导出 18 个接口（0 类型 / 18 宏 / 0 函数）
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    CHAR_BIT    常量 / 特性宏
 *         UCHAR_MAX   常量 / 特性宏
 *         SCHAR_MIN   常量 / 特性宏
 *         SCHAR_MAX   常量 / 特性宏
 *         CHAR_MIN    常量 / 特性宏
 *         CHAR_MAX    常量 / 特性宏
 *         SHRT_MIN    常量 / 特性宏
 *         SHRT_MAX    常量 / 特性宏
 *         USHRT_MAX   常量 / 特性宏
 *         INT_MIN     常量 / 特性宏
 *         INT_MAX     常量 / 特性宏
 *         UINT_MAX    常量 / 特性宏
 *         LONG_MIN    常量 / 特性宏
 *         LONG_MAX    常量 / 特性宏
 *         ULONG_MAX   常量 / 特性宏
 *         LLONG_MIN   常量 / 特性宏
 *         LLONG_MAX   常量 / 特性宏
 *         ULLONG_MAX  常量 / 特性宏
 */
#ifndef HBOS_USER_LIBC_LIMITS_H
#define HBOS_USER_LIBC_LIMITS_H

#define CHAR_BIT   8
#define UCHAR_MAX  255
#define SCHAR_MIN  (-128)
#define SCHAR_MAX  127
#define CHAR_MIN   SCHAR_MIN
#define CHAR_MAX   SCHAR_MAX

#define SHRT_MIN   (-32768)
#define SHRT_MAX   32767
#define USHRT_MAX  65535

#define INT_MIN    (-2147483647 - 1)
#define INT_MAX    2147483647
#define UINT_MAX   4294967295U

#define LONG_MIN   (-9223372036854775807L - 1)
#define LONG_MAX   9223372036854775807L
#define ULONG_MAX  18446744073709551615UL

#define LLONG_MIN  (-9223372036854775807LL - 1)
#define LLONG_MAX  9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL

#endif
