/**
 * @file    assert.h
 * @brief   HBOS 标准库 — 对标 ANSI C <assert.h>
 *
 * @layer   user
 * @owner   src/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    assert      断言失败即中止
 *   函数  __assert_fail  （待补）
 */
#ifndef HBOS_LIBC_ASSERT_H
#define HBOS_LIBC_ASSERT_H

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
void __assert_fail(const char *expr, const char *file, int line, const char *func);
#define assert(expr) \
    ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__, __func__))
#endif

#endif