/**
 * @file    assert.h
 * @brief   导出 2 个接口（0 类型 / 1 宏 / 1 函数）
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    assert      断言失败即中止
 */
#ifndef HBOS_USER_LIBC_ASSERT_H
#define HBOS_USER_LIBC_ASSERT_H

void __assert_fail(const char *assertion, const char *file,
                    unsigned int line, const char *function);

#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : \
    __assert_fail(#e, __FILE__, __LINE__, __func__))
#endif

#endif
