/**
 * @file    stdlib.h
 * @brief   HBOS 标准库 — 对标 ANSI C <stdlib.h>
 *
 * @layer   user
 * @owner   src/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    EXIT_SUCCESS  成功退出码（0）
 *         EXIT_FAILURE  失败退出码（1）
 *         RAND_MAX      rand() 最大值
 *   函数  malloc      从堆分配 size 字节
 *         free        释放堆块
 *         calloc      分配 nmemb×size 并清零
 *         realloc     调整已有分配块大小
 *         atoi        字符串转 int
 *         atol        字符串转 long
 *         atoll       （待补）
 *         atof        字符串转 double
 *         strtol      按进制转 long，endptr 收尾
 *         strtoul     按进制转 unsigned long
 *         abort       异常终止
 *         exit        结束进程，不返回
 *         system      （待补）
 *         getenv      取环境变量
 *         setenv      设置环境变量
 *         rand        伪随机数
 *         srand       设置随机种子
 *         abs         绝对值
 *         labs        long 绝对值
 */
#ifndef HBOS_LIBC_STDLIB_H
#define HBOS_LIBC_STDLIB_H

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
long long atoll(const char *s);
double atof(const char *s);
long  strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);

void  abort(void);
void  exit(int status);
int   atexit(void (*func)(void));

int   system(const char *cmd);
char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);

int   rand(void);
void  srand(unsigned int seed);

void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size, int (*compar)(const void *, const void *));
void  qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *));

int   abs(int n);
long  labs(long n);

#endif