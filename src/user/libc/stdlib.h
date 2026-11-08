/**
 * @file    stdlib.h
 * @brief   malloc/free/exit 等最常用的 stdlib 声明
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    EXIT_SUCCESS  成功退出码（0）
 *         EXIT_FAILURE  失败退出码（1）
 *         RAND_MAX      rand() 最大值
 *         PATH_MAX      常量 / 特性宏
 *         alloca        常量 / 特性宏
 *   函数  malloc      从堆分配 size 字节
 *         free        释放堆块
 *         calloc      分配 nmemb×size 并清零
 *         realloc     调整已有分配块大小
 *         atoi        字符串转 int
 *         atol        字符串转 long
 *         strtol      按进制转 long，endptr 收尾
 *         strtoul     按进制转 unsigned long
 *         strtoull    按进制转 unsigned long long
 *         realpath    （待补）
 *         abort       异常终止
 *         rand        伪随机数
 *         srand       设置随机种子
 *         abs         绝对值
 *         labs        long 绝对值
 *         getenv      取环境变量
 */
#ifndef HBOS_USER_LIBC_STDLIB_H
#define HBOS_USER_LIBC_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

int   atoi(const char *s);
long  atol(const char *s);
long  strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);

#define PATH_MAX 256
char *realpath(const char *path, char *resolved_path);

void  abort(void);
void  exit(int status) __attribute__((noreturn));

int   rand(void);
void  srand(unsigned int seed);

int   abs(int n);
long  labs(long n);

void  qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *));

/* HBOS has no environment-variable subsystem; always returns NULL, meaning
 * "not set" to callers (e.g. TinyCC treats a NULL TCC_LIBRARY_PATH as
 * "use compiled-in default"). */
char *getenv(const char *name);

#endif
#define alloca(size) __builtin_alloca(size)
