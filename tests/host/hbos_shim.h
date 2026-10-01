/* tests/host/hbos_shim.h — 宿主机测试用的符号改名与块设备替身。
 *
 * 为什么需要它：HBOS 内核自带 memset/memcpy/strlen 等实现，直接链接到宿主
 * 程序里会覆盖 glibc 的同名符号（libc 内部也在用它们，行为不可控）。这里把
 * 被测内核源码里的这些调用统一改名到 hbos_* 前缀，既能验证内核实现本身，
 * 又不干扰宿主 libc。
 *
 * 用法：编译内核 .c 时 -include 本文件。 */
#ifndef HBOS_HOST_SHIM_H
#define HBOS_HOST_SHIM_H

#include <stdint.h>
#include <stddef.h>

#define memset  hbos_memset
#define memcpy  hbos_memcpy
#define memmove hbos_memmove
#define memcmp  hbos_memcmp
#define memchr  hbos_memchr
#define strlen  hbos_strlen
#define strnlen hbos_strnlen
#define strcpy  hbos_strcpy
#define strncpy hbos_strncpy
#define strcat  hbos_strcat
#define strncat hbos_strncat
#define strcmp  hbos_strcmp
#define strncmp hbos_strncmp
#define strchr  hbos_strchr
#define strrchr hbos_strrchr
#define strstr  hbos_strstr

void *hbos_memset(void *s, int c, uint64_t n);
void *hbos_memcpy(void *dest, const void *src, uint64_t n);
void *hbos_memmove(void *dest, const void *src, size_t n);
int   hbos_memcmp(const void *s1, const void *s2, size_t n);
void *hbos_memchr(const void *s, int c, size_t n);
size_t hbos_strlen(const char *s);
size_t hbos_strnlen(const char *s, size_t maxlen);
char *hbos_strcpy(char *dest, const char *src);
char *hbos_strncpy(char *dest, const char *src, size_t n);
char *hbos_strcat(char *dest, const char *src);
char *hbos_strncat(char *dest, const char *src, size_t n);
int   hbos_strcmp(const char *s1, const char *s2);
int   hbos_strncmp(const char *s1, const char *s2, size_t n);
char *hbos_strchr(const char *s, int c);
char *hbos_strrchr(const char *s, int c);
char *hbos_strstr(const char *haystack, const char *needle);

/* 块命令计数器（host_fs_backend.c 里累加，测试结束时打印，用于对比
 * 优化前后的底层命令数——语义对拍的副产品，正好是性能证据）。 */
extern uint64_t hbostest_reads, hbostest_writes, hbostest_read_cmds, hbostest_write_cmds;

#endif
