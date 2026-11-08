/**
 * @file    string.h
 * @brief   mem 系列与 str 系列的声明（内核与用户态共用）
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   函数  memset      填充 n 字节为 c，返回 s
 *         memcpy      复制 n 字节，区域不得重叠
 *         memmove     复制 n 字节，允许重叠
 *         memcmp      比较 n 字节，返回差值符号
 *         strlen      字符串长度（不含结尾 NUL）
 *         strcmp      比较字符串，返回差值符号
 *         strncmp     比较前 n 个字符
 *         strcpy      复制字符串（含结尾 NUL）
 *         strncpy     复制至多 n 字符，不足补 NUL
 *         strcat      追加字符串
 *         memchr      在 n 字节内找字符 c
 *         strchr      找首次出现的字符
 *         strrchr     找末次出现的字符
 *         strstr      找子串，返回首次出现位置
 *         strpbrk     找首个属于字符集的字符
 *         strdup      复制到新分配的堆内存
 *         strerror    errno 值转错误描述
 */
#ifndef HBOS_USER_LIBC_STRING_H
#define HBOS_USER_LIBC_STRING_H

#include <stddef.h>

void *memset(void *s, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *s1, const void *s2, size_t n);
size_t strlen(const char *s);
int   strcmp(const char *s1, const char *s2);
int   strncmp(const char *s1, const char *s2, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
void *memchr(const void *s, int c, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strpbrk(const char *s, const char *accept);
char *strdup(const char *s);
char *strerror(int errnum);

#endif