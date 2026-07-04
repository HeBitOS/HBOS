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
