/**
 * @file    stdlib.h
 * @brief   malloc/free/exit 等最常用的 stdlib 声明
 *
 * @layer   user
 * @owner   src/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    EXIT_SUCCESS  成功退出码（0）
 *         EXIT_FAILURE  失败退出码（1）
 *   函数  malloc      从堆分配 size 字节
 *         calloc      分配 nmemb×size 并清零
 *         realloc     调整已有分配块大小
 *         free        释放堆块
 *         exit        结束进程，不返回
 *         atoi        字符串转 int
 *         strtol      按进制转 long，endptr 收尾
 */
#ifndef HBOS_STDLIB_H
#define HBOS_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void exit(int status);
int atoi(const char *nptr);
long strtol(const char *nptr, char **endptr, int base);

#endif
