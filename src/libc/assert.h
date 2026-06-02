/**
 * @file    assert.h
 * @brief   HBOS 标准库 — 对标 ANSI C <assert.h>
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