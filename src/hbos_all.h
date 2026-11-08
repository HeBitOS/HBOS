/**
 * @file    hbos_all.h
 * @brief   聚合索引：src/ 下全部 179 个头文件的入口
 *
 * @layer   top
 * @owner   scripts/gen_all_headers.py
 * @stable  generated（自动生成，勿手改）
 *
 * 用途
 *   **仅供编辑器建立索引**（Ctrl+Click 跳转 / 找引用 / 补全），
 *   不是编译契约。任何 .c 都不应 include 本文件——它会把你
 *   拉进整个内核的依赖图，编译时间翻倍且极易撞上符号冲突。
 *
 * 为什么分三支
 *   HBOS 有 16 组同名头，几乎全部落在「内核侧 / 用户态侧」这条
 *   分界线上。两侧同名函数签名按设计不同，例如：
 *     src/string.h            memcpy(…, uint64_t n)
 *     src/user/libc/string.h  memcpy(…, size_t n)
 *   这不是可合并的重复代码，而是两个编译环境。硬塞进同一个
 *   翻译单元会产生上百个 redefinition。
 *
 *   net/ 下的 lwip / mbedtls 配置文件是第三种情况：它们本身不冲突，
 *   但必须配第三方 include 路径才能展开，所以单独一支。
 *
 * 怎么用
 *   在编辑器里为每一支各建一个编译配置即可（或用下面的宏切换）：
 *     默认                                内核侧        134 个头
 *     HBOS_ALL_WITH_USER=1              用户态 libc    38 个头
 *     HBOS_ALL_WITH_NET=1               安全网络配置    7 个头
 *   三支互斥，同时打开多个只有第一个生效。
 *
 * 编译参数（复制即用）
 *   内核侧： -Isrc -Isrc/graphics -Isrc/shell -Isrc/core -Isrc/tools -Isrc/gui -Isrc/user -Isrc/net
 *   用户态： -Isrc/user/libc -Isrc/user/libc/sys -Isrc/user
 *   网络：   见 mk/secure_net.mk 的 LWIP_CFLAGS / MBEDTLS_APP_CFLAGS
 *
 * 索引头文件
 *   宏    HBOS_ALL_WITH_USER    1 = 走用户态 libc 侧
 *   宏    HBOS_ALL_WITH_NET     1 = 走安全网络配置侧
 *   宏    HBOS_ALL_KERNEL_N     134（内核侧头文件数，默认支）
 *   宏    HBOS_ALL_USER_N       38（用户态头文件数）
 *   宏    HBOS_ALL_NET_N        7（网络配置头文件数）
 */
#ifndef HBOS_ALL_H
#define HBOS_ALL_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define HBOS_ALL_KERNEL_N 134
#define HBOS_ALL_USER_N 38
#define HBOS_ALL_NET_N 7

#if defined(HBOS_ALL_WITH_USER) && HBOS_ALL_WITH_USER
/* ══ 用户态 libc 侧 ══════════════════════════════════════ */

/* ── src/user/libc/asm/prctl.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/asm/prctl.h */
#include "user/libc/asm/prctl.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/user/libc/asm/prctl.h */

/* ── src/user/libc/assert.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/assert.h */
#include "user/libc/assert.h"

/* 见 src/user/libc/assert.h:18 */
void __assert_fail(const char *assertion, const char *file, unsigned int line, const char *function);

/* ── src/user/libc/dirent.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/dirent.h */
#include "user/libc/dirent.h"

/* 见 src/user/libc/dirent.h:60 */
DIR *opendir(const char *path);
/* 见 src/user/libc/dirent.h:63 */
struct dirent *readdir(DIR *d);
/* 见 src/user/libc/dirent.h:66 */
int closedir(DIR *d);
/* 见 src/user/libc/dirent.h:69 */
int getdents(int fd, struct dirent *dirp, unsigned int count);

/* ── src/user/libc/dlfcn.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/dlfcn.h */
#include "user/libc/dlfcn.h"

/* 见 src/user/libc/dlfcn.h:24 */
void *dlopen(const char *filename, int flags);
/* 见 src/user/libc/dlfcn.h:25 */
void *dlsym(void *handle, const char *symbol);
/* 见 src/user/libc/dlfcn.h:26 */
void *dlvsym(void *handle, const char *symbol, const char *version);
/* 见 src/user/libc/dlfcn.h:27 */
int dlclose(void *handle);
/* 见 src/user/libc/dlfcn.h:28 */
char *dlerror(void);

/* ── src/user/libc/errno.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/errno.h */
#include "user/libc/errno.h"

/* 见 src/user/libc/errno.h:78 */
int *__errno_location(void);
/* * silently drops the error code.  ── 见 src/user/libc/errno.h:87 */
long __syscall_errno(long ret);

/* ── src/user/libc/fcntl.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/fcntl.h */
#include "user/libc/fcntl.h"

/* 见 src/user/libc/fcntl.h:30 */
int open(const char *path, int flags, ...);

/* ── src/user/libc/fenv.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/fenv.h */
#include "user/libc/fenv.h"

/* inline 定义 feclearexcept  ── 见 src/user/libc/fenv.h:47 */
/* inline 定义 feraiseexcept  ── 见 src/user/libc/fenv.h:48 */
/* inline 定义 fetestexcept  ── 见 src/user/libc/fenv.h:49 */
/* inline 定义 fegetround  ── 见 src/user/libc/fenv.h:50 */
/* inline 定义 fesetround  ── 见 src/user/libc/fenv.h:51 */
/* inline 定义 fegetenv  ── 见 src/user/libc/fenv.h:52 */
/* inline 定义 fesetenv  ── 见 src/user/libc/fenv.h:53 */

/* ── src/user/libc/getopt.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/getopt.h */
#include "user/libc/getopt.h"

/* 见 src/user/libc/getopt.h:25 */
int getopt(int argc, char *const argv[], const char *optstring);

/* ── src/user/libc/limits.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/limits.h */
#include "user/libc/limits.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/user/libc/limits.h */

/* ── src/user/libc/linux/futex.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/linux/futex.h */
#include "user/libc/linux/futex.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/user/libc/linux/futex.h */

/* ── src/user/libc/math.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/math.h */
#include "user/libc/math.h"

/* 见 src/user/libc/math.h:128 */
double fabs(double x);
/* 见 src/user/libc/math.h:129 */
double floor(double x);
/* 见 src/user/libc/math.h:130 */
double ceil(double x);
/* 见 src/user/libc/math.h:131 */
double trunc(double x);
/* 见 src/user/libc/math.h:132 */
double round(double x);
/* 见 src/user/libc/math.h:133 */
double sqrt(double x);
/* 见 src/user/libc/math.h:134 */
double fmod(double x, double y);
/* 见 src/user/libc/math.h:135 */
double modf(double x, double *iptr);
/* 见 src/user/libc/math.h:136 */
double copysign(double x, double y);
/* 见 src/user/libc/math.h:137 */
double fmin(double x, double y);
/* 见 src/user/libc/math.h:138 */
double fmax(double x, double y);
/* 见 src/user/libc/math.h:139 */
double remainder(double x, double y);
/* 见 src/user/libc/math.h:140 */
double scalbn(double x, int n);
/* 见 src/user/libc/math.h:141 */
double ldexp(double x, int n);
/* 见 src/user/libc/math.h:142 */
double frexp(double x, int *e);
/* 见 src/user/libc/math.h:143 */
double nearbyint(double x);
/* 见 src/user/libc/math.h:144 */
long lrint(double x);
/* 见 src/user/libc/math.h:145 */
double rint(double x);
/* 见 src/user/libc/math.h:146 */
double sin(double x);
/* 见 src/user/libc/math.h:147 */
double cos(double x);
/* 见 src/user/libc/math.h:148 */
double tan(double x);
/* 见 src/user/libc/math.h:149 */
double asin(double x);
/* 见 src/user/libc/math.h:150 */
double acos(double x);
/* 见 src/user/libc/math.h:151 */
double atan(double x);
/* 见 src/user/libc/math.h:152 */
double atan2(double y, double x);
/* 见 src/user/libc/math.h:153 */
double sinh(double x);
/* 见 src/user/libc/math.h:154 */
double cosh(double x);
/* 见 src/user/libc/math.h:155 */
double tanh(double x);
/* 见 src/user/libc/math.h:156 */
double asinh(double x);
/* 见 src/user/libc/math.h:157 */
double acosh(double x);
/* 见 src/user/libc/math.h:158 */
double atanh(double x);
/* 见 src/user/libc/math.h:159 */
double exp(double x);
/* 见 src/user/libc/math.h:160 */
double exp2(double x);
/* 见 src/user/libc/math.h:161 */
double expm1(double x);
/* 见 src/user/libc/math.h:162 */
double log(double x);
/* 见 src/user/libc/math.h:163 */
double log2(double x);
/* 见 src/user/libc/math.h:164 */
double log10(double x);
/* 见 src/user/libc/math.h:165 */
double log1p(double x);
/* 见 src/user/libc/math.h:166 */
double pow(double x, double y);
/* 见 src/user/libc/math.h:167 */
double cbrt(double x);
/* 见 src/user/libc/math.h:168 */
double hypot(double x, double y);

/* ── src/user/libc/netdb.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/netdb.h */
#include "user/libc/netdb.h"

/* 见 src/user/libc/netdb.h:75 */
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
/* 见 src/user/libc/netdb.h:77 */
void freeaddrinfo(struct addrinfo *res);
/* 见 src/user/libc/netdb.h:78 */
struct hostent *gethostbyname(const char *name);

/* ── src/user/libc/poll.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/poll.h */
#include "user/libc/poll.h"

/* 见 src/user/libc/poll.h:41 */
int poll(struct pollfd *fds, nfds_t nfds, int timeout);

/* ── src/user/libc/pthread.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/pthread.h */
#include "user/libc/pthread.h"

/* inline 定义 pthread_mutex_init  ── 见 src/user/libc/pthread.h:40 */
/* inline 定义 pthread_mutex_destroy  ── 见 src/user/libc/pthread.h:46 */
/* inline 定义 pthread_mutex_lock  ── 见 src/user/libc/pthread.h:50 */
/* inline 定义 pthread_mutex_unlock  ── 见 src/user/libc/pthread.h:54 */
/* inline 定义 pthread_cond_init  ── 见 src/user/libc/pthread.h:58 */
/* inline 定义 pthread_cond_destroy  ── 见 src/user/libc/pthread.h:64 */
/* inline 定义 pthread_cond_signal  ── 见 src/user/libc/pthread.h:68 */
/* inline 定义 pthread_cond_broadcast  ── 见 src/user/libc/pthread.h:72 */
/* inline 定义 pthread_cond_wait — * thread, so report an immediate timeout (Atomics.wait semantics).  ── 见 src/user/libc/pthread.h:78 */
/* inline 定义 pthread_cond_timedwait  ── 见 src/user/libc/pthread.h:83 */

/* ── src/user/libc/sched.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/sched.h */
#include "user/libc/sched.h"

/* 见 src/user/libc/sched.h:42 */
int sched_yield(void);
/* CPU 亲和位图：每个 bit 一个 CPU  ── 见 src/user/libc/sched.h:45 */
int sched_getaffinity(int pid, size_t cpusetsize, unsigned long *mask);
/* 见 src/user/libc/sched.h:46 */
int clone(int (*function)(void *), void *child_stack, int flags, void *argument, ...);

/* ── src/user/libc/setjmp.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/setjmp.h */
#include "user/libc/setjmp.h"

/* Hand-written in setjmp.asm (x86-64 SysV), not C — see that file for why.  ── 见 src/user/libc/setjmp.h:24 */
int setjmp(jmp_buf env);
/* 见 src/user/libc/setjmp.h:25 */
void longjmp(jmp_buf env, int val);

/* ── src/user/libc/signalfd.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/signalfd.h */
#include "user/libc/signalfd.h"

/* 见 src/user/libc/signalfd.h:53 */
int signalfd(int fd, const void *sigmask, size_t sizemask);

/* ── src/user/libc/socket.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/socket.h */
#include "user/libc/socket.h"

/* 见 src/user/libc/socket.h:147 */
int socket(int domain, int type, int protocol);
/* 见 src/user/libc/socket.h:148 */
int socketpair(int domain, int type, int protocol, int pair[2]);
/* 见 src/user/libc/socket.h:149 */
int bind(int sockfd, const void *addr, socklen_t addrlen);
/* 见 src/user/libc/socket.h:150 */
int listen(int sockfd, int backlog);
/* 见 src/user/libc/socket.h:151 */
int accept(int sockfd, void *addr, socklen_t *addrlen);
/* 见 src/user/libc/socket.h:152 */
int accept4(int sockfd, void *addr, socklen_t *addrlen, int flags);
/* 见 src/user/libc/socket.h:153 */
int connect(int sockfd, const void *addr, socklen_t addrlen);
/* 见 src/user/libc/socket.h:154 */
int getsockname(int sockfd, void *addr, socklen_t *addrlen);
/* 见 src/user/libc/socket.h:155 */
int getpeername(int sockfd, void *addr, socklen_t *addrlen);
/* 见 src/user/libc/socket.h:156 */
long send(int sockfd, const void *buf, size_t len, int flags);
/* 见 src/user/libc/socket.h:157 */
long recv(int sockfd, void *buf, size_t len, int flags);
/* 见 src/user/libc/socket.h:158 */
long sendto(int sockfd, const void *buf, size_t len, int flags, const void *addr, socklen_t addrlen);
/* 见 src/user/libc/socket.h:160 */
long recvfrom(int sockfd, void *buf, size_t len, int flags, void *addr, socklen_t *addrlen);
/* 见 src/user/libc/socket.h:162 */
long sendmsg(int sockfd, const struct msghdr *message, int flags);
/* 见 src/user/libc/socket.h:163 */
long recvmsg(int sockfd, struct msghdr *message, int flags);
/* 见 src/user/libc/socket.h:164 */
int getsockopt(int sockfd, int level, int option, void *value, socklen_t *length);
/* 见 src/user/libc/socket.h:166 */
int setsockopt(int sockfd, int level, int option, const void *value, socklen_t length);
/* 见 src/user/libc/socket.h:168 */
int shutdown(int sockfd, int how);
/* 见 src/user/libc/socket.h:169 */
int close(int fd);
/* 见 src/user/libc/socket.h:171 */
uint32_t htonl(uint32_t hostlong);
/* 见 src/user/libc/socket.h:172 */
uint16_t htons(uint16_t hostshort);
/* 见 src/user/libc/socket.h:173 */
uint32_t ntohl(uint32_t netlong);
/* 见 src/user/libc/socket.h:174 */
uint16_t ntohs(uint16_t netshort);
/* 见 src/user/libc/socket.h:176 */
uint32_t inet_addr(const char *cp);

/* ── src/user/libc/statfs.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/statfs.h */
#include "user/libc/statfs.h"

/* 见 src/user/libc/statfs.h:40 */
int statfs(const char *path, struct statfs *buf);
/* 见 src/user/libc/statfs.h:41 */
int fstatfs(int fd, struct statfs *buf);

/* ── src/user/libc/stdio.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/stdio.h */
#include "user/libc/stdio.h"

/* 见 src/user/libc/stdio.h:76 */
FILE *fopen(const char *path, const char *mode);
/* 见 src/user/libc/stdio.h:77 */
FILE *fdopen(int fd, const char *mode);
/* 见 src/user/libc/stdio.h:78 */
int fclose(FILE *fp);
/* 见 src/user/libc/stdio.h:79 */
int remove(const char *path);
/* 见 src/user/libc/stdio.h:80 */
int fflush(FILE *fp);
/* 见 src/user/libc/stdio.h:81 */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
/* 见 src/user/libc/stdio.h:82 */
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
/* 见 src/user/libc/stdio.h:83 */
int fseek(FILE *fp, long offset, int whence);
/* 见 src/user/libc/stdio.h:84 */
long ftell(FILE *fp);
/* 见 src/user/libc/stdio.h:85 */
int fgetc(FILE *fp);
/* 见 src/user/libc/stdio.h:87 */
int fputc(int c, FILE *fp);
/* 见 src/user/libc/stdio.h:89 */
char *fgets(char *s, int size, FILE *fp);
/* 见 src/user/libc/stdio.h:90 */
int fputs(const char *s, FILE *fp);
/* 见 src/user/libc/stdio.h:91 */
int feof(FILE *fp);
/* 见 src/user/libc/stdio.h:92 */
int ferror(FILE *fp);
/* 见 src/user/libc/stdio.h:94 */
int printf(const char *fmt, ...);
/* 见 src/user/libc/stdio.h:95 */
int fprintf(FILE *fp, const char *fmt, ...);
/* 见 src/user/libc/stdio.h:96 */
int sprintf(char *buf, const char *fmt, ...);
/* 见 src/user/libc/stdio.h:97 */
int snprintf(char *buf, size_t n, const char *fmt, ...);
/* 见 src/user/libc/stdio.h:98 */
int vprintf(const char *fmt, va_list ap);
/* 见 src/user/libc/stdio.h:99 */
int vfprintf(FILE *fp, const char *fmt, va_list ap);
/* 见 src/user/libc/stdio.h:100 */
int vsprintf(char *buf, const char *fmt, va_list ap);
/* 见 src/user/libc/stdio.h:101 */
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
/* 见 src/user/libc/stdio.h:103 */
int getchar(void);
/* 见 src/user/libc/stdio.h:104 */
int putchar(int c);
/* 见 src/user/libc/stdio.h:105 */
int puts(const char *s);

/* ── src/user/libc/stdlib.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/stdlib.h */
#include "user/libc/stdlib.h"

/* 见 src/user/libc/stdlib.h:44 */
void *malloc(size_t size);
/* 见 src/user/libc/stdlib.h:45 */
void free(void *ptr);
/* 见 src/user/libc/stdlib.h:46 */
void *calloc(size_t nmemb, size_t size);
/* 见 src/user/libc/stdlib.h:47 */
void *realloc(void *ptr, size_t size);
/* 见 src/user/libc/stdlib.h:49 */
int atoi(const char *s);
/* 见 src/user/libc/stdlib.h:50 */
long atol(const char *s);
/* 见 src/user/libc/stdlib.h:51 */
long strtol(const char *s, char **endptr, int base);
/* 见 src/user/libc/stdlib.h:52 */
unsigned long strtoul(const char *s, char **endptr, int base);
/* 见 src/user/libc/stdlib.h:53 */
unsigned long long strtoull(const char *s, char **endptr, int base);
/* 见 src/user/libc/stdlib.h:56 */
char *realpath(const char *path, char *resolved_path);
/* 见 src/user/libc/stdlib.h:58 */
void abort(void);
/* 见 src/user/libc/stdlib.h:61 */
int rand(void);
/* 见 src/user/libc/stdlib.h:62 */
void srand(unsigned int seed);
/* 见 src/user/libc/stdlib.h:64 */
int abs(int n);
/* 见 src/user/libc/stdlib.h:65 */
long labs(long n);
/* 见 src/user/libc/stdlib.h:67 */
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
/* * "use compiled-in default").  ── 见 src/user/libc/stdlib.h:73 */
char *getenv(const char *name);

/* ── src/user/libc/string.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/string.h */
#include "user/libc/string.h"

/* 见 src/user/libc/string.h:36 */
void *memset(void *s, int c, size_t n);
/* 见 src/user/libc/string.h:37 */
void *memcpy(void *dst, const void *src, size_t n);
/* 见 src/user/libc/string.h:38 */
void *memmove(void *dst, const void *src, size_t n);
/* 见 src/user/libc/string.h:39 */
int memcmp(const void *s1, const void *s2, size_t n);
/* 见 src/user/libc/string.h:40 */
size_t strlen(const char *s);
/* 见 src/user/libc/string.h:41 */
int strcmp(const char *s1, const char *s2);
/* 见 src/user/libc/string.h:42 */
int strncmp(const char *s1, const char *s2, size_t n);
/* 见 src/user/libc/string.h:43 */
char *strcpy(char *dst, const char *src);
/* 见 src/user/libc/string.h:44 */
char *strncpy(char *dst, const char *src, size_t n);
/* 见 src/user/libc/string.h:45 */
char *strcat(char *dst, const char *src);
/* 见 src/user/libc/string.h:46 */
void *memchr(const void *s, int c, size_t n);
/* 见 src/user/libc/string.h:47 */
char *strchr(const char *s, int c);
/* 见 src/user/libc/string.h:48 */
char *strrchr(const char *s, int c);
/* 见 src/user/libc/string.h:49 */
char *strstr(const char *haystack, const char *needle);
/* 见 src/user/libc/string.h:50 */
char *strpbrk(const char *s, const char *accept);
/* 见 src/user/libc/string.h:51 */
char *strdup(const char *s);
/* 见 src/user/libc/string.h:52 */
char *strerror(int errnum);

/* ── src/user/libc/sys/epoll.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/sys/epoll.h */
#include "user/libc/sys/epoll.h"

/* 见 src/user/libc/sys/epoll.h:67 */
int epoll_create1(int flags);
/* 见 src/user/libc/sys/epoll.h:68 */
int epoll_ctl(int epfd, int operation, int fd, struct epoll_event *event);
/* 见 src/user/libc/sys/epoll.h:69 */
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);

/* ── src/user/libc/sys/eventfd.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/sys/eventfd.h */
#include "user/libc/sys/eventfd.h"

/* 见 src/user/libc/sys/eventfd.h:32 */
int eventfd(unsigned int initial_value, int flags);
/* 见 src/user/libc/sys/eventfd.h:33 */
int eventfd_read(int fd, eventfd_t *value);
/* 见 src/user/libc/sys/eventfd.h:34 */
int eventfd_write(int fd, eventfd_t value);

/* ── src/user/libc/sys/memfd.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/sys/memfd.h */
#include "user/libc/sys/memfd.h"

/* 见 src/user/libc/sys/memfd.h:23 */
int memfd_create(const char *name, unsigned int flags);

/* ── src/user/libc/sys/mman.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/sys/mman.h */
#include "user/libc/sys/mman.h"

/* 见 src/user/libc/sys/mman.h:51 */
void *mremap(void *old_address, size_t old_size, size_t new_size, int flags, ...);
/* 见 src/user/libc/sys/mman.h:54 */
void *mmap(void *address, size_t length, int protection, int flags, int fd, long offset);
/* 见 src/user/libc/sys/mman.h:56 */
int munmap(void *address, size_t length);
/* 见 src/user/libc/sys/mman.h:57 */
int mprotect(void *address, size_t length, int protection);

/* ── src/user/libc/sys/random.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/sys/random.h */
#include "user/libc/sys/random.h"

/* 见 src/user/libc/sys/random.h:25 */
long getrandom(void *buffer, size_t length, unsigned int flags);

/* ── src/user/libc/sys/socket.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/sys/socket.h */
#include "user/libc/sys/socket.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/user/libc/sys/socket.h */

/* ── src/user/libc/sys/stat.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/sys/stat.h */
#include "user/libc/sys/stat.h"

/* 见 src/user/libc/sys/stat.h:77 */
int stat(const char *path, struct stat *buf);
/* 见 src/user/libc/sys/stat.h:78 */
int fstat(int fd, struct stat *buf);
/* 见 src/user/libc/sys/stat.h:79 */
int mkdir(const char *path, mode_t mode);
/* lstat() does not follow the final ramfs/HBFS symbolic-link component.  ── 见 src/user/libc/sys/stat.h:81 */
int lstat(const char *path, struct stat *buf);

/* ── src/user/libc/sys/syscall.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/sys/syscall.h */
#include "user/libc/sys/syscall.h"

/* 见 src/user/libc/sys/syscall.h:220 */
long syscall(long linux_number, ...);

/* ── src/user/libc/sys/time.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/sys/time.h */
#include "user/libc/sys/time.h"

/* * exactly (see src/user/libc/time.c).  ── 见 src/user/libc/sys/time.h:27 */
int gettimeofday(struct timeval *tv, void *tz);

/* ── src/user/libc/sys/uio.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/sys/uio.h */
#include "user/libc/sys/uio.h"

/* 见 src/user/libc/sys/uio.h:28 */
ssize_t readv(int fd, const struct iovec *vectors, int count);
/* 见 src/user/libc/sys/uio.h:29 */
ssize_t writev(int fd, const struct iovec *vectors, int count);

/* ── src/user/libc/sys/un.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/sys/un.h */
#include "user/libc/sys/un.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/user/libc/sys/un.h */

/* ── src/user/libc/sys/wait.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/sys/wait.h */
#include "user/libc/sys/wait.h"

/* * and isn't declared here. Add it if/when the kernel gains that.  ── 见 src/user/libc/sys/wait.h:37 */
pid_t waitpid(pid_t pid, int *status, int options);

/* ── src/user/libc/syscall.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/syscall.h */
#include "user/libc/syscall.h"

/* 见 src/user/libc/syscall.h:268 */
long __syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);
/* 见 src/user/libc/syscall.h:269 */
long __syscall3(long nr, long a0, long a1, long a2);
/* 见 src/user/libc/syscall.h:270 */
long __syscall1(long nr, long a0);

/* ── src/user/libc/time.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/time.h */
#include "user/libc/time.h"

/* * want a coarse timestamp (e.g. TinyCC's __TIMESTAMP__/__DATE__ support).  ── 见 src/user/libc/time.h:42 */
time_t time(time_t *tloc);
/* * TinyCC's only use of this is cosmetic __DATE__/__TIME__ macro text.  ── 见 src/user/libc/time.h:59 */
struct tm *localtime(const time_t *timer);
/* 见 src/user/libc/time.h:60 */
struct tm *localtime_r(const time_t *timer, struct tm *result);
/* 见 src/user/libc/time.h:61 */
int clock_gettime(clockid_t clockid, struct timespec *tp);

/* ── src/user/libc/timerfd.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/timerfd.h */
#include "user/libc/timerfd.h"

/* 见 src/user/libc/timerfd.h:34 */
int timerfd_create(int clockid, int flags);
/* 见 src/user/libc/timerfd.h:35 */
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value, struct itimerspec *old_value);
/* 见 src/user/libc/timerfd.h:38 */
int timerfd_gettime(int fd, struct itimerspec *curr_value);

/* ── src/user/libc/unistd.h ──────────────────────────────── */
/* 真实声明位于 src/user/libc/unistd.h */
#include "user/libc/unistd.h"

/* 见 src/user/libc/unistd.h:73 */
unsigned int sleep(unsigned int seconds);
/* 见 src/user/libc/unistd.h:74 */
int usleep(unsigned int useconds);
/* 见 src/user/libc/unistd.h:75 */
pid_t getpid(void);
/* 见 src/user/libc/unistd.h:76 */
char *getcwd(char *buf, size_t size);
/* 见 src/user/libc/unistd.h:77 */
int chdir(const char *path);
/* 见 src/user/libc/unistd.h:78 */
int unlink(const char *path);
/* 见 src/user/libc/unistd.h:79 */
int symlink(const char *target, const char *linkpath);
/* 见 src/user/libc/unistd.h:80 */
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
/* 见 src/user/libc/unistd.h:82 */
ssize_t read(int fd, void *buf, size_t count);
/* 见 src/user/libc/unistd.h:83 */
ssize_t write(int fd, const void *buf, size_t count);
/* 见 src/user/libc/unistd.h:84 */
off_t lseek(int fd, off_t offset, int whence);
/* 见 src/user/libc/unistd.h:85 */
int ftruncate(int fd, off_t length);
/* 见 src/user/libc/unistd.h:87 */
int rmdir(const char *path);
/* 见 src/user/libc/unistd.h:88 */
int pipe2(int pipefd[2], int flags);
/* * task is replaced/terminated by the kernel), -1 on failure.  ── 见 src/user/libc/unistd.h:92 */
int execve(const char *path, char *const argv[], char *const envp[]);
/* 见 src/user/libc/unistd.h:93 */
int execvp(const char *file, char *const argv[]);
/* 见 src/user/libc/unistd.h:95 */
int access(const char *path, int mode);
/* 见 src/user/libc/unistd.h:96 */
int isatty(int fd);
/* * ext2/FAT32 currently use the filesystem copy/delete fallback.  ── 见 src/user/libc/unistd.h:100 */
int rename(const char *oldpath, const char *newpath);
/* * kernel's native syscall table.  ── 见 src/user/libc/unistd.h:111 */
uid_t getuid(void);
/* 见 src/user/libc/unistd.h:112 */
uid_t geteuid(void);
/* 见 src/user/libc/unistd.h:113 */
gid_t getgid(void);
/* 见 src/user/libc/unistd.h:114 */
gid_t getegid(void);
/* 见 src/user/libc/unistd.h:115 */
int setuid(uid_t uid);
/* 见 src/user/libc/unistd.h:116 */
int setgid(gid_t gid);
/* 见 src/user/libc/unistd.h:117 */
int getgroups(int size, gid_t list[]);
/* 见 src/user/libc/unistd.h:118 */
int setgroups(int size, const gid_t list[]);
/* 见 src/user/libc/unistd.h:119 */
pid_t getpgid(pid_t pid);
/* 见 src/user/libc/unistd.h:121 */
int hax_app_exists(const char *name);

#elif defined(HBOS_ALL_WITH_NET) && HBOS_ALL_WITH_NET
/* ══ 安全网络配置侧（lwip / mbedtls）═════════════════════ */

/* ── src/net/arch/cc.h ──────────────────────────────── */
/* 真实声明位于 src/net/arch/cc.h */
#include "net/arch/cc.h"

/* 见 src/net/arch/cc.h:13 */
void hbos_lwip_assert(const char *message);
/* 见 src/net/arch/cc.h:16 */
uint32_t hbos_lwip_rand(void);

/* ── src/net/lwipopts.h ──────────────────────────────── */
/* 真实声明位于 src/net/lwipopts.h */
#include "net/lwipopts.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/net/lwipopts.h */

/* ── src/net/mbedtls_config.h ──────────────────────────────── */
/* 真实声明位于 src/net/mbedtls_config.h */
#include "net/mbedtls_config.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/net/mbedtls_config.h */

/* ── src/net/mbedtls_port.h ──────────────────────────────── */
/* 真实声明位于 src/net/mbedtls_port.h */
#include "net/mbedtls_port.h"

/* 见 src/net/mbedtls_port.h:7 */
void *hbos_mbedtls_calloc(size_t count, size_t size);
/* 见 src/net/mbedtls_port.h:8 */
void hbos_mbedtls_free(void *pointer);
/* 见 src/net/mbedtls_port.h:9 */
int64_t hbos_mbedtls_time(int64_t *result);
/* 见 src/net/mbedtls_port.h:10 */
int hbos_mbedtls_snprintf(char *buffer, size_t size, const char *format, ...);

/* ── src/net/mbedtls_shim/arpa/inet.h ──────────────────────────────── */
/* 真实声明位于 src/net/mbedtls_shim/arpa/inet.h */
#include "net/mbedtls_shim/arpa/inet.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/net/mbedtls_shim/arpa/inet.h */

/* ── src/net/mbedtls_shim/sys/socket.h ──────────────────────────────── */
/* 真实声明位于 src/net/mbedtls_shim/sys/socket.h */
#include "net/mbedtls_shim/sys/socket.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/net/mbedtls_shim/sys/socket.h */

/* ── src/net/psa_crypto_config.h ──────────────────────────────── */
/* 真实声明位于 src/net/psa_crypto_config.h */
#include "net/psa_crypto_config.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/net/psa_crypto_config.h */

#else
/* ══ 内核侧（默认）═══════════════════════════════════════ */

/* ── src/types.h ──────────────────────────────── */
/* 真实声明位于 src/types.h */
#include "types.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/types.h */

/* ── src/flanterm_private.h ──────────────────────────────── */
/* 真实声明位于 src/flanterm_private.h */
#ifndef FLANTERM_IN_FLANTERM
#  define FLANTERM_IN_FLANTERM 1
#  define HBOS_ALL_LOCAL_FLANTERM_IN_FLANTERM 1
#endif
#include "flanterm_private.h"
#ifdef HBOS_ALL_LOCAL_FLANTERM_IN_FLANTERM
#  undef FLANTERM_IN_FLANTERM
#  undef HBOS_ALL_LOCAL_FLANTERM_IN_FLANTERM
#endif

/* 见 src/flanterm_private.h:132 */
void flanterm_context_reinit(struct flanterm_context *ctx);

/* ── src/fb_private.h ──────────────────────────────── */
/* 真实声明位于 src/fb_private.h */
#ifndef FLANTERM_IN_FLANTERM
#  define FLANTERM_IN_FLANTERM 1
#  define HBOS_ALL_LOCAL_FLANTERM_IN_FLANTERM 1
#endif
#include "fb_private.h"
#ifdef HBOS_ALL_LOCAL_FLANTERM_IN_FLANTERM
#  undef FLANTERM_IN_FLANTERM
#  undef HBOS_ALL_LOCAL_FLANTERM_IN_FLANTERM
#endif

/* 该头只有类型/宏，无函数声明  ── 见 src/fb_private.h */

/* ── src/ac97.h ──────────────────────────────── */
/* 真实声明位于 src/ac97.h */
#include "ac97.h"

/* *  codec + bus master 寄存器，成功返回 0。找不到设备返回 -1。  ── 见 src/ac97.h:24 */
int ac97_init(void);
/* 见 src/ac97.h:27 */
int ac97_present(void);
/* 见 src/ac97.h:43 */
int ac97_play_pcm16(const int16_t *samples, uint32_t frame_count, int channels);

/* ── src/acpi.h ──────────────────────────────── */
/* 真实声明位于 src/acpi.h */
#include "acpi.h"

/* 见 src/acpi.h:48 */
void acpi_init(void *mbi);
/* 见 src/acpi.h:51 */
int acpi_poweroff(void);
/* 见 src/acpi.h:54 */
const acpi_madt_info_t *acpi_get_madt(void);

/* ── src/ahci.h ──────────────────────────────── */
/* 真实声明位于 src/ahci.h */
#include "ahci.h"

/* 见 src/ahci.h:55 */
int ahci_init(void);
/* 见 src/ahci.h:63 */
int ahci_read_sector(uint32_t lba, uint8_t *buffer);
/* 见 src/ahci.h:66 */
int ahci_read_sectors(uint32_t lba, uint8_t *buffer, uint32_t count);
/* 见 src/ahci.h:74 */
int ahci_write_sector(uint32_t lba, const uint8_t *buffer);
/* 见 src/ahci.h:77 */
int ahci_write_sectors(uint32_t lba, const uint8_t *buffer, uint32_t count);
/* 见 src/ahci.h:83 */
uint32_t ahci_sector_count(void);
/* 见 src/ahci.h:89 */
const char *ahci_model(void);
/* 见 src/ahci.h:95 */
int ahci_present(void);
/* 见 src/ahci.h:98 */
void ahci_get_stats(ahci_stats_t *out);
/* 见 src/ahci.h:101 */
const char *ahci_last_error(void);

/* ── src/app/api/gui_service.h ──────────────────────────────── */
/* 真实声明位于 src/app/api/gui_service.h */
#include "app/api/gui_service.h"

/* 见 src/app/api/gui_service.h:192 */
int gui_service_canvas_info(int *w, int *h);
/* 见 src/app/api/gui_service.h:193 */
void gui_service_canvas_clear(uint32_t color);
/* 见 src/app/api/gui_service.h:194 */
void gui_service_canvas_rect(int x, int y, int w, int h, uint32_t color);
/* 见 src/app/api/gui_service.h:195 */
void gui_service_canvas_text(int x, int y, const char *s, uint32_t color, int scale);
/* 见 src/app/api/gui_service.h:196 */
void gui_service_canvas_present(void);
/* 见 src/app/api/gui_service.h:197 */
int gui_service_canvas_pollkey(void);
/* 见 src/app/api/gui_service.h:198 */
int gui_service_canvas_pollmouse(int *x, int *y);
/* 见 src/app/api/gui_service.h:200 */
int gui_service_window_open(uint32_t owner_task, const char *title, int w, int h);
/* 见 src/app/api/gui_service.h:201 */
int gui_service_window_info(uint32_t owner_task, int *w, int *h);
/* 见 src/app/api/gui_service.h:202 */
void gui_service_window_clear(uint32_t owner_task, uint32_t color);
/* 见 src/app/api/gui_service.h:203 */
void gui_service_window_fill(uint32_t owner_task, int x, int y, int w, int h, uint32_t color);
/* 见 src/app/api/gui_service.h:205 */
void gui_service_window_text(uint32_t owner_task, int x, int y, const char *s, uint32_t color);
/* 见 src/app/api/gui_service.h:207 */
void gui_service_window_blit(uint32_t owner_task, int x, int y, int w, int h, const uint32_t *pixels, int stride);
/* 见 src/app/api/gui_service.h:209 */
void gui_service_window_present(uint32_t owner_task);
/* 见 src/app/api/gui_service.h:210 */
int gui_service_window_poll(uint32_t owner_task, int *event4);
/* 见 src/app/api/gui_service.h:211 */
void gui_service_window_close(uint32_t owner_task);
/* 见 src/app/api/gui_service.h:214 */
int gui_service_window_v2(uint32_t owner_task, uint32_t operation, int handle, void *data);
/* 见 src/app/api/gui_service.h:216 */
int gui_service_settings(uint32_t owner_task, uint32_t operation, gui_service_settings_t *settings);
/* * 从不通知 service，跟随 service 的 HAX 应用窗口就会和桌面主题脱节。  ── 见 src/app/api/gui_service.h:221 */
int gui_service_theme_set(int light);

/* ── src/app/api/hal.h ──────────────────────────────── */
/* 真实声明位于 src/app/api/hal.h */
#include "app/api/hal.h"

/* 见 src/app/api/hal.h:41 */
void console_puts(const char *str);
/* 见 src/app/api/hal.h:42 */
void console_write(const char *str, uint64_t len);
/* 见 src/app/api/hal.h:43 */
void console_putchar(char c);
/* 见 src/app/api/hal.h:44 */
void console_flush(void);
/* 见 src/app/api/hal.h:45 */
void console_clear(void);
/* 见 src/app/api/hal.h:46 */
void console_set_fg(uint32_t color);
/* 见 src/app/api/hal.h:47 */
void console_set_bg(uint32_t color);
/* 见 src/app/api/hal.h:48 */
void console_get_size(uint64_t *cols, uint64_t *rows);
/* 见 src/app/api/hal.h:49 */
bool console_is_initialized(void);
/* 见 src/app/api/hal.h:51 */
int kb_get_key(void);
/* 见 src/app/api/hal.h:58 */
void sys_reboot(void);
/* 见 src/app/api/hal.h:59 */
void sys_poweroff(void);
/* 见 src/app/api/hal.h:60 */
void sys_udelay(uint64_t us);
/* 见 src/app/api/hal.h:61 */
void sys_mdelay(uint64_t ms);
/* 见 src/app/api/hal.h:63 */
void app_register_command(const char *name, const char *desc, void (*handler)(int argc, char **argv));
/* 见 src/app/api/hal.h:65 */
void app_main(void);

/* ── src/ata.h ──────────────────────────────── */
/* 真实声明位于 src/ata.h */
#include "ata.h"

/* 探测全部 4 个槽位  ── 见 src/ata.h:86 */
int ata_init(void);
/* 获取首个可用的 LBA28 硬盘  ── 见 src/ata.h:87 */
const ata_device_t *ata_primary(void);
/* 已识别的硬盘数  ── 见 src/ata.h:88 */
int ata_drive_count(void);
/* 按序号取设备信息  ── 见 src/ata.h:89 */
const ata_device_t *ata_drive(int idx);
/* 等待控制器就绪（BSY清零，主通道）  ── 见 src/ata.h:90 */
int ata_wait(void);
/* 兼容旧接口：探测主通道主盘  ── 见 src/ata.h:91 */
int ata_identify(ata_device_t *out);
/* 读取指定LBA扇区到缓冲区  ── 见 src/ata.h:92 */
int ata_read_sector(uint32_t lba, uint8_t *buffer);
/* 将缓冲区数据写入指定LBA扇区  ── 见 src/ata.h:93 */
int ata_write_sector(uint32_t lba, const uint8_t *buffer);

/* ── src/block.h ──────────────────────────────── */
/* 真实声明位于 src/block.h */
#include "block.h"

/* 见 src/block.h:45 */
int block_init(void);
/* 见 src/block.h:48 */
int block_read_sector(uint32_t lba, uint8_t *buffer);
/* 见 src/block.h:51 */
int block_read_sectors(uint32_t lba, uint8_t *buffer, uint32_t count);
/* 见 src/block.h:54 */
int block_write_sector(uint32_t lba, const uint8_t *buffer);
/* 见 src/block.h:57 */
int block_write_sectors(uint32_t lba, const uint8_t *buffer, uint32_t count);
/* 见 src/block.h:60 */
uint32_t block_sector_count(void);
/* 见 src/block.h:63 */
block_backend_t block_backend(void);
/* 见 src/block.h:66 */
const char *block_backend_name(void);

/* ── src/bmp.h ──────────────────────────────── */
/* 真实声明位于 src/bmp.h */
#include "bmp.h"

/* 见 src/bmp.h:34 */
int bmp_decode(const uint8_t *data, uint32_t size, uint8_t *out_rgb, uint32_t out_cap, int max_w, int max_h, int *out_w, int *out_h);

/* ── src/core/cpu.h ──────────────────────────────── */
/* 真实声明位于 src/core/cpu.h */
#include "core/cpu.h"

/* inline 定义 wrmsr  ── 见 src/core/cpu.h:319 */
/* inline 定义 rdmsr  ── 见 src/core/cpu.h:324 */
/* inline 定义 invlpg  ── 见 src/core/cpu.h:331 */
/* inline 定义 read_cr2  ── 见 src/core/cpu.h:336 */
/* inline 定义 read_cr3  ── 见 src/core/cpu.h:343 */
/* inline 定义 write_cr3  ── 见 src/core/cpu.h:350 */
/* inline 定义 fpu_enable — * 窄范围风险，而不是这里要解决的问题。  ── 见 src/core/cpu.h:364 */
/* inline 定义 pat_init  ── 见 src/core/cpu.h:396 */
/* inline 定义 cli  ── 见 src/core/cpu.h:404 */
/* inline 定义 sti  ── 见 src/core/cpu.h:407 */
/* inline 定义 hlt  ── 见 src/core/cpu.h:410 */
/* 见 src/core/cpu.h:416 */
void gdt_init(void);
/* 见 src/core/cpu.h:417 */
void gdt_idt_init(void);
/* 见 src/core/cpu.h:418 */
void tss_set_stack(uint64_t rsp0);
/* *  否则 A 核的切换会把 B 核的 ring0 栈顶改掉。  ── 见 src/core/cpu.h:421 */
void tss_set_stack_cpu(int cpu, uint64_t rsp0);
/* * 只读的），只做"加载到这个核心"这一步。  ── 见 src/core/cpu.h:426 */
void gdt_idt_load_ap(int cpu_idx, uint64_t rsp0);
/* 见 src/core/cpu.h:427 */
void int_enable(void);
/* 见 src/core/cpu.h:428 */
void int_disable(void);
/* 见 src/core/cpu.h:429 */
bool int_get_state(void);
/* 见 src/core/cpu.h:432 */
uint64_t pit_get_ticks(void);
/* 见 src/core/cpu.h:434 */
uint32_t pit_get_frequency_hz(void);
/* 见 src/core/cpu.h:436 */
uint64_t pit_ticks_from_ms(uint32_t ms);

/* ── src/core/heap.h ──────────────────────────────── */
/* 真实声明位于 src/core/heap.h */
#include "core/heap.h"

/* Initialize kernel heap  ── 见 src/core/heap.h:31 */
void heap_init(void);
/* Allocate memory on kernel heap  ── 见 src/core/heap.h:34 */
void *kmalloc(size_t size);
/* Allocate and zero memory  ── 见 src/core/heap.h:37 */
void *kcalloc(size_t num, size_t size);
/* Free allocated memory  ── 见 src/core/heap.h:40 */
void kfree(void *ptr);
/* Resize allocated memory  ── 见 src/core/heap.h:43 */
void *krealloc(void *ptr, size_t new_size);
/* Allocate aligned memory on kernel heap  ── 见 src/core/heap.h:46 */
void *kmalloc_aligned(size_t size, size_t align);

/* ── src/core/io.h ──────────────────────────────── */
/* 真实声明位于 src/core/io.h */
#include "core/io.h"

/* inline 定义 io_out8  ── 见 src/core/io.h:31 */
/* inline 定义 io_out16  ── 见 src/core/io.h:35 */
/* inline 定义 io_out32  ── 见 src/core/io.h:39 */
/* inline 定义 io_in8  ── 见 src/core/io.h:43 */
/* inline 定义 io_in16  ── 见 src/core/io.h:49 */
/* inline 定义 io_in32  ── 见 src/core/io.h:55 */
/* inline 定义 io_delay  ── 见 src/core/io.h:62 */
/* inline 定义 cpu_relax  ── 见 src/core/io.h:67 */
/* inline 定义 cpu_rdtsc  ── 见 src/core/io.h:72 */

/* ── src/core/pmm.h ──────────────────────────────── */
/* 真实声明位于 src/core/pmm.h */
#include "core/pmm.h"

/* (at)mbi: pointer to Multiboot2 info structure  ── 见 src/core/pmm.h:54 */
void pmm_init(void *mbi);
/* Returns physical address, or 0 on OOM  ── 见 src/core/pmm.h:58 */
uint64_t pmm_alloc_page(void);
/* Free a physical page previously returned by pmm_alloc_page  ── 见 src/core/pmm.h:61 */
void pmm_free_page(uint64_t phys_addr);
/* 见 src/core/pmm.h:65 */
uint64_t pmm_query_max_phys(void);
/* 见 src/core/pmm.h:66 */
void pmm_enable_high_memory(uint64_t limit);
/* bitmap only when its last reference is released.  ── 见 src/core/pmm.h:70 */
int pmm_retain_page(uint64_t phys_addr);
/* 见 src/core/pmm.h:71 */
uint16_t pmm_page_refcount(uint64_t phys_addr);
/* Returns physical base address  ── 见 src/core/pmm.h:75 */
uint64_t pmm_alloc_blocks(size_t count);
/* Free a contiguous region  ── 见 src/core/pmm.h:78 */
void pmm_free_blocks(uint64_t phys_addr, size_t count);
/* Get total / free physical memory in bytes  ── 见 src/core/pmm.h:81 */
uint64_t pmm_get_total_mem(void);
/* 见 src/core/pmm.h:82 */
uint64_t pmm_get_free_mem(void);
/* Mark a physical region as used (reserved by bootloader/hardware)  ── 见 src/core/pmm.h:85 */
void pmm_reserve_region(uint64_t base, uint64_t length);

/* ── src/core/sched_core.h ──────────────────────────────── */
/* 真实声明位于 src/core/sched_core.h */
#include "core/sched_core.h"

/* 见 src/core/sched_core.h:54 */
void schedq_init(sched_queue_t *q, uint32_t age_quantum);
/* 见 src/core/sched_core.h:57 */
int schedq_push(sched_queue_t *q, sched_node_t *n, uint8_t level);
/* 见 src/core/sched_core.h:60 */
int schedq_remove(sched_queue_t *q, sched_node_t *n);
/* 见 src/core/sched_core.h:63 */
sched_node_t *schedq_pick(sched_queue_t *q);
/* 见 src/core/sched_core.h:65 */
uint32_t schedq_count(const sched_queue_t *q);
/* 见 src/core/sched_core.h:66 */
int schedq_empty(const sched_queue_t *q);

/* ── src/core/task.h ──────────────────────────────── */
/* 真实声明位于 src/core/task.h */
#include "core/task.h"

/* Initialize the task scheduler  ── 见 src/core/task.h:319 */
void task_init(void);
/* Returns task ID, or -1 on failure  ── 见 src/core/task.h:323 */
int task_create(const char *name, void (*entry)(void *), void *arg);
/* Returns task ID, or -1 on failure  ── 见 src/core/task.h:329 */
int task_create_ring3(const char *name, uint64_t user_entry, uint64_t user_stack);
/* 见 src/core/task.h:330 */
int task_create_ring3_as(const char *name, uint64_t user_entry, uint64_t user_stack, uint64_t pml4_phys);
/* 见 src/core/task.h:332 */
int task_create_ring3_full(const char *name, uint64_t user_entry, uint64_t user_stack, uint64_t user_argc, uint64_t user_argv, uint64_t pml4_phys);
/* * HBOS 的 exec 是"建新任务 + 老任务退出"，没有这一步新进程会丢掉全部 fd。  ── 见 src/core/task.h:341 */
void task_exec_inherit(int new_id);
/* Cooperative yield — switch to next ready task  ── 见 src/core/task.h:344 */
void task_yield(void);
/* Terminate the current task  ── 见 src/core/task.h:347 */
void task_exit(void);
/* 见 src/core/task.h:348 */
void task_set_exit_status(int status);
/* 见 src/core/task.h:349 */
int task_wait(uint32_t id, int *status);
/* 见 src/core/task.h:350 */
int task_kill(uint32_t id, int sig);
/* 向进程组发信号；返回送达数  ── 见 src/core/task.h:351 */
int task_kill_pgrp(uint32_t pgid, int sig);
/* 0/-errno  ── 见 src/core/task.h:352 */
int task_setpgid(uint32_t pid, uint32_t pgid);
/* 新 sid 或 -errno  ── 见 src/core/task.h:353 */
int task_setsid(void);
/* pgid 或 -ESRCH  ── 见 src/core/task.h:354 */
int task_getpgid(uint32_t pid);
/* int task_getpgid(uint32_t pid);                       pgid 或 -ESRCH  ── 见 src/core/task.h:355 */
int task_getsid(uint32_t pid);
/* Fork current task (clone with same context)  ── 见 src/core/task.h:358 */
int task_fork(void);
/* 见 src/core/task.h:359 */
int task_fork_linux(const hbos_linux_clone_context_t *context);
/* 见 src/core/task.h:360 */
int task_clone_user_thread(const hbos_clone_request_t *request);
/* 见 src/core/task.h:361 */
int task_clone_linux_thread(const hbos_clone_request_t *request, const hbos_linux_clone_context_t *context);
/* 见 src/core/task.h:363 */
int task_set_robust_list(void *head, size_t length);
/* 见 src/core/task.h:364 */
int task_get_robust_list(int tid, void **head, size_t *length);
/* 见 src/core/task.h:365 */
int task_set_tid_address(uint32_t *address);
/* Get current task ID  ── 见 src/core/task.h:368 */
uint32_t task_get_id(void);
/* 见 src/core/task.h:369 */
uint32_t task_get_process_id(void);
/* 见 src/core/task.h:370 */
uint64_t task_get_fs_base(void);
/* 见 src/core/task.h:371 */
int task_set_fs_base(uint64_t base);
/* Get current task object  ── 见 src/core/task.h:374 */
task_t *task_current(void);
/* 见 src/core/task.h:375 */
const task_t *task_get_by_id(uint32_t id);
/* 见 src/core/task.h:377 */
task_t *task_lookup(uint32_t id);
/* symbols back into it — see src/user/ldso.c.  ── 见 src/core/task.h:382 */
void task_set_host_symtab(uint32_t id, void *symtab, char *strtab, uint64_t count);
/* 见 src/core/task.h:384 */
void task_set_exe_path(uint32_t id, const char *path);
/* Get number of active (non-terminated) tasks  ── 见 src/core/task.h:387 */
int task_get_count(void);
/* 见 src/core/task.h:390 */
void task_set_priority(task_t *t, uint8_t prio);
/* 见 src/core/task.h:391 */
uint8_t task_get_priority(const task_t *t);
/* 见 src/core/task.h:394 */
void task_set_dependency(task_t *t, uint32_t dep_id);
/* 见 src/core/task.h:396 */
void task_set_parallel_width(task_t *t, uint8_t width);
/* 见 src/core/task.h:399 */
void task_set_affinity_any(task_t *t, int any);
/* 见 src/core/task.h:401 */
int task_get_cpu(const task_t *t);
/* 见 src/core/task.h:403 */
int task_ready_count_on_cpu(int cpu);
/* 见 src/core/task.h:405 */
const task_t *task_current_on_cpu(int cpu);
/* 见 src/core/task.h:408 */
void task_ap_enter(int cpu);
/* 见 src/core/task.h:410 */
int task_create_idle(int cpu);
/* 见 src/core/task.h:412 */
void task_smp_release_aps(void);
/* Get active task by visible index, or NULL when out of range  ── 见 src/core/task.h:415 */
const task_t *task_get_active(uint32_t index);
/* List all tasks (for debug)  ── 见 src/core/task.h:418 */
void task_list_all(void);
/* Preemptive scheduling  ── 见 src/core/task.h:421 */
void task_schedule(void);
/* 见 src/core/task.h:422 */
void task_preempt_enable(void);
/* 见 src/core/task.h:423 */
void task_preempt_disable(void);
/* 见 src/core/task.h:424 */
void pit_init(uint32_t freq_hz);

/* ── src/core/vmm.h ──────────────────────────────── */
/* 真实声明位于 src/core/vmm.h */
#include "core/vmm.h"

/* (at)p4_phys: physical address of the current PML4 table  ── 见 src/core/vmm.h:95 */
void vmm_init(uint64_t p4_phys);
/* Returns 0 on success, -1 on failure (e.g. OOM)  ── 见 src/core/vmm.h:99 */
int vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
/* Unmap a virtual page  ── 见 src/core/vmm.h:102 */
void vmm_unmap_page(uint64_t virt_addr);
/* Unmap a page and return its physical frame when the PTE owns it.  ── 见 src/core/vmm.h:105 */
void vmm_release_page(uint64_t virt_addr);
/* Get physical address mapped at a virtual address, or 0 if not mapped  ── 见 src/core/vmm.h:108 */
uint64_t vmm_get_phys(uint64_t virt_addr);
/* 见 src/core/vmm.h:111 */
int vmm_protect_page(uint64_t virt_addr, uint64_t flags);
/* 见 src/core/vmm.h:114 */
int vmm_handle_cow_fault(uint64_t virt_addr);
/* 见 src/core/vmm.h:117 */
int vmm_make_page_private(uint64_t virt_addr);
/* 见 src/core/vmm.h:120 */
uint64_t vmm_get_page_flags(uint64_t virt_addr);
/* 见 src/core/vmm.h:123 */
void vmm_set_nx_enabled(bool enabled);
/* *  4 GiB 以上的 RAM 靠它才能被内核访问；中间的 PD/PDPT/PML4 表按需分配。  ── 见 src/core/vmm.h:127 */
int vmm_identity_map_2m(uint64_t start, uint64_t end, uint64_t flags);
/* Returns virtual address on success, 0 on OOM  ── 见 src/core/vmm.h:131 */
uint64_t vmm_alloc_page_at(uint64_t virt_addr, uint64_t flags);
/* Get current PML4 physical address  ── 见 src/core/vmm.h:134 */
uint64_t vmm_get_pml4(void);
/* 见 src/core/vmm.h:137 */
uint64_t vmm_create_address_space(void);
/* 见 src/core/vmm.h:140 */
uint64_t vmm_clone_address_space(uint64_t src_pml4);
/* 见 src/core/vmm.h:143 */
void vmm_destroy_address_space(uint64_t pml4_phys);
/* 见 src/core/vmm.h:146 */
void vmm_set_pml4(uint64_t pml4_phys);
/* 见 src/core/vmm.h:149 */
void *vmm_map_mmio(uint64_t phys_addr, size_t size);
/* 见 src/core/vmm.h:152 */
uint64_t vmm_virt_to_phys(uint64_t virt_addr);
/* 见 src/core/vmm.h:161 */
int vmm_set_range_wc(uint64_t phys_addr, uint64_t size);

/* ── src/core/wait.h ──────────────────────────────── */
/* 真实声明位于 src/core/wait.h */
#include "core/wait.h"

/* inline 定义 pit_deadline_after_ms  ── 见 src/core/wait.h:34 */
/* inline 定义 pit_deadline_reached  ── 见 src/core/wait.h:40 */
/* inline 定义 hw_deadline_start  ── 见 src/core/wait.h:44 */
/* inline 定义 hw_deadline_expired_ticks  ── 见 src/core/wait.h:50 */
/* inline 定义 hw_deadline_expired_ms  ── 见 src/core/wait.h:66 */

/* ── src/crypto/aes_gcm.h ──────────────────────────────── */
/* 真实声明位于 src/crypto/aes_gcm.h */
#include "crypto/aes_gcm.h"

/* 见 src/crypto/aes_gcm.h:35 */
void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
/* 见 src/crypto/aes_gcm.h:49 */
void aes128_gcm_seal(const uint8_t key[16], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len, const uint8_t *plain, size_t plain_len, uint8_t *cipher, uint8_t tag[16]);
/* 见 src/crypto/aes_gcm.h:67 */
int aes128_gcm_open(const uint8_t key[16], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len, const uint8_t *cipher, size_t cipher_len, const uint8_t tag[16], uint8_t *plain);

/* ── src/crypto/chacha20_poly1305.h ──────────────────────────────── */
/* 真实声明位于 src/crypto/chacha20_poly1305.h */
#include "crypto/chacha20_poly1305.h"

/* 见 src/crypto/chacha20_poly1305.h:33 */
void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]);
/* 见 src/crypto/chacha20_poly1305.h:45 */
void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t *in, uint8_t *out, size_t len);
/* 见 src/crypto/chacha20_poly1305.h:56 */
void poly1305_mac(const uint8_t *msg, size_t len, const uint8_t key[32], uint8_t tag[16]);
/* 见 src/crypto/chacha20_poly1305.h:71 */
int chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len, const uint8_t *cipher, size_t cipher_len, const uint8_t tag[16], uint8_t *plain);
/* 见 src/crypto/chacha20_poly1305.h:88 */
void chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len, const uint8_t *plain, size_t plain_len, uint8_t *cipher, uint8_t tag[16]);

/* ── src/crypto/p256.h ──────────────────────────────── */
/* 真实声明位于 src/crypto/p256.h */
#include "crypto/p256.h"

/* 见 src/crypto/p256.h:31 */
void p256_public_key(uint8_t out_xy[64], const uint8_t priv[32]);
/* *  peer 非法（不在曲线上/无穷远）时 out_x 全 0，返回 -1。  ── 见 src/crypto/p256.h:35 */
int p256_ecdh(uint8_t out_x[32], const uint8_t priv[32], const uint8_t peer_xy[64]);

/* ── src/crypto/sha256.h ──────────────────────────────── */
/* 真实声明位于 src/crypto/sha256.h */
#include "crypto/sha256.h"

/* 见 src/crypto/sha256.h:49 */
void sha256_init(sha256_ctx_t *ctx);
/* 见 src/crypto/sha256.h:58 */
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len);
/* 见 src/crypto/sha256.h:66 */
void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_DIGEST_SIZE]);
/* 见 src/crypto/sha256.h:77 */
void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t out[SHA256_DIGEST_SIZE]);
/* 见 src/crypto/sha256.h:92 */
int hkdf_sha256_extract(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, uint8_t prk[SHA256_DIGEST_SIZE]);
/* 见 src/crypto/sha256.h:107 */
int hkdf_sha256_expand(const uint8_t prk[SHA256_DIGEST_SIZE], const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len);

/* ── src/crypto/x25519.h ──────────────────────────────── */
/* 真实声明位于 src/crypto/x25519.h */
#include "crypto/x25519.h"

/* 见 src/crypto/x25519.h:21 */
void x25519_public_key(uint8_t out[32], const uint8_t scalar[32]);
/* 见 src/crypto/x25519.h:22 */
void x25519_shared_secret(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);

/* ── src/devfs.h ──────────────────────────────── */
/* 真实声明位于 src/devfs.h */
#include "devfs.h"

/* 见 src/devfs.h:39 */
void devfs_init(void);
/* 见 src/devfs.h:42 */
vfs_node_t *devfs_lookup(const char *path);
/* 见 src/devfs.h:45 */
int devfs_readdir(const char *path, uint32_t index, char *name, uint32_t *type);
/* 见 src/devfs.h:48 */
void devfs_register_nodes(void);

/* ── src/elf.h ──────────────────────────────── */
/* 真实声明位于 src/elf.h */
#include "elf.h"

/* 见 src/elf.h:170 */
int elf64_load_and_spawn(const uint8_t *data, size_t size, char *const argv[], char *const envp[], const char *task_name);
/* 见 src/elf.h:183 */
int elf64_load_and_exec(const uint8_t *data, size_t size, char *const argv[], char *const envp[]);
/* *  否则 /proc/<pid>/exe 只能拿到 basename，dirname() 的结果不可用。  ── 见 src/elf.h:190 */
int elf64_load_vfs_and_exec(struct vfs_node *node, char *const argv[], char *const envp[]);
/* 见 src/elf.h:194 */
void elf_set_exec_path(const char *path);
/* 见 src/elf.h:197 */
int elf64_load_vfs_and_spawn(struct vfs_node *node, char *const argv[], char *const envp[], const char *task_name);
/* 见 src/elf.h:201 */
const char *elf64_last_error(void);

/* ── src/errno.h ──────────────────────────────── */
/* 真实声明位于 src/errno.h */
#include "errno.h"

/* 见 src/errno.h:144 */
int *__errno_location(void);

/* ── src/exfat.h ──────────────────────────────── */
/* 真实声明位于 src/exfat.h */
#include "exfat.h"

/* 见 src/exfat.h:61 */
int exfat_mount(exfat_fs_t *fs, exfat_read_fn rd, void *ctx, uint64_t volume_lba);
/* 见 src/exfat.h:64 */
int exfat_list_root(exfat_fs_t *fs, exfat_dirent_t *out, int max);
/* 见 src/exfat.h:67 */
int exfat_list_dir(exfat_fs_t *fs, uint32_t first_cluster, exfat_dirent_t *out, int max);
/* 见 src/exfat.h:74 */
int exfat_iter_dir(exfat_fs_t *fs, uint32_t first_cluster, exfat_dir_cb cb, void *ctx);
/* 见 src/exfat.h:78 */
int exfat_read_file(exfat_fs_t *fs, uint32_t first_cluster, uint64_t size, uint64_t offset, void *buf, uint32_t len);
/* 见 src/exfat.h:87 */
int exfat_create_file(exfat_fs_t *fs, const char *name, const void *data, uint32_t size);
/* 见 src/exfat.h:95 */
int exfat_write_file(exfat_fs_t *fs, const char *name, uint64_t offset, const void *data, uint32_t len);
/* 见 src/exfat.h:99 */
int exfat_create_in_dir(exfat_fs_t *fs, uint32_t dir_cluster, const char *name, const void *data, uint32_t size);
/* 见 src/exfat.h:103 */
int exfat_write_in_dir(exfat_fs_t *fs, uint32_t dir_cluster, const char *name, uint64_t offset, const void *data, uint32_t len);
/* 见 src/exfat.h:107 */
int exfat_truncate_in_dir(exfat_fs_t *fs, uint32_t dir_cluster, const char *name);
/* 见 src/exfat.h:110 */
int exfat_unlink_in_dir(exfat_fs_t *fs, uint32_t dir_cluster, const char *name);
/* 见 src/exfat.h:113 */
void exfat_volume_label(exfat_fs_t *fs, char *out, int cap);

/* ── src/ext2.h ──────────────────────────────── */
/* 真实声明位于 src/ext2.h */
#include "ext2.h"

/* Read  ── 见 src/ext2.h:182 */
int ext2_mount(uint32_t partition_lba, ext2_fs_t *fs);
/* 见 src/ext2.h:184 */
const char *ext2_last_error(void);
/* 见 src/ext2.h:185 */
int ext2_read_inode(ext2_fs_t *fs, uint32_t inode_num, ext2_inode_t *inode);
/* 见 src/ext2.h:186 */
int ext2_read_block(ext2_fs_t *fs, uint32_t block, uint8_t *buf);
/* 见 src/ext2.h:187 */
int ext2_read_file(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t offset, uint8_t *buf, uint32_t count);
/* 见 src/ext2.h:189 */
int ext2_readdir(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t index, char *name, uint32_t *type);
/* 见 src/ext2.h:197 */
int ext2_iter_dir(ext2_fs_t *fs, ext2_inode_t *dir, ext2_dir_cb cb, void *ctx);
/* 见 src/ext2.h:198 */
int ext2_lookup(ext2_fs_t *fs, ext2_inode_t *dir_inode, const char *name, uint32_t *out_inode);
/* 见 src/ext2.h:200 */
int ext2_path_to_inode(ext2_fs_t *fs, const char *path, uint32_t *out_inode);
/* Write  ── 见 src/ext2.h:203 */
int ext2_write_block(ext2_fs_t *fs, uint32_t block, const uint8_t *buf);
/* 见 src/ext2.h:204 */
int ext2_write_inode(ext2_fs_t *fs, uint32_t inode_num, const ext2_inode_t *inode);
/* 见 src/ext2.h:205 */
uint32_t ext2_alloc_block(ext2_fs_t *fs);
/* 见 src/ext2.h:206 */
int ext2_free_block(ext2_fs_t *fs, uint32_t block);
/* 见 src/ext2.h:207 */
uint32_t ext2_alloc_inode(ext2_fs_t *fs);
/* 见 src/ext2.h:208 */
int ext2_free_inode(ext2_fs_t *fs, uint32_t inode_num);
/* 见 src/ext2.h:209 */
int ext2_write_file(ext2_fs_t *fs, uint32_t inode_num, uint32_t offset, const uint8_t *buf, uint32_t count);
/* 见 src/ext2.h:211 */
int ext2_create_file(ext2_fs_t *fs, uint32_t dir_ino, const char *name, uint8_t file_type);
/* *  返回新建 inode 号，失败返回 -1。  ── 见 src/ext2.h:214 */
int ext2_symlink(ext2_fs_t *fs, uint32_t dir_ino, const char *name, const char *target);
/* 见 src/ext2.h:217 */
int ext2_readlink(ext2_fs_t *fs, const ext2_inode_t *inode, char *buf, uint32_t capacity);
/* 见 src/ext2.h:219 */
int ext2_unlink(ext2_fs_t *fs, uint32_t dir_ino, const char *name);
/* 见 src/ext2.h:220 */
int ext2_truncate(ext2_fs_t *fs, uint32_t inode_num, uint32_t new_size);
/* 见 src/ext2.h:221 */
int ext2_mkdir(ext2_fs_t *fs, uint32_t parent_ino, const char *name);

/* ── src/fat32.h ──────────────────────────────── */
/* 真实声明位于 src/fat32.h */
#include "fat32.h"

/* 见 src/fat32.h:117 */
int fat32_mount(uint32_t partition_lba, fat32_fs_t *fs);
/* 见 src/fat32.h:118 */
int fat32_read_cluster(fat32_fs_t *fs, uint32_t cluster, uint8_t *buf);
/* 见 src/fat32.h:119 */
int fat32_next_cluster(fat32_fs_t *fs, uint32_t cluster);
/* 见 src/fat32.h:120 */
int fat32_read_file(fat32_fs_t *fs, uint32_t first_cluster, uint32_t file_size, uint32_t offset, uint8_t *buf, uint32_t count);
/* 见 src/fat32.h:122 */
int fat32_readdir(fat32_fs_t *fs, uint32_t dir_cluster, uint32_t index, char *name, uint32_t *type, uint32_t *size);
/* 见 src/fat32.h:124 */
int fat32_lookup(fat32_fs_t *fs, uint32_t dir_cluster, const char *name, uint32_t *out_cluster, uint32_t *out_size, uint8_t *out_attr);
/* 见 src/fat32.h:126 */
int fat32_write_file(fat32_fs_t *fs, uint32_t first_cluster, uint32_t *file_size, uint32_t offset, const uint8_t *buf, uint32_t count);
/* 见 src/fat32.h:128 */
int fat32_create_file(fat32_fs_t *fs, uint32_t dir_cluster, const char *name, uint32_t *out_cluster);
/* 见 src/fat32.h:130 */
int fat32_delete_file(fat32_fs_t *fs, uint32_t dir_cluster, const char *name);
/* 见 src/fat32.h:133 */
int fat32_mkdir(fat32_fs_t *fs, uint32_t dir_cluster, const char *name);
/* 见 src/fat32.h:136 */
int fat32_rmdir(fat32_fs_t *fs, uint32_t dir_cluster, const char *name);
/* *  创建时的 0。  ── 见 src/fat32.h:142 */
int fat32_set_file_size(fat32_fs_t *fs, uint32_t dir_cluster, uint32_t file_cluster, uint32_t new_size);
/* *  volume_label 最多 11 字节，NULL 则使用默认卷标。  ── 见 src/fat32.h:147 */
int fat32_format(uint32_t partition_lba, uint32_t total_sectors, const char *volume_label);

/* ── src/fb.h ──────────────────────────────── */
/* 真实声明位于 src/fb.h */
#include "fb.h"

/* 见 src/fb.h:58 */
struct flanterm_context *flanterm_fb_init( /* If _malloc and _free are nulled, use the bump allocated instance (1 use only). */ void *(*_malloc)(uint64_t size), void (*_free)(void *ptr, uint64_t size), uint32_t *framebuffer, uint64_t width, uint64_t height, uint64_t pitch, uint8_t red_mask_size, uint8_t red_mask_shift, uint8_t green_mask_size, uint8_t green_mask_shift, uint8_t blue_mask_size, uint8_t blue_mask_shift, uint32_t *canvas, /* If nulled, no canvas. */ uint32_t *ansi_colours, uint32_t *ansi_bright_colours, /* If nulled, default. */ uint32_t *default_bg, uint32_t *default_fg, /* If nulled, default. */ uint32_t *default_bg_bright, uint32_t *default_fg_bright, /* If nulled, default. */ /* If font is null, use default font and font_width and font_height ignored. */ void *font, uint64_t font_width, uint64_t font_height, uint64_t font_spacing, /* If scale_x and scale_y are 0, automatically scale font based on resolution. */ uint64_t font_scale_x, uint64_t font_scale_y, uint64_t margin, uint32_t num_glyph );

/* ── src/fcntl.h ──────────────────────────────── */
/* 真实声明位于 src/fcntl.h */
#include "fcntl.h"

/* 见 src/fcntl.h:55 */
int open(const char *path, int flags, ...);

/* ── src/fd.h ──────────────────────────────── */
/* 真实声明位于 src/fd.h */
#include "fd.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/fd.h */

/* ── src/flanterm.h ──────────────────────────────── */
/* 真实声明位于 src/flanterm.h */
#include "flanterm.h"

/* 见 src/flanterm.h:83 */
void flanterm_putchar(struct flanterm_context *ctx, uint8_t c);
/* 见 src/flanterm.h:84 */
void flanterm_write(struct flanterm_context *ctx, const char *buf, uint64_t count);
/* 见 src/flanterm.h:85 */
void flanterm_flush(struct flanterm_context *ctx);
/* 见 src/flanterm.h:86 */
void flanterm_full_refresh(struct flanterm_context *ctx);
/* 见 src/flanterm.h:87 */
void flanterm_deinit(struct flanterm_context *ctx, void (*_free)(void *ptr, uint64_t size));
/* 见 src/flanterm.h:89 */
void flanterm_get_dimensions(struct flanterm_context *ctx, uint64_t *cols, uint64_t *rows);
/* 见 src/flanterm.h:90 */
void flanterm_set_autoflush(struct flanterm_context *ctx, bool state);
/* 见 src/flanterm.h:91 */
void flanterm_set_callback( struct flanterm_context *ctx, void (*callback)(struct flanterm_context *, uint64_t, uint64_t, uint64_t, uint64_t) );
/* 见 src/flanterm.h:95 */
uint64_t flanterm_get_oob_output(struct flanterm_context *ctx);
/* 见 src/flanterm.h:96 */
void flanterm_set_oob_output(struct flanterm_context *ctx, uint64_t oob_output);

/* ── src/fs.h ──────────────────────────────── */
/* 真实声明位于 src/fs.h */
#include "fs.h"

/* 初始化文件系统  ── 见 src/fs.h:139 */
int fs_init(void);
/* 检查文件系统一致性  ── 见 src/fs.h:140 */
int fs_check(fs_check_result_t *out);
/* 格式化磁盘（使用默认范围）  ── 见 src/fs.h:141 */
int fs_format_disk(void);
/* 在磁盘上安装 HBFS 文件系统  ── 见 src/fs.h:142 */
int fs_install_disk(void);
/* 在指定 LBA 范围安装 HBFS  ── 见 src/fs.h:143 */
int fs_install_disk_at(uint32_t start_lba, uint32_t sectors);
/* 格式化磁盘为 FAT32（使用默认/已有范围）  ── 见 src/fs.h:144 */
int fs_format_disk_fat32(void);
/* 在磁盘上安装 FAT32 文件系统（自动选择分区范围），成为新装默认格式  ── 见 src/fs.h:145 */
int fs_install_disk_fat32(void);
/* 在指定 LBA 范围安装 FAT32  ── 见 src/fs.h:146 */
int fs_install_disk_fat32_at(uint32_t start_lba, uint32_t sectors);
/* 见 src/fs.h:148 */
int fs_install_disk_fat32_partition(int index);
/* *  /boot/grub/grub.cfg。需先 install auto。  ── 见 src/fs.h:151 */
int fs_install_boot_grub(void);
/* 见 src/fs.h:153 */
int fs_install_boot_kernel(const uint8_t *data, uint32_t len);
/* 见 src/fs.h:155 */
const uint8_t *fs_boot_module_image(uint32_t *out_len);
/* 见 src/fs.h:157 */
uint32_t fs_grub_boot_image_size(void);
/* 见 src/fs.h:158 */
uint32_t fs_grub_core_image_size(void);
/* 获取最近一次错误描述  ── 见 src/fs.h:159 */
const char *fs_last_error(void);
/* 读取分区表信息（最多 4 个分区）  ── 见 src/fs.h:160 */
int fs_read_partitions(fs_partition_info_t out[4]);
/* 挂载磁盘上的 HBFS 文件系统  ── 见 src/fs.h:161 */
int fs_mount_disk(void);
/* 惰性重挂载：盘晚就绪/热插拔时重新探测，不重置 ramfs  ── 见 src/fs.h:162 */
int fs_retry_mount(void);
/* 见 src/fs.h:169 */
int fs_mount_partition(int index);
/* 将文件表同步到磁盘  ── 见 src/fs.h:170 */
int fs_sync(void);
/* 判断当前后端是否为磁盘文件系统  ── 见 src/fs.h:171 */
int fs_is_disk(void);
/* 见 src/fs.h:173 */
int fs_is_readonly(void);
/* 获取当前后端名称字符串  ── 见 src/fs.h:174 */
const char *fs_backend_name(void);
/* 获取 HBFS 分区起始 LBA  ── 见 src/fs.h:175 */
uint32_t fs_disk_start_lba(void);
/* 获取 HBFS 分区总扇区数  ── 见 src/fs.h:176 */
uint32_t fs_disk_total_sectors(void);
/* 获取 HBFS 所需最小扇区数  ── 见 src/fs.h:177 */
uint32_t fs_min_sectors(void);
/* 获取文件系统总容量（字节）  ── 见 src/fs.h:178 */
uint32_t fs_capacity_bytes(void);
/* 获取已使用字节数  ── 见 src/fs.h:179 */
uint32_t fs_used_bytes(void);
/* 列出所有文件（预留接口）  ── 见 src/fs.h:180 */
void fs_list(void);
/* 按文件名查找文件  ── 见 src/fs.h:181 */
file_t *fs_find_file(const char *name);
/* 创建文件（若已存在则返回已有文件）  ── 见 src/fs.h:182 */
file_t *fs_create_file(const char *name);
/* 创建 ramfs/HBFS 符号链接  ── 见 src/fs.h:183 */
file_t *fs_create_symlink(const char *name, const char *target);
/* 删除文件  ── 见 src/fs.h:184 */
int fs_delete_file(const char *name);
/* 复制文件  ── 见 src/fs.h:185 */
int fs_copy_file(const char *src, const char *dst);
/* 重命名文件或目录树  ── 见 src/fs.h:186 */
int fs_rename_file(const char *old_name, const char *new_name);
/* 截断文件（清零大小）  ── 见 src/fs.h:187 */
int fs_truncate_file(file_t *f);
/* 读取文件数据  ── 见 src/fs.h:188 */
uint32_t fs_read_file_data(file_t *f, uint32_t offset, void *buf, uint32_t count);
/* 写入文件数据  ── 见 src/fs.h:189 */
int fs_write_file_data(file_t *f, uint32_t offset, const void *buf, uint32_t count);
/* 获取已使用文件数  ── 见 src/fs.h:190 */
uint32_t fs_get_count(void);
/* 按索引获取文件  ── 见 src/fs.h:191 */
file_t *fs_get_file(uint32_t index);
/* 按索引获取 VFS 节点  ── 见 src/fs.h:192 */
vfs_node_t *fs_get_node(uint32_t index);
/* 读取文件（预留接口）  ── 见 src/fs.h:193 */
void fs_read_file(file_t *f);
/* 见 src/fs.h:195 */
int fs_mkdir(const char *name);
/* 见 src/fs.h:196 */
int fs_rmdir(const char *name);
/* 见 src/fs.h:197 */
int fs_opendir(const char *name);
/* 见 src/fs.h:198 */
int fs_readdir(const char *name, char *out_name, uint32_t *out_type);
/* 见 src/fs.h:199 */
int fs_closedir(const char *name);

/* ── src/gpu.h ──────────────────────────────── */
/* 真实声明位于 src/gpu.h */
#include "gpu.h"

/* 见 src/gpu.h:114 */
int gpu_init(void);
/* 见 src/gpu.h:121 */
gpu_device_t *gpu_primary(void);
/* 见 src/gpu.h:132 */
void gpu_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t color);
/* 见 src/gpu.h:145 */
void gpu_copy_rect(uint64_t src_x, uint64_t src_y, uint64_t dst_x, uint64_t dst_y, uint64_t w, uint64_t h);
/* 见 src/gpu.h:159 */
void gpu_bitblt(const uint32_t *src, uint64_t src_pitch, uint64_t dst_x, uint64_t dst_y, uint64_t w, uint64_t h);
/* 见 src/gpu.h:166 */
void gpu_flush(void);

/* ── src/graphics/blob_unpack.h ──────────────────────────────── */
/* 真实声明位于 src/graphics/blob_unpack.h */
#include "graphics/blob_unpack.h"

/* 见 src/graphics/blob_unpack.h:46 */
const uint8_t *blob_unpack(const uint8_t *src, uint32_t len, const uint8_t **cache, uint32_t *out_len);

/* ── src/graphics/font_cjk.h ──────────────────────────────── */
/* 真实声明位于 src/graphics/font_cjk.h */
#include "graphics/font_cjk.h"

/* inline 定义 utf8_init — Initialize UTF-8 decoder state  ── 见 src/graphics/font_cjk.h:56 */
/* inline 定义 utf8_feed — returns 1 on success with *out set to the decoded codepoint.  ── 见 src/graphics/font_cjk.h:65 */
/* inline 定义 is_cjk_codepoint — Check if a codepoint is CJK (needs wide glyph rendering)  ── 见 src/graphics/font_cjk.h:105 */
/* Returns pointer to 32-byte glyph bitmap, or NULL if not found.  ── 见 src/graphics/font_cjk.h:117 */
const uint8_t* cjk_font_lookup(uint32_t codepoint);
/* Initialize CJK font subsystem (verifies magic)  ── 见 src/graphics/font_cjk.h:120 */
bool cjk_font_init(void);
/* Get total number of loaded CJK glyphs  ── 见 src/graphics/font_cjk.h:123 */
uint32_t cjk_font_get_count(void);

/* ── src/graphics/graphics.h ──────────────────────────────── */
/* 真实声明位于 src/graphics/graphics.h */
#include "graphics/graphics.h"

/* 返回 0 成功, -1 失败（回退到 VGA 文本模式）  ── 见 src/graphics/graphics.h:54 */
int graphics_init(void *mbi);
/* 输出原始字符（绕过 UTF-8 解码器和回滚捕获，用于行编辑重绘）  ── 见 src/graphics/graphics.h:70 */
void console_putchar_raw(char c);
/* Pass NULL to restore normal terminal rendering.  ── 见 src/graphics/graphics.h:75 */
void console_set_sink(void (*fn)(char));
/* （kb_get_key 在紧循环中读键），否则会冻结整个 GUI 合成器且无任何画面反馈。  ── 见 src/graphics/graphics.h:80 */
bool console_has_sink(void);
/* 重置终端状态和 framebuffer 内容（用于从像素级 GUI 返回 shell）  ── 见 src/graphics/graphics.h:86 */
void console_reset_terminal(void);
/* 设置终端标题（未来用）  ── 见 src/graphics/graphics.h:98 */
void console_set_title(const char *title);
/* 光标闪烁（每 ~120k 循环翻转一次）  ── 见 src/graphics/graphics.h:104 */
void console_cursor_blink(void);
/* VGA 文本模式回退时中文无法渲染，返回 false  ── 见 src/graphics/graphics.h:108 */
bool console_is_framebuffer(void);
/* TUI 输出路径已在每次操作后自动调用；GUI 罕见的 no-surface 兜底路径也会调用。  ── 见 src/graphics/graphics.h:112 */
void console_present(void);
/* 向上翻页（查看更早的内容）  ── 见 src/graphics/graphics.h:119 */
void console_scroll_up(int lines);
/* 向下翻页（查看更新的内容）  ── 见 src/graphics/graphics.h:122 */
void console_scroll_down(int lines);
/* 是否处于回滚状态（非实时视图）  ── 见 src/graphics/graphics.h:125 */
bool console_is_scrolled(void);
/* 恢复实时视图  ── 见 src/graphics/graphics.h:128 */
void console_scroll_reset(void);
/* 绘制像素  ── 见 src/graphics/graphics.h:135 */
void fb_put_pixel(uint64_t x, uint64_t y, uint32_t color);
/* 绘制矩形  ── 见 src/graphics/graphics.h:138 */
void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t color);
/* 见 src/graphics/graphics.h:149 */
int fb_get_info(fb_info_t *info);
/* 动态切换分辨率（Bochs/QEMU BGA 驱动支持，并同步更新帧缓冲与终端）  ── 见 src/graphics/graphics.h:152 */
int fb_set_resolution(uint32_t width, uint32_t height);
/* 轮询 QEMU/Bochs VGA EDID 首选分辨率（当宿主自由调整窗口大小时返回 1 并写入新宽高）  ── 见 src/graphics/graphics.h:155 */
int fb_poll_host_resize(uint32_t *out_w, uint32_t *out_h);
/* 获取当前显示物理尺寸计算出的推荐 DPI（基准 96 DPI）  ── 见 src/graphics/graphics.h:158 */
int fb_get_edid_dpi(void);

/* ── src/graphics/gui_font.h ──────────────────────────────── */
/* 真实声明位于 src/graphics/gui_font.h */
#include "graphics/gui_font.h"

/* Parse the embedded blob. Safe to call repeatedly. False if missing/malformed.  ── 见 src/graphics/gui_font.h:52 */
bool gui_font_init(void);
/* Number of baked sizes, and per-size metrics.  ── 见 src/graphics/gui_font.h:55 */
int gui_font_size_count(void);
/* 见 src/graphics/gui_font.h:56 */
int gui_font_size_px(int idx);
/* 见 src/graphics/gui_font.h:57 */
int gui_font_line_height_n(int idx);
/* 见 src/graphics/gui_font.h:58 */
int gui_font_ascent_n(int idx);
/* number of selectable base sizes  ── 见 src/graphics/gui_font.h:64 */
int gui_font_base_count(void);
/* active base slot, 0..base_count-1  ── 见 src/graphics/gui_font.h:65 */
int gui_font_active(void);
/* clamp-set the active base slot  ── 见 src/graphics/gui_font.h:66 */
void gui_font_set_active(int slot);
/* size-directory index of active base  ── 见 src/graphics/gui_font.h:67 */
int gui_font_active_base_idx(void);
/* index of its 2x companion, or -1  ── 见 src/graphics/gui_font.h:68 */
int gui_font_active_large_idx(void);
/* out->coverage == NULL and a valid advance.  ── 见 src/graphics/gui_font.h:72 */
bool gui_font_lookup_n(uint32_t codepoint, int idx, gui_glyph_t *out);
/* Convenience wrappers for the active base size.  ── 见 src/graphics/gui_font.h:75 */
int gui_font_line_height(void);
/* 见 src/graphics/gui_font.h:76 */
int gui_font_ascent(void);
/* 见 src/graphics/gui_font.h:77 */
bool gui_font_lookup(uint32_t codepoint, gui_glyph_t *out);

/* ── src/graphics/gui_icons.h ──────────────────────────────── */
/* 真实声明位于 src/graphics/gui_icons.h */
#include "graphics/gui_icons.h"

/* Parse the embedded blob. Safe to call repeatedly. False if missing/malformed.  ── 见 src/graphics/gui_icons.h:46 */
bool gui_icons_init(void);
/* length in *tile, or NULL if unavailable / id out of range.  ── 见 src/graphics/gui_icons.h:50 */
const uint32_t *gui_icon_tile(int id, int *tile);

/* ── src/graphics/gui_wall.h ──────────────────────────────── */
/* 真实声明位于 src/graphics/gui_wall.h */
#include "graphics/gui_wall.h"

/* Parse the embedded blob. Safe to call repeatedly. False if missing/malformed.  ── 见 src/graphics/gui_wall.h:26 */
bool gui_wall_init(void);
/* Returns the pixel buffer (0x00RRGGBB, row-major) and its dimensions, or NULL.  ── 见 src/graphics/gui_wall.h:29 */
const uint32_t *gui_wall_pixels(int *w, int *h);

/* ── src/gui/app_registry.h ──────────────────────────────── */
/* 真实声明位于 src/gui/app_registry.h */
#include "gui/app_registry.h"

/* 见 src/gui/app_registry.h:65 */
void appreg_init(void);
/* 见 src/gui/app_registry.h:68 */
void appreg_rescan(void);
/* 见 src/gui/app_registry.h:70 */
int appreg_count(void);
/* 见 src/gui/app_registry.h:71 */
const app_reg_t *appreg_at(int i);
/* 未找到返回 -1  ── 见 src/gui/app_registry.h:72 */
int appreg_find_id(const char *id);
/* 从 start 找下一个  ── 见 src/gui/app_registry.h:73 */
int appreg_find_by_pkg(const char *pkg, int start);
/* 见 src/gui/app_registry.h:76 */
int appreg_register(de_entry_t *de, int scope);
/* 见 src/gui/app_registry.h:79 */
int appreg_unregister_pkg(const char *pkg);
/* 见 src/gui/app_registry.h:82 */
int appreg_unregister_id(const char *id);
/* 见 src/gui/app_registry.h:85 */
uint32_t appreg_generation(void);
/* "user" / "system"  ── 见 src/gui/app_registry.h:88 */
const char *appreg_scope_name(int scope);
/* 见 src/gui/app_registry.h:89 */
const char *appreg_apps_dir(int scope);
/* 见 src/gui/app_registry.h:90 */
const char *appreg_bin_dir(int scope);
/* 安装根：/opt/<pkg> 的父目录  ── 见 src/gui/app_registry.h:91 */
const char *appreg_prefix(int scope);
/* status / info 所在目录  ── 见 src/gui/app_registry.h:92 */
const char *appreg_state_dir(int scope);
/* 由 scope 决定的实际路径，写成形如 <prefix>/<pkg> 的绝对路径。  ── 见 src/gui/app_registry.h:95 */
void appreg_pkg_root(int scope, const char *pkg, char *out, uint32_t cap);
/* 见 src/gui/app_registry.h:96 */
void appreg_bin_path(int scope, const char *name, char *out, uint32_t cap);

/* ── src/gui/browser_backend.h ──────────────────────────────── */
/* 真实声明位于 src/gui/browser_backend.h */
#include "gui/browser_backend.h"

/* 见 src/gui/browser_backend.h:92 */
const hive_browser_backend_t *hive_browser_lite_backend(void);
/* 见 src/gui/browser_backend.h:93 */
int hive_browser_backend_valid(const hive_browser_backend_t *backend);
/* 见 src/gui/browser_backend.h:94 */
int hive_browser_backend_supports(const hive_browser_backend_t *backend, uint64_t required);
/* 见 src/gui/browser_backend.h:96 */
uint64_t hive_browser_level_requirements(hive_browser_level_t level);
/* 见 src/gui/browser_backend.h:97 */
uint64_t hive_browser_missing(const hive_browser_backend_t *backend, hive_browser_level_t level);
/* 见 src/gui/browser_backend.h:99 */
const char *hive_browser_capability_name(uint64_t capability);
/* 见 src/gui/browser_backend.h:100 */
uint64_t hive_browser_detect_requirements(const char *html, size_t length);

/* ── src/gui/browser_layout.h ──────────────────────────────── */
/* 真实声明位于 src/gui/browser_layout.h */
#include "gui/browser_layout.h"

/* Compute a responsive, non-overlapping card grid. Returns 0 on success.  ── 见 src/gui/browser_layout.h:46 */
int hive_browser_grid_layout(hive_browser_grid_t *grid, int viewport_w, int viewport_h, size_t card_count, int min_card_w, int card_h, int gap);
/* Return the card index at (x,y), or SIZE_MAX when no card is hit.  ── 见 src/gui/browser_layout.h:51 */
size_t hive_browser_grid_hit_test(const hive_browser_grid_t *grid, int x, int y);

/* ── src/gui/compositor.h ──────────────────────────────── */
/* 真实声明位于 src/gui/compositor.h */
#include "gui/compositor.h"

/* 见 src/gui/compositor.h:90 */
int compositor_init(compositor_t *c);
/* 见 src/gui/compositor.h:99 */
void compositor_begin_frame(compositor_t *c);
/* 见 src/gui/compositor.h:108 */
void compositor_end_frame(compositor_t *c);
/* 见 src/gui/compositor.h:122 */
void compositor_damage_rect(compositor_t *c, uint64_t x, uint64_t y, uint64_t w, uint64_t h);
/* 见 src/gui/compositor.h:133 */
uint32_t *compositor_get_buf(compositor_t *c);
/* 见 src/gui/compositor.h:138 */
uint64_t compositor_get_width(compositor_t *c);
/* 见 src/gui/compositor.h:143 */
uint64_t compositor_get_height(compositor_t *c);
/* 见 src/gui/compositor.h:154 */
uint32_t compositor_blend(uint32_t src, uint32_t dst);
/* 见 src/gui/compositor.h:166 */
void compositor_fill_rect_alpha(compositor_t *c, uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t color);

/* ── src/gui/desktop_entry.h ──────────────────────────────── */
/* 真实声明位于 src/gui/desktop_entry.h */
#include "gui/desktop_entry.h"

/* *  键名带 [locale] 的变体（Name[zh_CN]=）一律忽略，避免覆盖 Name=。  ── 见 src/gui/desktop_entry.h:77 */
int de_parse(const char *text, de_entry_t *out);
/* 见 src/gui/desktop_entry.h:80 */
uint32_t de_format(const de_entry_t *e, char *buf, uint32_t cap);
/* 见 src/gui/desktop_entry.h:83 */
int de_icon_id(const char *icon_name);
/* 见 src/gui/desktop_entry.h:86 */
int de_has_category(const char *categories, const char *want);
/* 见 src/gui/desktop_entry.h:89 */
void de_exec_basename(const char *exec, char *out, uint32_t cap);

/* ── src/gui/effects.h ──────────────────────────────── */
/* 真实声明位于 src/gui/effects.h */
#include "gui/effects.h"

/* inline 定义 make_argb  ── 见 src/gui/effects.h:38 */
/* 见 src/gui/effects.h:50 */
uint32_t color_blend(uint32_t fg, uint32_t bg, uint8_t alpha);
/* 见 src/gui/effects.h:59 */
uint32_t color_darken(uint32_t color, uint8_t factor);
/* 见 src/gui/effects.h:68 */
uint32_t color_lighten(uint32_t color, uint8_t factor);
/* 见 src/gui/effects.h:88 */
void draw_shadow(uint32_t *buf, uint64_t pitch, uint64_t screen_w, uint64_t screen_h, int x, int y, int w, int h, int blur_radius, uint8_t opacity);
/* 见 src/gui/effects.h:109 */
int point_in_rounded_rect(int px, int py, int rx, int ry, int rw, int rh, int radius);
/* 见 src/gui/effects.h:126 */
void draw_rounded_rect(uint32_t *buf, uint64_t pitch, uint64_t screen_w, uint64_t screen_h, int x, int y, int w, int h, int radius, uint32_t color);
/* 见 src/gui/effects.h:149 */
void draw_gradient_vertical(uint32_t *buf, uint64_t pitch, uint64_t screen_w, uint64_t screen_h, int x, int y, int w, int h, uint32_t color_top, uint32_t color_bottom);
/* 见 src/gui/effects.h:168 */
void draw_gradient_horizontal(uint32_t *buf, uint64_t pitch, uint64_t screen_w, uint64_t screen_h, int x, int y, int w, int h, uint32_t color_left, uint32_t color_right);

/* ── src/gui/glass.h ──────────────────────────────── */
/* 真实声明位于 src/gui/glass.h */
#include "gui/glass.h"

/* * 界面仍然正常（只是没有磨砂效果）。  ── 见 src/gui/glass.h:42 */
void glass_init(void);
/* 见 src/gui/glass.h:45 */
int glass_available(void);
/* 见 src/gui/glass.h:62 */
void glass_rect(int x, int y, int w, int h, int radius, int corners, uint32_t tint, int tint_a, int strength);
/* * gui_dirty_mark_full()，下一帧从干净背景整帧重画玻璃。  ── 见 src/gui/glass.h:70 */
int glass_full_requested(void);
/* 见 src/gui/glass.h:71 */
void glass_clear_full_request(void);

/* ── src/gui/gui_app.h ──────────────────────────────── */
/* 真实声明位于 src/gui/gui_app.h */
#include "gui/gui_app.h"

/* 见 src/gui/gui_app.h:64 */
const gui_app_module_t *gui_app_by_mode(int mode);
/* 见 src/gui/gui_app.h:66 */
int gui_app_module_count(void);
/* 见 src/gui/gui_app.h:68 */
const gui_app_module_t *gui_app_module_at(int index);
/* 见 src/gui/gui_app.h:69 */
int gui_app_draw(gui_state_t *st, int mode, int tx, int ty, int win_w, int win_h);
/* 见 src/gui/gui_app.h:70 */
int gui_app_handle_key(gui_state_t *st, int key);
/* 见 src/gui/gui_app.h:71 */
int gui_app_tick(gui_state_t *st);
/* 见 src/gui/gui_app.h:72 */
int gui_app_on_click(gui_state_t *st, int mx, int my, int tx, int ty, int win_w, int win_h);
/* 见 src/gui/gui_app.h:73 */
void app_settings_wheel(gui_state_t *st, int steps);

/* ── src/gui/gui_browser.h ──────────────────────────────── */
/* 真实声明位于 src/gui/gui_browser.h */
#include "gui/gui_browser.h"

/* 见 src/gui/gui_browser.h:45 */
int gui_browser_render_page(int x, int y, int w, int h, const char *buf, uint32_t len, int scroll);
/* 见 src/gui/gui_browser.h:52 */
int gui_browser_measure_page(int w, const char *buf, uint32_t len);
/* 见 src/gui/gui_browser.h:61 */
void gui_browser_reset(void);
/* 见 src/gui/gui_browser.h:69 */
int gui_browser_hit_link(int x, int y, char *out, uint32_t out_cap);
/* 见 src/gui/gui_browser.h:75 */
int gui_browser_image_count(void);
/* 见 src/gui/gui_browser.h:76 */
int gui_browser_image_info(int index, const char **src, int *w, int *h);

/* ── src/gui/gui_canvas.h ──────────────────────────────── */
/* 真实声明位于 src/gui/gui_canvas.h */
#include "gui/gui_canvas.h"

/* 见 src/gui/gui_canvas.h:42 */
int gui_app_info(int *w, int *h);
/* 见 src/gui/gui_canvas.h:45 */
void gui_app_clear(uint32_t color);
/* 见 src/gui/gui_canvas.h:48 */
void gui_app_rect(int x, int y, int w, int h, uint32_t color);
/* 见 src/gui/gui_canvas.h:51 */
void gui_app_text(int x, int y, const char *s, uint32_t color, int scale);
/* 见 src/gui/gui_canvas.h:54 */
void gui_app_present(void);
/* 见 src/gui/gui_canvas.h:57 */
int gui_app_pollkey(void);
/* 见 src/gui/gui_canvas.h:60 */
int gui_app_pollmouse(int *x, int *y);
/* 见 src/gui/gui_canvas.h:68 */
void gui_app_line(int x0, int y0, int x1, int y1, uint32_t color);
/* 见 src/gui/gui_canvas.h:71 */
void gui_app_thick_line(int x0, int y0, int x1, int y1, int thickness, uint32_t color);
/* 见 src/gui/gui_canvas.h:74 */
void gui_app_circle(int cx, int cy, int r, uint32_t color);
/* 见 src/gui/gui_canvas.h:77 */
void gui_app_fill_circle(int cx, int cy, int r, uint32_t color);
/* 见 src/gui/gui_canvas.h:80 */
void gui_app_round_rect(int x, int y, int w, int h, int r, uint32_t color, int corners);
/* 见 src/gui/gui_canvas.h:83 */
void gui_app_gradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
/* 见 src/gui/gui_canvas.h:86 */
void gui_app_alpha_rect(int x, int y, int w, int h, uint32_t color);
/* 见 src/gui/gui_canvas.h:89 */
void gui_app_blit(int x, int y, int w, int h, const uint32_t *src);
/* 见 src/gui/gui_canvas.h:92 */
void gui_app_icon(int x, int y, int icon_id, uint32_t tint);
/* 见 src/gui/gui_canvas.h:95 */
int gui_app_text_width(const char *s, int scale);
/* 见 src/gui/gui_canvas.h:96 */
int gui_app_text_height(int scale);

/* ── src/gui/gui_dirty.h ──────────────────────────────── */
/* 真实声明位于 src/gui/gui_dirty.h */
#include "gui/gui_dirty.h"

/* 见 src/gui/gui_dirty.h:39 */
void gui_dirty_reset(void);
/* 见 src/gui/gui_dirty.h:40 */
void gui_dirty_mark_full(void);
/* 见 src/gui/gui_dirty.h:41 */
int gui_dirty_is_full(void);
/* 见 src/gui/gui_dirty.h:42 */
void gui_dirty_add(int x, int y, int w, int h);
/* 见 src/gui/gui_dirty.h:43 */
int gui_dirty_count(void);
/* 见 src/gui/gui_dirty.h:44 */
int gui_dirty_get(int idx, int *x, int *y, int *w, int *h);
/* 见 src/gui/gui_dirty.h:46 */
void gui_clip_clear(void);
/* 见 src/gui/gui_dirty.h:47 */
void gui_clip_set(int x, int y, int w, int h);
/* 见 src/gui/gui_dirty.h:58 */
int gui_clip_push(int x, int y, int w, int h);
/* 见 src/gui/gui_dirty.h:60 */
void gui_clip_pop(void);
/* 见 src/gui/gui_dirty.h:61 */
int gui_clip_active(void);
/* 见 src/gui/gui_dirty.h:62 */
int gui_clip_intersect(int *x, int *y, int *w, int *h);
/* 见 src/gui/gui_dirty.h:64 */
void gui_dirty_expand(int *x, int *y, int *w, int *h, int pad, int max_w, int max_h);
/* 见 src/gui/gui_dirty.h:71 */
int gui_dirty_add_regions(const gui_region_t *regions, int n, int screen_w, int screen_h);

/* ── src/gui/gui_draw.h ──────────────────────────────── */
/* 真实声明位于 src/gui/gui_draw.h */
#include "gui/gui_draw.h"

/* 见 src/gui/gui_draw.h:106 */
uint32_t gui_rgb(uint8_t r, uint8_t g, uint8_t b);
/* 见 src/gui/gui_draw.h:108 */
void gui_set_layer_opacity(uint8_t opacity);
/* 见 src/gui/gui_draw.h:109 */
uint8_t gui_get_layer_opacity(void);
/* 见 src/gui/gui_draw.h:111 */
void gui_set_surface(uint32_t *surface, int w, int h, uint32_t pitch_px);
/* * buffer 上；裁剪区语义不变。只能单层嵌套（内层请求会被忽略）。  ── 见 src/gui/gui_draw.h:116 */
void gui_target_begin(uint32_t *px, int w, int h, uint32_t pitch_px, int origin_x, int origin_y);
/* 见 src/gui/gui_draw.h:118 */
void gui_target_end(void);
/* 见 src/gui/gui_draw.h:119 */
int gui_target_active(void);
/* 见 src/gui/gui_draw.h:120 */
void gd_target_origin(int *ox, int *oy);
/* * 必须在 gui_set_surface() 之后调用；失败时自动退回直贴路径。  ── 见 src/gui/gui_draw.h:124 */
void gui_wl_init(uint32_t *pixels, int w, int h, uint32_t pitch_px);
/* 见 src/gui/gui_draw.h:125 */
int gui_wl_active(void);
/* 见 src/gui/gui_draw.h:126 */
void gui_present_surface(const fb_info_t *fb);
/* 见 src/gui/gui_draw.h:127 */
void gui_present_rect(const fb_info_t *fb, int x, int y, int w, int h);
/* 见 src/gui/gui_draw.h:129 */
void gui_rect(int x, int y, int w, int h, uint32_t color);
/* 见 src/gui/gui_draw.h:130 */
void gui_rect_alpha(int x, int y, int w, int h, uint32_t color);
/* * 使用。  ── 见 src/gui/gui_draw.h:135 */
void gui_blit_rgb888(int x, int y, const uint8_t *rgb, int img_w, int img_h, int clip_x, int clip_y, int clip_w, int clip_h);
/* * 一个 32 位拷贝就完事。alpha=0xFF 的像素走不透明路径，其余做混合。  ── 见 src/gui/gui_draw.h:140 */
void gui_blit_argb8888(int x, int y, const uint8_t *argb, int img_w, int img_h, int src_stride, int clip_x, int clip_y, int clip_w, int clip_h);
/* 见 src/gui/gui_draw.h:143 */
void gui_border(int x, int y, int w, int h, uint32_t color);
/* 见 src/gui/gui_draw.h:144 */
void gui_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
/* 见 src/gui/gui_draw.h:145 */
void gui_soft_shadow(int x, int y, int w, int h);
/* 见 src/gui/gui_draw.h:146 */
void gui_draw_panel_shell(int x, int y, int w, int h, uint32_t top, uint32_t bottom, uint32_t border_c, uint32_t accent);
/* 见 src/gui/gui_draw.h:148 */
void gui_text(int x, int y, const char *s, uint32_t color, int scale);
/* 见 src/gui/gui_draw.h:149 */
void gui_text_clipped(int x, int y, int max_x, const char *s, uint32_t color, int scale);
/* 见 src/gui/gui_draw.h:150 */
int gui_text_width(const char *s, int scale);
/* * gui_draw_codepoint 画出字形并返回步进；查不到字形时按 '?' 兜底。  ── 见 src/gui/gui_draw.h:155 */
int gui_cp_advance(uint32_t cp, int scale);
/* 见 src/gui/gui_draw.h:156 */
int gui_draw_codepoint(int x, int y, uint32_t cp, uint32_t color, int scale);
/* * 控制台终端与 C 语法高亮共用。  ── 见 src/gui/gui_draw.h:161 */
int gui_text_mono(int x, int y, int max_x, const char *s, uint32_t color);
/* Flat Breeze 风格小按钮（代码工作台工具行用）  ── 见 src/gui/gui_draw.h:166 */
void gui_draw_small_button(int x, int y, int w, const char *label, uint32_t color);
/* 8x16 控制台位图字体的单字符绘制（C 语法高亮 span 用），y 为 cell 顶  ── 见 src/gui/gui_draw.h:168 */
int gui_draw_mono_char(int x, int y, char c, uint32_t color);
/* GUI 主循环按键轮询（含 GUI_KEY_* 归一化），C 脚本 GUI 的 wait_key 用  ── 见 src/gui/gui_draw.h:170 */
int gui_key_poll(void);
/* 离屏表面尺寸（GUI 未运行时为 0）  ── 见 src/gui/gui_draw.h:172 */
int gui_surface_width(void);
/* 见 src/gui/gui_draw.h:173 */
int gui_surface_height(void);
/* DPI 缩放（ui_s 的导出形式；g_ui_scale 由 GUI 启动时按分辨率设置）  ── 见 src/gui/gui_draw.h:175 */
int gui_ui_scale_v(int v);
/* 见 src/gui/gui_draw.h:177 */
void gui_append_char(char *buf, uint32_t cap, uint32_t *pos, char c);
/* 见 src/gui/gui_draw.h:178 */
void gui_append_str(char *buf, uint32_t cap, uint32_t *pos, const char *s);
/* 见 src/gui/gui_draw.h:179 */
void gui_append_int(char *buf, uint32_t cap, uint32_t *pos, int v);
/* 见 src/gui/gui_draw.h:180 */
void gui_append_uint(char *buf, uint32_t cap, uint32_t *pos, uint32_t v);
/* 见 src/gui/gui_draw.h:181 */
void gui_append_ll(char *buf, uint32_t cap, uint32_t *pos, long long v);
/* * long long -- see app_calc.c's calc_to_sci for how mant/exp are derived.  ── 见 src/gui/gui_draw.h:184 */
void gui_append_sci(char *buf, uint32_t cap, uint32_t *pos, long long mant, int exp);
/* 见 src/gui/gui_draw.h:186 */
void gui_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
/* 见 src/gui/gui_draw.h:187 */
void gui_draw_thick_line(int x0, int y0, int x1, int y1, int thickness, uint32_t color);
/* 见 src/gui/gui_draw.h:188 */
void gui_fill_circle(int cx, int cy, int r, uint32_t color);
/* 见 src/gui/gui_draw.h:189 */
void gui_draw_circle(int cx, int cy, int r, uint32_t color);
/* 见 src/gui/gui_draw.h:190 */
void gui_fill_round_rect(int x, int y, int w, int h, int r, uint32_t color);
/* * 300+ 处调用点零改动；其它模块（控件库等）应直接用 gd_ 版本。  ── 见 src/gui/gui_draw.h:196 */
void gui_set_brightness(int pct);
/* 见 src/gui/gui_draw.h:197 */
int gui_get_brightness(void);
/* 见 src/gui/gui_draw.h:198 */
uint32_t *gd_surface_ptr(void);
/* 见 src/gui/gui_draw.h:199 */
int gd_surface_w(void);
/* 见 src/gui/gui_draw.h:200 */
int gd_surface_h(void);
/* 见 src/gui/gui_draw.h:201 */
uint32_t gd_surface_pitch(void);
/* 见 src/gui/gui_draw.h:202 */
int gd_draw_text_codepoint(int x, int y, uint32_t cp, uint32_t color, int scale);
/* 见 src/gui/gui_draw.h:203 */
void gd_vgradient_alpha(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
/* 见 src/gui/gui_draw.h:204 */
void gd_draw_window_control_icon(int x, int y, int kind, int restore, uint32_t color);
/* 见 src/gui/gui_draw.h:209 */
int *gd_text_italic_ptr(void);
/* 见 src/gui/gui_draw.h:210 */
void gd_rect(int x, int y, int w, int h, uint32_t color);
/* 见 src/gui/gui_draw.h:211 */
void gd_rect_alpha(int x, int y, int w, int h, uint32_t color);
/* 见 src/gui/gui_draw.h:212 */
void gd_border(int x, int y, int w, int h, uint32_t color);
/* 见 src/gui/gui_draw.h:213 */
void gd_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
/* 见 src/gui/gui_draw.h:214 */
void gd_hgradient(int x, int y, int w, int h, uint32_t left, uint32_t right);
/* 见 src/gui/gui_draw.h:215 */
void gd_soft_shadow(int x, int y, int w, int h);
/* 见 src/gui/gui_draw.h:224 */
void gd_fill_round_rect(int x, int y, int w, int h, int r, uint32_t color, int corners);
/* 见 src/gui/gui_draw.h:225 */
void gd_round_border(int x, int y, int w, int h, int r, uint32_t border_c, uint32_t body_c);
/* 见 src/gui/gui_draw.h:226 */
void gd_draw_panel_shell(int x, int y, int w, int h, uint32_t top, uint32_t bottom, uint32_t border_c, uint32_t accent);
/* 见 src/gui/gui_draw.h:228 */
void gd_draw_inset_shell(int x, int y, int w, int h, uint32_t border_c);
/* 见 src/gui/gui_draw.h:229 */
void gd_blit_glyph(int x, int y, const gui_glyph_t *g, uint32_t color, int scale);
/* 见 src/gui/gui_draw.h:230 */
void gd_blit_mono_glyph(int x, int y, const uint8_t *glyph, uint32_t color, int scale);
/* 见 src/gui/gui_draw.h:231 */
void gd_blit_icon_rgba(int x, int y, int sz, const uint32_t *src, int tile);
/* 见 src/gui/gui_draw.h:232 */
void gd_blit_icon(int x, int y, int sz, int icon_id);
/* 见 src/gui/gui_draw.h:233 */
int gd_draw_mono_char(int x, int y, char c, uint32_t color);
/* 见 src/gui/gui_draw.h:234 */
int gd_text_width(const char *s, int scale);
/* 见 src/gui/gui_draw.h:235 */
void gd_text(int x, int y, const char *s, uint32_t color, int scale);
/* 见 src/gui/gui_draw.h:236 */
void gd_text_clipped(int x, int y, int max_x, const char *s, uint32_t color, int scale);
/* 见 src/gui/gui_draw.h:237 */
int gd_text_mono(int x, int y, int max_x, const char *s, uint32_t color);
/* 见 src/gui/gui_draw.h:238 */
uint32_t gd_rgb_mix(uint32_t c1, uint32_t c2, int t);
/* 见 src/gui/gui_draw.h:239 */
uint32_t gd_rgb_lift(uint32_t c, int d);
/* 见 src/gui/gui_draw.h:240 */
uint32_t gd_apply_layer_opacity(uint32_t color);
/* 见 src/gui/gui_draw.h:241 */
void gd_round_corner(int bx, int by, int r, int cxc, int cyc, uint32_t color);
/* 见 src/gui/gui_draw.h:242 */
void gd_append_ll(char *buf, uint32_t cap, uint32_t *pos, long long v);
/* 见 src/gui/gui_draw.h:243 */
void gd_append_sci(char *buf, uint32_t cap, uint32_t *pos, long long mant, int exp);
/* 见 src/gui/gui_draw.h:244 */
void gd_append_char(char *buf, uint32_t cap, uint32_t *pos, char c);
/* 见 src/gui/gui_draw.h:245 */
void gd_append_str(char *buf, uint32_t cap, uint32_t *pos, const char *s);
/* 见 src/gui/gui_draw.h:246 */
void gd_append_uint(char *buf, uint32_t cap, uint32_t *pos, uint32_t v);
/* 见 src/gui/gui_draw.h:247 */
void gd_append_int(char *buf, uint32_t cap, uint32_t *pos, int v);

/* ── src/gui/gui_effects.h ──────────────────────────────── */
/* 真实声明位于 src/gui/gui_effects.h */
#include "gui/gui_effects.h"

/* 见 src/gui/gui_effects.h:45 */
void gui_effects_set_enabled(int on);
/* 见 src/gui/gui_effects.h:46 */
int gui_effects_enabled(void);
/* 见 src/gui/gui_effects.h:48 */
void gui_effects_set_perm(int level);
/* 见 src/gui/gui_effects.h:49 */
int gui_effects_perm(void);
/* 见 src/gui/gui_effects.h:56 */
int gui_effects_allowed(int expensive);
/* 见 src/gui/gui_effects.h:64 */
int gui_effects_allowed_geometry(void);
/* 见 src/gui/gui_effects.h:67 */
int gui_effects_max_frames(void);
/* 见 src/gui/gui_effects.h:76 */
int gui_effects_duration_ms(void);
/* 见 src/gui/gui_effects.h:79 */
uint32_t gui_effects_pit_hz(void);
/* 见 src/gui/gui_effects.h:82 */
uint64_t gui_effects_now_ticks(void);
/* 见 src/gui/gui_effects.h:91 */
int gui_effects_ease_progress(uint64_t t0, uint32_t dur_ms);
/* 见 src/gui/gui_effects.h:117 */
int gui_ease_apply(int curve, int progress);
/* 见 src/gui/gui_effects.h:126 */
int gui_effects_ease_curve(uint64_t t0, uint32_t dur_ms, int curve);
/* 见 src/gui/gui_effects.h:129 */
uint64_t gui_effects_now_ms(void);

/* ── src/gui/gui_html.h ──────────────────────────────── */
/* 真实声明位于 src/gui/gui_html.h */
#include "gui/gui_html.h"

/* ── 解析入口（实现见 src/gui/gui_html.c）──  ── 见 src/gui/gui_html.h:169 */
char gh_br_decode_entity(const char *p, uint32_t *consumed);
/* 见 src/gui/gui_html.h:170 */
void gh_browser_text_from_html(const char *html, char *out, uint32_t cap, uint32_t *out_len);
/* 见 src/gui/gui_html.h:171 */
int gh_br_is_inline_type(int t);
/* 见 src/gui/gui_html.h:172 */
int gh_tag_ci_eq(const char *name, int len, const char *lit);
/* 见 src/gui/gui_html.h:173 */
int gh_css_simple_selector_matches(const char *selector, const char *tag, int tag_len, const char *cls, const char *id);
/* 见 src/gui/gui_html.h:175 */
void gh_browser_style_get(int type, browser_style_t *s);
/* 见 src/gui/gui_html.h:176 */
int gh_browser_style_for_tag(const char *name, int len);
/* 见 src/gui/gui_html.h:177 */
int gh_br_attr_value(const char *tag, uint32_t tag_len, const char *name, char *out, uint32_t out_cap);
/* 见 src/gui/gui_html.h:179 */
int gh_css_parse_color(const char *s, uint32_t *out);
/* 见 src/gui/gui_html.h:180 */
void gh_css_parse_decls(css_decl_t *decl, const char *text, uint32_t len);
/* 见 src/gui/gui_html.h:181 */
void gh_css_parse_stylesheet(const char *css, uint32_t len);
/* 见 src/gui/gui_html.h:182 */
void gh_css_reset(void);
/* 见 src/gui/gui_html.h:217 */
void gh_css_resolve_for_tag(const char *tag, int tag_len, const char *cls, const char *id, const char *inline_style, css_decl_t *out);
/* 见 src/gui/gui_html.h:224 */
void gb_img_set(int slot, int w, int h, const uint8_t *rgb, uint32_t nbytes);
/* 见 src/gui/gui_html.h:225 */
void gb_img_set_src(int slot, const char *src);
/* 见 src/gui/gui_html.h:226 */
int gb_img_count(void);
/* 见 src/gui/gui_html.h:227 */
void gb_img_reset(void);
/* 见 src/gui/gui_html.h:243 */
void gb_browser_add_link_rect(int x, int y, int w, int h, int link_idx);
/* 见 src/gui/gui_html.h:244 */
int gb_browser_draw_segment(int x, int w, int *cy, int row_unit, int scroll, int max_lines, int *drawn_rows, const char *buf, uint32_t start, uint32_t seg_len, const browser_style_t *bsp, int link_idx);
/* 见 src/gui/gui_html.h:245 */
uint32_t gb_br_read_style(const char *buf, uint32_t len, uint32_t i, int type, browser_style_t *bs, int *link_idx);
/* 见 src/gui/gui_html.h:246 */
uint32_t gb_br_utf8_next(const char *s, uint32_t pos, uint32_t end, uint32_t *cp_out);
/* 见 src/gui/gui_html.h:247 */
int gb_br_cp_is_cjk(uint32_t cp);
/* 见 src/gui/gui_html.h:248 */
void gb_br_draw_tok(int px, int py, int rowh, int rh, const char *tok, int tw, const browser_style_t *bs, int link_idx);
/* 见 src/gui/gui_html.h:249 */
int gb_browser_flow_block(int x, int w, int *cy, int row_unit, int scroll, int max_lines, int *drawn_rows, const char *buf, const br_run_t *runs, int nrun);
/* 见 src/gui/gui_html.h:250 */
uint32_t gb_br_table_cell(const char *buf, uint32_t start, uint32_t seg_len, int col, char *out, uint32_t out_cap);
/* 见 src/gui/gui_html.h:251 */
void gb_br_table_scan(const char *buf, uint32_t len, uint32_t pos, int avail_w, int *col_w, int *ncols_out);
/* 见 src/gui/gui_html.h:252 */
int gb_br_draw_table_row(int x, int *cy, int row_unit, int scroll, int max_lines, int *drawn_rows, const char *buf, uint32_t start, uint32_t seg_len, const browser_style_t *bs, const int *col_w, int ncols, int last_row);
/* 见 src/gui/gui_html.h:253 */
uint32_t gb_br_card_read(const char *buf, uint32_t len, uint32_t i, br_card_t *c);
/* 见 src/gui/gui_html.h:254 */
void gb_br_card_text_copy(const char *buf, uint32_t start, uint32_t len, char *out, uint32_t cap);
/* 见 src/gui/gui_html.h:255 */
int gb_browser_draw_card_grid(int x, int w, int *cy, int row_unit, int scroll, int max_lines, int *drawn_rows, const char *buf, const br_card_t *cards, int ncards);
/* 见 src/gui/gui_html.h:256 */
int gb_browser_draw_hero_card(int x, int w, int *cy, int row_unit, int scroll, int max_lines, int *drawn_rows, const char *buf, const br_card_t *card);
/* 见 src/gui/gui_html.h:257 */
int gb_draw_rendered_page(int x, int y, int w, int h, const char *buf, uint32_t len, int scroll);
/* 见 src/gui/gui_html.h:258 */
void gb_br_icon_arrow(int cx, int cy, int dir, uint32_t col);
/* 见 src/gui/gui_html.h:259 */
void gb_br_icon_reload(int cx, int cy, uint32_t col, uint32_t bg);
/* 见 src/gui/gui_html.h:261 */
int gb_link_rect_count(void);
/* 见 src/gui/gui_html.h:262 */
browser_link_rect_t *gb_link_rect_at(int idx);
/* 见 src/gui/gui_html.h:263 */
void gb_link_rects_reset(void);

/* ── src/gui/gui_panel.h ──────────────────────────────── */
/* 真实声明位于 src/gui/gui_panel.h */
#include "gui/gui_panel.h"

/* 见 src/gui/gui_panel.h:37 */
const char *gui_file_path(gui_state_t *st);
/* 见 src/gui/gui_panel.h:38 */
void gui_set_file_path(gui_state_t *st, const char *path);
/* 见 src/gui/gui_panel.h:39 */
uint32_t gui_file_count(gui_state_t *st);
/* 见 src/gui/gui_panel.h:40 */
int gui_file_entry(gui_state_t *st, uint32_t index, char *name, uint32_t *type, vfs_node_t **node, char *full, uint32_t full_cap);
/* 见 src/gui/gui_panel.h:42 */
int gui_selected_entry(gui_state_t *st, char *name, uint32_t *type, vfs_node_t **node, char *full, uint32_t full_cap);
/* 见 src/gui/gui_panel.h:44 */
file_t *gui_selected_regular_file(gui_state_t *st);
/* 见 src/gui/gui_panel.h:45 */
void gui_select_file(gui_state_t *st, int index);
/* 后缀匹配（大小写不敏感，FAT32 短文件名全大写也能命中）  ── 见 src/gui/gui_panel.h:48 */
int gui_has_suffix(const char *path, const char *suffix);
/* 记事本共享命名（文件面板选中文件时会把名字带给记事本）  ── 见 src/gui/gui_panel.h:51 */
void gui_set_note_name(gui_state_t *st, const char *name);
/* 见 src/gui/gui_panel.h:52 */
const char *gui_note_name(gui_state_t *st);
/* * win 传 NULL 时按 idx 从 WM 取。  ── 见 src/gui/gui_panel.h:56 */
void gui_window_metrics(gui_state_t *st, int w, int h, const wm_window_t *win, int idx, int *out_x, int *out_y, int *out_w, int *out_h);
/* 把 st->app_mode / active 同步到当前活跃窗口（键盘/定时器分发前调用）  ── 见 src/gui/gui_panel.h:59 */
void gui_sync_focus(gui_state_t *st);
/* * 提供站内入口，不必让用户自己去开文件管理器（issue #8）。  ── 见 src/gui/gui_panel.h:63 */
void gui_open_files_panel(gui_state_t *st);

/* ── src/gui/gui_state.h ──────────────────────────────── */
/* 真实声明位于 src/gui/gui_state.h */
#include "gui/gui_state.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/gui/gui_state.h */

/* ── src/gui/hiverc.h ──────────────────────────────── */
/* 真实声明位于 src/gui/hiverc.h */
#include "gui/hiverc.h"

/* 见 src/gui/hiverc.h:31 */
void hiverc_load(struct gui_state *st);
/* 见 src/gui/hiverc.h:32 */
void hiverc_save(struct gui_state *st);
/* 当前用户与其家目录（简单多用户入口）  ── 见 src/gui/hiverc.h:35 */
const char *hiverc_current_user(void);
/* 见 src/gui/hiverc.h:36 */
void hiverc_user_home(char *out, int cap);
/* * 与登录用户名（决定家目录）相互独立，可随时自定义。  ── 见 src/gui/hiverc.h:40 */
void hive_display_name(char *out, int cap);
/* 见 src/gui/hiverc.h:41 */
int hive_set_display_name(const char *name);

/* ── src/gui/overview_layout.h ──────────────────────────────── */
/* 真实声明位于 src/gui/overview_layout.h */
#include "gui/overview_layout.h"

/* 见 src/gui/overview_layout.h:57 */
int ov_layout_compute(const ov_win_t *wins, int n, int ax, int ay, int aw, int ah, int gap, ov_layout_t *out);

/* ── src/gui/wayland/backend/wl_backend.h ──────────────────────────────── */
/* 真实声明位于 src/gui/wayland/backend/wl_backend.h */
#include "gui/wayland/backend/wl_backend.h"

/* 见 src/gui/wayland/backend/wl_backend.h:33 */
void wl_backend_init(void);
/* 见 src/gui/wayland/backend/wl_backend.h:36 */
void wl_backend_pump(void);
/* 见 src/gui/wayland/backend/wl_backend.h:39 */
void wl_backend_surface_committed(wl_client_t *client, wl_surface_state_t *surface);
/* 见 src/gui/wayland/backend/wl_backend.h:43 */
void wl_backend_surface_destroyed(wl_client_t *client, wl_surface_state_t *surface);
/* 见 src/gui/wayland/backend/wl_backend.h:51 */
int wl_backend_pointer_motion(int x, int y);
/* 见 src/gui/wayland/backend/wl_backend.h:52 */
int wl_backend_pointer_button(int x, int y, uint32_t button, int pressed);
/* 见 src/gui/wayland/backend/wl_backend.h:53 */
int wl_backend_pointer_axis(int x, int y, int32_t axis, int32_t value);
/* 见 src/gui/wayland/backend/wl_backend.h:54 */
int wl_backend_keyboard_key(uint32_t keycode, int pressed);
/* 见 src/gui/wayland/backend/wl_backend.h:55 */
int wl_backend_keyboard_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
/* 见 src/gui/wayland/backend/wl_backend.h:59 */
int wl_backend_has_keyboard_focus(void);
/* 见 src/gui/wayland/backend/wl_backend.h:69 */
void wl_backend_set_input_probe(int enable);

/* ── src/gui/wayland/core/wl_server.h ──────────────────────────────── */
/* 真实声明位于 src/gui/wayland/core/wl_server.h */
#include "gui/wayland/core/wl_server.h"

/* 见 src/gui/wayland/core/wl_server.h:127 */
int wl_server_start(const char *path);
/* 见 src/gui/wayland/core/wl_server.h:129 */
int wl_server_running(void);
/* 见 src/gui/wayland/core/wl_server.h:132 */
int wl_server_poll_once(void);
/* 见 src/gui/wayland/core/wl_server.h:136 */
wl_object_t *wl_client_object(wl_client_t *client, uint32_t id);
/* 见 src/gui/wayland/core/wl_server.h:139 */
uint32_t wl_client_new_id(wl_client_t *client, wl_object_kind_t kind, void *data);
/* 见 src/gui/wayland/core/wl_server.h:143 */
wl_object_t *wl_client_adopt_id(wl_client_t *client, uint32_t id, wl_object_kind_t kind, void *data);
/* 见 src/gui/wayland/core/wl_server.h:147 */
void wl_client_destroy(wl_client_t *client, uint32_t id);
/* 见 src/gui/wayland/core/wl_server.h:150 */
uint32_t wl_client_find_id(wl_client_t *client, wl_object_kind_t kind, const void *data);
/* 见 src/gui/wayland/core/wl_server.h:155 */
void wl_event_begin(wl_client_t *client, uint32_t object_id, uint16_t opcode);
/* 见 src/gui/wayland/core/wl_server.h:156 */
void wl_event_u32(wl_client_t *client, uint32_t value);
/* 见 src/gui/wayland/core/wl_server.h:157 */
void wl_event_i32(wl_client_t *client, int32_t value);
/* 见 src/gui/wayland/core/wl_server.h:158 */
void wl_event_string(wl_client_t *client, const char *text);
/* 见 src/gui/wayland/core/wl_server.h:159 */
void wl_event_end(wl_client_t *client);
/* 见 src/gui/wayland/core/wl_server.h:160 */
void wl_event_abort(wl_client_t *client);
/* 见 src/gui/wayland/core/wl_server.h:162 */
int wl_client_flush(wl_client_t *client);
/* 见 src/gui/wayland/core/wl_server.h:164 */
void wl_server_broadcast_global(const wl_global_t *global);
/* 见 src/gui/wayland/core/wl_server.h:167 */
void wl_server_set_release_hook(wl_object_release_fn hook);
/* * 只打一行，诊断"客户端到底绑了哪些全局、哪个没绑"时不必再猜。  ── 见 src/gui/wayland/core/wl_server.h:171 */
void wl_dbg_bind(const char *interface, uint32_t name, uint32_t version, uint32_t id);
/* * 在哪一步被丢掉"时用，比给每处单独写一套打印省事。  ── 见 src/gui/wayland/core/wl_server.h:176 */
void wl_dbg_note(const char *label, int a, int b, int c);
/* 见 src/gui/wayland/core/wl_server.h:180 */
const wl_global_t *wl_server_global_by_interface(const char *interface);
/* 见 src/gui/wayland/core/wl_server.h:181 */
const wl_global_t *wl_server_global_by_name(uint32_t name);
/* 见 src/gui/wayland/core/wl_server.h:182 */
const wl_global_t *wl_server_global_list(int *count);
/* 见 src/gui/wayland/core/wl_server.h:186 */
wl_client_t *wl_server_client_at(int index);
/* 见 src/gui/wayland/core/wl_server.h:187 */
int wl_server_client_count(void);

/* ── src/gui/wayland/core/wl_wire.h ──────────────────────────────── */
/* 真实声明位于 src/gui/wayland/core/wl_wire.h */
#include "gui/wayland/core/wl_wire.h"

/* 见 src/gui/wayland/core/wl_wire.h:55 */
uint32_t wl_arg_u32(wl_args_t *args);
/* 见 src/gui/wayland/core/wl_wire.h:56 */
int32_t wl_arg_i32(wl_args_t *args);
/* 见 src/gui/wayland/core/wl_wire.h:58 */
int wl_arg_string(wl_args_t *args, char *buf, uint32_t capacity);
/* 见 src/gui/wayland/core/wl_wire.h:60 */
void wl_arg_skip_string(wl_args_t *args);
/* 见 src/gui/wayland/core/wl_wire.h:61 */
void wl_arg_skip_u32(wl_args_t *args, int count);
/* 见 src/gui/wayland/core/wl_wire.h:62 */
int wl_args_failed(const wl_args_t *args);
/* 见 src/gui/wayland/core/wl_wire.h:72 */
int wl_message_parse(const uint8_t *data, uint32_t available, wl_message_header_t *header);
/* 见 src/gui/wayland/core/wl_wire.h:89 */
void wl_outbuf_init(wl_outbuf_t *out, uint8_t *buffer, uint32_t capacity);
/* 见 src/gui/wayland/core/wl_wire.h:90 */
int wl_out_begin(wl_outbuf_t *out, uint32_t object_id, uint16_t opcode);
/* 见 src/gui/wayland/core/wl_wire.h:91 */
void wl_out_u32(wl_outbuf_t *out, uint32_t value);
/* 见 src/gui/wayland/core/wl_wire.h:92 */
void wl_out_i32(wl_outbuf_t *out, int32_t value);
/* 见 src/gui/wayland/core/wl_wire.h:93 */
void wl_out_string(wl_outbuf_t *out, const char *text);
/* 见 src/gui/wayland/core/wl_wire.h:94 */
void wl_out_end(wl_outbuf_t *out);
/* 见 src/gui/wayland/core/wl_wire.h:95 */
void wl_out_abort(wl_outbuf_t *out);

/* ── src/gui/wayland/iface/wl_iface.h ──────────────────────────────── */
/* 真实声明位于 src/gui/wayland/iface/wl_iface.h */
#include "gui/wayland/iface/wl_iface.h"

/* 见 src/gui/wayland/iface/wl_iface.h:90 */
void wl_iface_init(void);
/* 见 src/gui/wayland/iface/wl_iface.h:94 */
wl_surface_state_t *wl_surface_alloc(void);
/* 见 src/gui/wayland/iface/wl_iface.h:95 */
void wl_surface_free(wl_surface_state_t *surface);
/* 见 src/gui/wayland/iface/wl_iface.h:96 */
wl_shm_buffer_t *wl_buffer_alloc(void);
/* 见 src/gui/wayland/iface/wl_iface.h:97 */
void wl_buffer_free(wl_shm_buffer_t *buffer);
/* 见 src/gui/wayland/iface/wl_iface.h:98 */
wl_shm_pool_state_t *wl_pool_alloc(void);
/* 见 src/gui/wayland/iface/wl_iface.h:99 */
void wl_pool_free(wl_shm_pool_state_t *pool);
/* *  返回 0 成功；非 0 表示这块 buffer 读不出来（fd 无效/越界）。  ── 见 src/gui/wayland/iface/wl_iface.h:103 */
int wl_buffer_read(const wl_shm_buffer_t *buffer, void *dst, int dst_stride, int x, int y, int w, int h);
/* 见 src/gui/wayland/iface/wl_iface.h:108 */
int wl_iface_surface_count(void);
/* 见 src/gui/wayland/iface/wl_iface.h:109 */
wl_surface_state_t *wl_iface_surface_at(int index);
/* 见 src/gui/wayland/iface/wl_iface.h:113 */
void wl_display_request(wl_client_t *client, wl_object_t *object, uint16_t opcode, wl_args_t *args);
/* 见 src/gui/wayland/iface/wl_iface.h:115 */
void wl_registry_request(wl_client_t *client, wl_object_t *object, uint16_t opcode, wl_args_t *args);
/* 见 src/gui/wayland/iface/wl_iface.h:117 */
void wl_compositor_request(wl_client_t *client, wl_object_t *object, uint16_t opcode, wl_args_t *args);
/* 见 src/gui/wayland/iface/wl_iface.h:119 */
void wl_surface_request(wl_client_t *client, wl_object_t *object, uint16_t opcode, wl_args_t *args);
/* 见 src/gui/wayland/iface/wl_iface.h:121 */
void wl_region_request(wl_client_t *client, wl_object_t *object, uint16_t opcode, wl_args_t *args);
/* 见 src/gui/wayland/iface/wl_iface.h:123 */
void wl_shm_request(wl_client_t *client, wl_object_t *object, uint16_t opcode, wl_args_t *args);
/* 见 src/gui/wayland/iface/wl_iface.h:125 */
void wl_shm_pool_request(wl_client_t *client, wl_object_t *object, uint16_t opcode, wl_args_t *args);
/* 见 src/gui/wayland/iface/wl_iface.h:127 */
void wl_buffer_request(wl_client_t *client, wl_object_t *object, uint16_t opcode, wl_args_t *args);
/* 见 src/gui/wayland/iface/wl_iface.h:129 */
void wl_seat_request(wl_client_t *client, wl_object_t *object, uint16_t opcode, wl_args_t *args);
/* 见 src/gui/wayland/iface/wl_iface.h:131 */
void wl_input_device_request(wl_client_t *client, wl_object_t *object, uint16_t opcode, wl_args_t *args);
/* 见 src/gui/wayland/iface/wl_iface.h:133 */
void wl_xdg_wm_base_request(wl_client_t *client, wl_object_t *object, uint16_t opcode, wl_args_t *args);
/* 见 src/gui/wayland/iface/wl_iface.h:135 */
void wl_xdg_surface_request(wl_client_t *client, wl_object_t *object, uint16_t opcode, wl_args_t *args);
/* 见 src/gui/wayland/iface/wl_iface.h:137 */
void wl_xdg_toplevel_request(wl_client_t *client, wl_object_t *object, uint16_t opcode, wl_args_t *args);
/* 见 src/gui/wayland/iface/wl_iface.h:139 */
void wl_xdg_popup_request(wl_client_t *client, wl_object_t *object, uint16_t opcode, wl_args_t *args);
/* 见 src/gui/wayland/iface/wl_iface.h:143 */
void wl_surface_send_configure(wl_client_t *client, wl_surface_state_t *surface);

/* ── src/gui/wayland/wl_protocol.h ──────────────────────────────── */
/* 真实声明位于 src/gui/wayland/wl_protocol.h */
#include "gui/wayland/wl_protocol.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/gui/wayland/wl_protocol.h */

/* ── src/gui/winsrv.h ──────────────────────────────── */
/* 真实声明位于 src/gui/winsrv.h */
#include "gui/winsrv.h"

/* 见 src/gui/winsrv.h:118 */
int winsrv_create(uint32_t owner_task, const char *title, int w, int h);
/* 见 src/gui/winsrv.h:120 */
int winsrv_create_handle(uint32_t owner_task, const char *title, int w, int h);
/* 见 src/gui/winsrv.h:123 */
void winsrv_destroy(int id);
/* 见 src/gui/winsrv.h:126 */
void winsrv_close_for_task(uint32_t owner_task);
/* 见 src/gui/winsrv.h:129 */
winsrv_window_t *winsrv_get(int id);
/* 见 src/gui/winsrv.h:132 */
winsrv_window_t *winsrv_for_task(uint32_t owner_task);
/* 见 src/gui/winsrv.h:134 */
winsrv_window_t *winsrv_for_handle(uint32_t owner_task, int handle);
/* 见 src/gui/winsrv.h:135 */
int winsrv_handle(const winsrv_window_t *win);
/* 见 src/gui/winsrv.h:138 */
int winsrv_count(void);
/* 见 src/gui/winsrv.h:139 */
int winsrv_has_dirty(void);
/* 见 src/gui/winsrv.h:142 */
int winsrv_reap_dead(void);
/* ── 表面绘制原语（并发安全，写入窗口自有表面） ─────────────  ── 见 src/gui/winsrv.h:145 */
void winsrv_clear(winsrv_window_t *win, uint32_t color);
/* 见 src/gui/winsrv.h:146 */
void winsrv_fill (winsrv_window_t *win, int x, int y, int w, int h, uint32_t color);
/* 见 src/gui/winsrv.h:147 */
void winsrv_text (winsrv_window_t *win, int x, int y, const char *s, uint32_t color);
/* 见 src/gui/winsrv.h:148 */
void winsrv_blit_argb(winsrv_window_t *win, int x, int y, int w, int h, const uint32_t *pixels, int stride);
/* 见 src/gui/winsrv.h:150 */
int winsrv_resize(winsrv_window_t *win, int w, int h);
/* 见 src/gui/winsrv.h:151 */
void winsrv_set_title(winsrv_window_t *win, const char *title);
/* 见 src/gui/winsrv.h:152 */
void winsrv_mark_dirty(winsrv_window_t *win, int x, int y, int w, int h);
/* ── 事件队列 ───────────────────────────────────────────────  ── 见 src/gui/winsrv.h:155 */
void winsrv_push_event(winsrv_window_t *win, int type, int a, int b, int c);
/* 见 src/gui/winsrv.h:157 */
int winsrv_pop_event(winsrv_window_t *win, winsrv_event_t *ev);

/* ── src/gui/wl/wl_compositor.h ──────────────────────────────── */
/* 真实声明位于 src/gui/wl/wl_compositor.h */
#include "gui/wl/wl_compositor.h"

/* 见 src/gui/wl/wl_compositor.h:54 */
wl_compositor_t *wl_comp_create(wl_blit_fn blit, void *ctx);
/* 见 src/gui/wl/wl_compositor.h:57 */
int wl_comp_add(wl_compositor_t *c, wl_surface_t *s, int x, int y, int z);
/* 见 src/gui/wl/wl_compositor.h:58 */
int wl_comp_remove(wl_compositor_t *c, wl_surface_t *s);
/* 见 src/gui/wl/wl_compositor.h:59 */
int wl_comp_move(wl_compositor_t *c, wl_surface_t *s, int x, int y);
/* 见 src/gui/wl/wl_compositor.h:60 */
int wl_comp_set_visible(wl_compositor_t *c, wl_surface_t *s, int visible);
/* 见 src/gui/wl/wl_compositor.h:61 */
int wl_comp_raise(wl_compositor_t *c, wl_surface_t *s);
/* 见 src/gui/wl/wl_compositor.h:64 */
wl_surface_t *wl_comp_hit(const wl_compositor_t *c, int x, int y);
/* 见 src/gui/wl/wl_compositor.h:67 */
int wl_comp_paint(wl_compositor_t *c);
/* 见 src/gui/wl/wl_compositor.h:69 */
int wl_comp_count(const wl_compositor_t *c);

/* ── src/gui/wl/wl_seat.h ──────────────────────────────── */
/* 真实声明位于 src/gui/wl/wl_seat.h */
#include "gui/wl/wl_seat.h"

/* 见 src/gui/wl/wl_seat.h:79 */
wl_seat_t *wl_seat_create(void);
/* 见 src/gui/wl/wl_seat.h:80 */
void wl_seat_destroy(wl_seat_t *s);
/* 见 src/gui/wl/wl_seat.h:81 */
void wl_seat_add_listener(wl_seat_t *s, wl_event_fn fn, void *ctx);
/* 见 src/gui/wl/wl_seat.h:84 */
void wl_seat_set_pointer_focus(wl_seat_t *s, int surface_id);
/* 见 src/gui/wl/wl_seat.h:86 */
void wl_seat_set_keyboard_focus(wl_seat_t *s, int surface_id);
/* 见 src/gui/wl/wl_seat.h:88 */
int wl_seat_pointer_motion(wl_seat_t *s, int x, int y);
/* 见 src/gui/wl/wl_seat.h:89 */
int wl_seat_pointer_button(wl_seat_t *s, int button, int pressed);
/* 见 src/gui/wl/wl_seat.h:90 */
int wl_seat_pointer_axis(wl_seat_t *s, int axis);
/* 见 src/gui/wl/wl_seat.h:91 */
int wl_seat_key(wl_seat_t *s, uint32_t key, int pressed);
/* 见 src/gui/wl/wl_seat.h:94 */
int wl_seat_dispatch(wl_seat_t *s, int max_events);
/* 见 src/gui/wl/wl_seat.h:95 */
int wl_seat_pending(const wl_seat_t *s);
/* 见 src/gui/wl/wl_seat.h:96 */
uint32_t wl_seat_dropped(const wl_seat_t *s);

/* ── src/gui/wl/wl_surface.h ──────────────────────────────── */
/* 真实声明位于 src/gui/wl/wl_surface.h */
#include "gui/wl/wl_surface.h"

/* 见 src/gui/wl/wl_surface.h:71 */
wl_surface_t *wl_surface_create(int w, int h);
/* 见 src/gui/wl/wl_surface.h:72 */
void wl_surface_destroy(wl_surface_t *s);
/* 见 src/gui/wl/wl_surface.h:74 */
void wl_surface_set_commit_listener(wl_surface_t *s, wl_commit_fn fn, void *ctx);
/* 见 src/gui/wl/wl_surface.h:77 */
int wl_surface_attach(wl_surface_t *s, wl_buffer_t *buf);
/* 见 src/gui/wl/wl_surface.h:80 */
void wl_surface_damage(wl_surface_t *s, int x, int y, int w, int h);
/* 见 src/gui/wl/wl_surface.h:83 */
int wl_surface_commit(wl_surface_t *s);
/* 见 src/gui/wl/wl_surface.h:85 */
wl_buffer_t *wl_surface_current(const wl_surface_t *s);
/* 见 src/gui/wl/wl_surface.h:86 */
int wl_surface_is_mapped(const wl_surface_t *s);
/* 见 src/gui/wl/wl_surface.h:87 */
uint32_t wl_surface_serial(const wl_surface_t *s);

/* ── src/gui/wm.h ──────────────────────────────── */
/* 真实声明位于 src/gui/wm.h */
#include "gui/wm.h"

/* 返回 v 缩放后的像素值  ── 见 src/gui/wm.h:102 */
int ui_s(int v);
/* 取窗口当前层级；不存在返回 0。  ── 见 src/gui/wm.h:204 */
uint32_t wm_window_layer(wm_state_t *wm, int idx);
/* * 但不会低于 0，也不会超过撞击者。  ── 见 src/gui/wm.h:208 */
uint32_t wm_knock_down(wm_state_t *wm, int idx, uint32_t target_layer);
/* * 的语义——视觉顺序来自 layer 的排序，交换即换位。  ── 见 src/gui/wm.h:216 */
int wm_sink_one(wm_state_t *wm, int idx);
/* 每帧调用：递减撞击冷却/高亮帧数。  ── 见 src/gui/wm.h:218 */
void wm_tick_knock(wm_state_t *wm);
/* 两个窗口矩形是否相交（用于「撞击」判定）。  ── 见 src/gui/wm.h:220 */
int wm_windows_collide(wm_state_t *wm, int a, int b);
/* 见 src/gui/wm.h:222 */
void wm_init(wm_state_t *wm, int desk_w, int desk_h);
/* 见 src/gui/wm.h:223 */
void wm_set_panel_title(int panel, const char *title);
/* 见 src/gui/wm.h:224 */
void wm_set_app_title(int mode, const char *title);
/* 见 src/gui/wm.h:225 */
const char *wm_window_title_by_mode(int mode);
/* 见 src/gui/wm.h:226 */
int wm_open_window(wm_state_t *wm, int kind, int mode, int unique);
/* 见 src/gui/wm.h:227 */
void wm_close_window(wm_state_t *wm, int idx);
/* 见 src/gui/wm.h:228 */
void wm_focus_window(wm_state_t *wm, int idx);
/* 见 src/gui/wm.h:229 */
void wm_focus_next(wm_state_t *wm, int dir);
/* 见 src/gui/wm.h:230 */
void wm_move_window(wm_state_t *wm, int idx, int x, int y);
/* 见 src/gui/wm.h:231 */
void wm_resize_window(wm_state_t *wm, int idx, int w, int h);
/* 见 src/gui/wm.h:232 */
void wm_minimize_window(wm_state_t *wm, int idx);
/* 见 src/gui/wm.h:233 */
void wm_maximize_window(wm_state_t *wm, int idx);
/* 见 src/gui/wm.h:234 */
void wm_restore_window(wm_state_t *wm, int idx);
/* * 指针锚点，再调 wm_restore_window()，避免二次读状态拿到旧几何。  ── 见 src/gui/wm.h:237 */
void wm_restore_target(const wm_state_t *wm, int idx, int *x, int *y, int *w, int *h);
/* 见 src/gui/wm.h:238 */
void wm_toggle_maximize(wm_state_t *wm, int idx);
/* 见 src/gui/wm.h:239 */
void wm_snap_window(wm_state_t *wm, int idx, int side);
/* 从吸附态退出：恢复到吸附前的几何。返回 1 表示确实做了还原。  ── 见 src/gui/wm.h:241 */
int wm_unsnap_window(wm_state_t *wm, int idx);
/* 见 src/gui/wm.h:242 */
void wm_toggle_start_menu(wm_state_t *wm);
/* 见 src/gui/wm.h:243 */
void wm_close_start_menu(wm_state_t *wm);
/* 见 src/gui/wm.h:245 */
wm_window_t *wm_get_window(wm_state_t *wm, int idx);
/* 见 src/gui/wm.h:246 */
wm_window_t *wm_get_active(wm_state_t *wm);
/* 见 src/gui/wm.h:247 */
const char *wm_window_title(wm_window_t *win);
/* 见 src/gui/wm.h:248 */
void wm_get_window_rect(wm_state_t *wm, int idx, int *x, int *y, int *w, int *h);
/* 见 src/gui/wm.h:249 */
int wm_hit_titlebar(wm_state_t *wm, int mx, int my);
/* 见 src/gui/wm.h:250 */
int wm_hit_close(wm_state_t *wm, int mx, int my);
/* 见 src/gui/wm.h:251 */
int wm_hit_minimize(wm_state_t *wm, int mx, int my);
/* 见 src/gui/wm.h:252 */
int wm_hit_maximize(wm_state_t *wm, int mx, int my);
/* 见 src/gui/wm.h:253 */
int wm_hit_border(wm_state_t *wm, int mx, int my, int *edge);
/* 见 src/gui/wm.h:254 */
int wm_hit_start_menu(wm_state_t *wm, int mx, int my);
/* 见 src/gui/wm.h:255 */
void wm_update_animations(wm_state_t *wm);
/* 见 src/gui/wm.h:256 */
int wm_has_active_animations(wm_state_t *wm);

/* ── src/https.h ──────────────────────────────── */
/* 真实声明位于 src/https.h */
#include "https.h"

/* 见 src/https.h:41 */
int secure_https_get(const hbos_https_request_v2_t *request, uint32_t *output_length);
/* 见 src/https.h:43 */
const char *secure_https_last_error(void);

/* ── src/inflate.h ──────────────────────────────── */
/* 真实声明位于 src/inflate.h */
#include "inflate.h"

/* 见 src/inflate.h:37 */
int inflate_raw(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap, uint32_t *out_len);
/* 见 src/inflate.h:44 */
int inflate_zlib(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap, uint32_t *out_len);

/* ── src/input/mouse.h ──────────────────────────────── */
/* 真实声明位于 src/input/mouse.h */
#include "input/mouse.h"

/* 见 src/input/mouse.h:33 */
int mouse_init(void);
/* 见 src/input/mouse.h:34 */
void mouse_shutdown(void);
/* 见 src/input/mouse.h:35 */
int mouse_poll(mouse_event_t *ev);
/* 见 src/input/mouse.h:36 */
int mouse_is_ready(void);
/* 见 src/input/mouse.h:37 */
const char *mouse_backend_name(void);
/* 见 src/input/mouse.h:38 */
void ps2_mouse_enqueue_byte(uint8_t b);

/* ── src/ipc.h ──────────────────────────────── */
/* 真实声明位于 src/ipc.h */
#include "ipc.h"

/* 见 src/ipc.h:61 */
void ipc_init(void);
/* 见 src/ipc.h:62 */
int shmget(int key, size_t size, int flags);
/* 见 src/ipc.h:63 */
void *shmat(int shmid, const void *shmaddr, int flags);
/* 见 src/ipc.h:64 */
int shmdt(const void *shmaddr);
/* 见 src/ipc.h:65 */
int shmctl(int shmid, int cmd, void *buf);

/* ── src/jpeg.h ──────────────────────────────── */
/* 真实声明位于 src/jpeg.h */
#include "jpeg.h"

/* 见 src/jpeg.h:30 */
int jpeg_decode(const uint8_t *data, uint32_t size, uint8_t *out_rgb, uint32_t out_cap, int max_w, int max_h, int *out_w, int *out_h);

/* ── src/lib/ext4_extent.h ──────────────────────────────── */
/* 真实声明位于 src/lib/ext4_extent.h */
#include "lib/ext4_extent.h"

/* 见 src/lib/ext4_extent.h:34 */
uint32_t ext4_extent_map_raw(ext4_ext_read_fn rd, void *ctx, const uint8_t i_block[60], uint32_t block_size, uint32_t logical);

/* ── src/lib/json.h ──────────────────────────────── */
/* 真实声明位于 src/lib/json.h */
#include "lib/json.h"

/* 见 src/lib/json.h:91 */
json_doc_t *json_alloc(void);
/* 见 src/lib/json.h:92 */
void json_free(json_doc_t *doc);
/* 见 src/lib/json.h:98 */
void json_reset(json_doc_t *doc);
/* 见 src/lib/json.h:109 */
bool json_parse_object(const char *text, uint32_t len, json_doc_t *out);
/* 见 src/lib/json.h:112 */
const char *json_get_str(const json_doc_t *doc, const char *key);
/* 见 src/lib/json.h:115 */
int32_t json_get_num1000(const json_doc_t *doc, const char *key, int32_t fallback);
/* 见 src/lib/json.h:118 */
bool json_get_bool(const json_doc_t *doc, const char *key, bool fallback);
/* 见 src/lib/json.h:121 */
int json_get_int(const json_doc_t *doc, const char *key, int fallback);
/* 见 src/lib/json.h:128 */
bool json_parse_color(const char *s, uint32_t *out);

/* ── src/lib/utf8.h ──────────────────────────────── */
/* 真实声明位于 src/lib/utf8.h */
#include "lib/utf8.h"

/* 见 src/lib/utf8.h:43 */
int utf8_decode(const char *s, uint32_t len, uint32_t *cp);
/* 见 src/lib/utf8.h:51 */
int utf8_encode(uint32_t cp, char *out);
/* 见 src/lib/utf8.h:54 */
int utf8_valid(const char *s, uint32_t len);
/* 见 src/lib/utf8.h:57 */
uint32_t utf8_length(const char *s, uint32_t len);

/* ── src/libc/assert.h ──────────────────────────────── */
/* 真实声明位于 src/libc/assert.h */
#include "libc/assert.h"

/* 见 src/libc/assert.h:22 */
void __assert_fail(const char *expr, const char *file, int line, const char *func);

/* ── src/libc/ctype.h ──────────────────────────────── */
/* 真实声明位于 src/libc/ctype.h */
#include "libc/ctype.h"

/* 见 src/libc/ctype.h:31 */
int isalnum(int c);
/* 见 src/libc/ctype.h:32 */
int isalpha(int c);
/* 见 src/libc/ctype.h:33 */
int isblank(int c);
/* 见 src/libc/ctype.h:34 */
int iscntrl(int c);
/* 见 src/libc/ctype.h:35 */
int isdigit(int c);
/* 见 src/libc/ctype.h:36 */
int isgraph(int c);
/* 见 src/libc/ctype.h:37 */
int islower(int c);
/* 见 src/libc/ctype.h:38 */
int isprint(int c);
/* 见 src/libc/ctype.h:39 */
int ispunct(int c);
/* 见 src/libc/ctype.h:40 */
int isspace(int c);
/* 见 src/libc/ctype.h:41 */
int isupper(int c);
/* 见 src/libc/ctype.h:42 */
int isxdigit(int c);
/* 见 src/libc/ctype.h:44 */
int toupper(int c);
/* 见 src/libc/ctype.h:45 */
int tolower(int c);

/* ── src/libc/stdio.h ──────────────────────────────── */
/* 真实声明位于 src/libc/stdio.h */
#include "libc/stdio.h"

/* 见 src/libc/stdio.h:85 */
FILE *fopen(const char *path, const char *mode);
/* 见 src/libc/stdio.h:86 */
int fclose(FILE *fp);
/* 见 src/libc/stdio.h:87 */
int fflush(FILE *fp);
/* 见 src/libc/stdio.h:88 */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
/* 见 src/libc/stdio.h:89 */
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
/* 见 src/libc/stdio.h:90 */
int fseek(FILE *fp, long offset, int whence);
/* 见 src/libc/stdio.h:91 */
long ftell(FILE *fp);
/* 见 src/libc/stdio.h:92 */
int fgetc(FILE *fp);
/* 见 src/libc/stdio.h:93 */
int fputc(int c, FILE *fp);
/* 见 src/libc/stdio.h:94 */
int ungetc(int c, FILE *fp);
/* 见 src/libc/stdio.h:95 */
char *fgets(char *s, int size, FILE *fp);
/* 见 src/libc/stdio.h:96 */
int fputs(const char *s, FILE *fp);
/* 见 src/libc/stdio.h:97 */
int feof(FILE *fp);
/* 见 src/libc/stdio.h:98 */
int ferror(FILE *fp);
/* 见 src/libc/stdio.h:99 */
void clearerr(FILE *fp);
/* 见 src/libc/stdio.h:100 */
int fileno(FILE *fp);
/* 见 src/libc/stdio.h:102 */
int printf(const char *fmt, ...);
/* 见 src/libc/stdio.h:103 */
int fprintf(FILE *fp, const char *fmt, ...);
/* 见 src/libc/stdio.h:104 */
int sprintf(char *buf, const char *fmt, ...);
/* 见 src/libc/stdio.h:105 */
int snprintf(char *buf, size_t n, const char *fmt, ...);
/* 见 src/libc/stdio.h:106 */
int vprintf(const char *fmt, va_list ap);
/* 见 src/libc/stdio.h:107 */
int vfprintf(FILE *fp, const char *fmt, va_list ap);
/* 见 src/libc/stdio.h:108 */
int vsprintf(char *buf, const char *fmt, va_list ap);
/* 见 src/libc/stdio.h:109 */
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
/* 见 src/libc/stdio.h:111 */
int scanf(const char *fmt, ...);
/* 见 src/libc/stdio.h:112 */
int fscanf(FILE *fp, const char *fmt, ...);
/* 见 src/libc/stdio.h:113 */
int sscanf(const char *s, const char *fmt, ...);
/* 见 src/libc/stdio.h:115 */
int getchar(void);
/* 见 src/libc/stdio.h:116 */
int putchar(int c);
/* 见 src/libc/stdio.h:117 */
int puts(const char *s);
/* 见 src/libc/stdio.h:118 */
int getc(FILE *fp);
/* 见 src/libc/stdio.h:119 */
int putc(int c, FILE *fp);
/* 见 src/libc/stdio.h:121 */
void perror(const char *s);
/* 见 src/libc/stdio.h:122 */
int remove(const char *path);
/* 见 src/libc/stdio.h:123 */
int rename(const char *old, const char *new);

/* ── src/libc/stdlib.h ──────────────────────────────── */
/* 真实声明位于 src/libc/stdlib.h */
#include "libc/stdlib.h"

/* 见 src/libc/stdlib.h:45 */
void *malloc(size_t size);
/* 见 src/libc/stdlib.h:46 */
void free(void *ptr);
/* 见 src/libc/stdlib.h:47 */
void *calloc(size_t nmemb, size_t size);
/* 见 src/libc/stdlib.h:48 */
void *realloc(void *ptr, size_t size);
/* 见 src/libc/stdlib.h:50 */
int atoi(const char *s);
/* 见 src/libc/stdlib.h:51 */
long atol(const char *s);
/* 见 src/libc/stdlib.h:52 */
long long atoll(const char *s);
/* 见 src/libc/stdlib.h:53 */
double atof(const char *s);
/* 见 src/libc/stdlib.h:54 */
long strtol(const char *s, char **endptr, int base);
/* 见 src/libc/stdlib.h:55 */
unsigned long strtoul(const char *s, char **endptr, int base);
/* 见 src/libc/stdlib.h:57 */
void abort(void);
/* 见 src/libc/stdlib.h:58 */
void exit(int status);
/* 见 src/libc/stdlib.h:59 */
int atexit(void (*func)(void));
/* 见 src/libc/stdlib.h:61 */
int system(const char *cmd);
/* 见 src/libc/stdlib.h:62 */
char *getenv(const char *name);
/* 见 src/libc/stdlib.h:63 */
int setenv(const char *name, const char *value, int overwrite);
/* 见 src/libc/stdlib.h:65 */
int rand(void);
/* 见 src/libc/stdlib.h:66 */
void srand(unsigned int seed);
/* 见 src/libc/stdlib.h:68 */
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
/* 见 src/libc/stdlib.h:70 */
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
/* 见 src/libc/stdlib.h:73 */
int abs(int n);
/* 见 src/libc/stdlib.h:74 */
long labs(long n);

/* ── src/libc/string.h ──────────────────────────────── */
/* 真实声明位于 src/libc/string.h */
#include "libc/string.h"

/* 见 src/libc/string.h:37 */
void *memset(void *s, int c, size_t n);
/* 见 src/libc/string.h:38 */
void *memcpy(void *dest, const void *src, size_t n);
/* 见 src/libc/string.h:39 */
void *memmove(void *dest, const void *src, size_t n);
/* 见 src/libc/string.h:40 */
int memcmp(const void *s1, const void *s2, size_t n);
/* 见 src/libc/string.h:41 */
void *memchr(const void *s, int c, size_t n);
/* 见 src/libc/string.h:43 */
size_t strlen(const char *s);
/* 见 src/libc/string.h:44 */
size_t strnlen(const char *s, size_t maxlen);
/* 见 src/libc/string.h:45 */
char *strcpy(char *dest, const char *src);
/* 见 src/libc/string.h:46 */
char *strncpy(char *dest, const char *src, size_t n);
/* 见 src/libc/string.h:47 */
char *strcat(char *dest, const char *src);
/* 见 src/libc/string.h:48 */
char *strncat(char *dest, const char *src, size_t n);
/* 见 src/libc/string.h:49 */
int strcmp(const char *s1, const char *s2);
/* 见 src/libc/string.h:50 */
int strncmp(const char *s1, const char *s2, size_t n);
/* 见 src/libc/string.h:51 */
char *strchr(const char *s, int c);
/* 见 src/libc/string.h:52 */
char *strrchr(const char *s, int c);
/* 见 src/libc/string.h:53 */
char *strstr(const char *haystack, const char *needle);
/* 见 src/libc/string.h:54 */
char *strdup(const char *s);
/* 见 src/libc/string.h:55 */
char *strtok(char *str, const char *delim);

/* ── src/limine.h ──────────────────────────────── */
/* 真实声明位于 src/limine.h */
#include "limine.h"

/* 见 src/limine.h:528 */
struct LIMINE_MP(info);
/* inline 定义 LIMINE_MP  ── 见 src/limine.h:548 */
/* inline 定义 LIMINE_MP  ── 见 src/limine.h:608 */

/* ── src/linux_compat.h ──────────────────────────────── */
/* 真实声明位于 src/linux_compat.h */
#include "linux_compat.h"

/* 见 src/linux_compat.h:152 */
long linux_compat_read(int fd, void *buffer, size_t count);
/* 见 src/linux_compat.h:153 */
long linux_compat_write(int fd, const void *buffer, size_t count);
/* 见 src/linux_compat.h:154 */
int linux_compat_close(int fd);
/* 见 src/linux_compat.h:155 */
void linux_compat_retain(int fd);
/* 按 fd 条目直接调整 pipe 引用计数（fork 复制 fd 表时用，见 task.c）。  ── 见 src/linux_compat.h:157 */
void linux_compat_retain_entry(fd_entry_t *entry);
/* 见 src/linux_compat.h:158 */
void linux_compat_release_entry(fd_entry_t *entry);
/* 读端阻塞判断用：除自己以外是否还有写端（见 posix.c 的 read()）。  ── 见 src/linux_compat.h:160 */
int posix_pipe_write_end_open(void *pipe);
/* 见 src/linux_compat.h:161 */
void linux_compat_retain_task(struct task *task);
/* 见 src/linux_compat.h:162 */
void linux_compat_release_task(struct task *task);
/* 见 src/linux_compat.h:164 */
int linux_compat_poll(linux_pollfd_t *fds, uint32_t count, int timeout_ms);
/* 见 src/linux_compat.h:165 */
int linux_compat_pipe2(int pipefd[2], int flags);
/* 见 src/linux_compat.h:166 */
int linux_compat_eventfd2(uint32_t initial_value, int flags);
/* 见 src/linux_compat.h:167 */
int linux_compat_epoll_create1(int flags);
/* 见 src/linux_compat.h:168 */
int linux_compat_epoll_ctl(int epfd, int operation, int fd, const linux_epoll_event_t *event);
/* 见 src/linux_compat.h:170 */
int linux_compat_epoll_wait(int epfd, linux_epoll_event_t *events, int max_events, int timeout_ms);
/* 见 src/linux_compat.h:172 */
int linux_compat_inotify_init1(int flags);
/* 见 src/linux_compat.h:173 */
int linux_compat_inotify_add_watch(int fd, const char *path, uint32_t mask);
/* 见 src/linux_compat.h:174 */
int linux_compat_inotify_rm_watch(int fd, int watch_descriptor);
/* 见 src/linux_compat.h:175 */
void linux_compat_inotify_notify(const char *path, uint32_t mask, int is_directory);
/* 见 src/linux_compat.h:177 */
void linux_compat_inotify_move(const char *old_path, const char *new_path, int is_directory);
/* 见 src/linux_compat.h:179 */
void linux_compat_inotify_replace_target(const char *path, int is_directory);
/* 见 src/linux_compat.h:181 */
long linux_compat_getrandom(void *buffer, size_t count, unsigned int flags);
/* 见 src/linux_compat.h:182 */
int linux_compat_futex(uint32_t *address, int operation, uint32_t value, const void *timeout);
/* 见 src/linux_compat.h:184 */
int linux_compat_futex6(uint32_t *address, int operation, uint32_t value, const void *timeout, uint32_t *address2, uint32_t bitset);
/* 见 src/linux_compat.h:188 */
int linux_compat_unix_socket(int type, int protocol);
/* 见 src/linux_compat.h:189 */
int linux_compat_unix_socketpair(int type, int protocol, int pair[2]);
/* 见 src/linux_compat.h:190 */
int linux_compat_unix_bind(int fd, const void *address, size_t length);
/* 见 src/linux_compat.h:191 */
int linux_compat_unix_listen(int fd, int backlog);
/* 见 src/linux_compat.h:192 */
int linux_compat_unix_accept(int fd, void *address, uint32_t *length);
/* 见 src/linux_compat.h:193 */
int linux_compat_unix_connect(int fd, const void *address, size_t length);
/* 见 src/linux_compat.h:194 */
int linux_compat_unix_getsockname(int fd, void *address, uint32_t *length);
/* 见 src/linux_compat.h:195 */
int linux_compat_unix_getpeername(int fd, void *address, uint32_t *length);
/* 见 src/linux_compat.h:196 */
long linux_compat_unix_send(int fd, const void *buffer, size_t count, int flags);
/* 见 src/linux_compat.h:198 */
long linux_compat_unix_recv(int fd, void *buffer, size_t count, int flags);
/* 见 src/linux_compat.h:199 */
int linux_compat_unix_getsockopt(int fd, int level, int option, void *value, uint32_t *length);
/* 见 src/linux_compat.h:201 */
int linux_compat_unix_setsockopt(int fd, int level, int option, const void *value, uint32_t length);
/* 见 src/linux_compat.h:203 */
int linux_compat_unix_shutdown(int fd, int how);
/* 见 src/linux_compat.h:204 */
int linux_compat_unix_send_rights(int fd, const int *fds, size_t count);
/* 见 src/linux_compat.h:205 */
int linux_compat_unix_recv_rights(int fd, int *fds, size_t capacity, size_t *received, int *truncated);
/* /dev/shm 内存后备（见 linux_compat.c 的实现注释）。  ── 见 src/linux_compat.h:209 */
int linux_compat_shm_is_path(const char *path);
/* 见 src/linux_compat.h:210 */
int linux_compat_shm_open(const char *path, int flags);
/* 见 src/linux_compat.h:211 */
int linux_compat_shm_stat(const char *path, uint64_t *size);
/* 见 src/linux_compat.h:212 */
int linux_compat_shm_unlink(const char *path);
/* 见 src/linux_compat.h:213 */
int linux_compat_memfd_create(const char *name, unsigned int flags);
/* 见 src/linux_compat.h:214 */
int linux_compat_memfd_truncate(int fd, uint64_t size);
/* 见 src/linux_compat.h:215 */
int linux_compat_memfd_size(int fd, uint64_t *size);
/* 见 src/linux_compat.h:216 */
int linux_compat_memfd_read_at(int fd, uint64_t offset, void *buffer, size_t count);
/* * slot 扩容到 offset+count。软件光栅/共享位图正是用 pwrite 写共享内存。  ── 见 src/linux_compat.h:220 */
int linux_compat_memfd_write_at(int fd, uint64_t offset, const void *buffer, size_t count);
/* 见 src/linux_compat.h:222 */
int linux_compat_memfd_map(int fd, uint64_t address, size_t length, uint64_t offset, int writable, uint32_t *backing_id);
/* 见 src/linux_compat.h:225 */
void linux_compat_memfd_unmap(uint32_t backing_id);
/* 见 src/linux_compat.h:226 */
int linux_compat_memfd_retain_map(uint32_t backing_id);
/* * 下面三个函数按 (文件节点, 文件内页号) 共享物理页并做引用计数。  ── 见 src/linux_compat.h:237 */
uint64_t linux_compat_shm_page(struct vfs_node *node, uint64_t page_index, void *private_data, int *created);
/* 见 src/linux_compat.h:239 */
uint64_t linux_compat_shm_lookup(struct vfs_node *node, uint64_t page_index);
/* 见 src/linux_compat.h:240 */
void linux_compat_shm_unmap(struct vfs_node *node, uint64_t backing_offset, uint64_t bytes);
/* 见 src/linux_compat.h:243 */
int linux_compat_timerfd_create(int clockid, int flags);
/* 见 src/linux_compat.h:244 */
int linux_compat_timerfd_settime(int fd, int flags, const void *new_value, void *old_value);
/* 见 src/linux_compat.h:246 */
int linux_compat_timerfd_gettime(int fd, void *curr_value);
/* 见 src/linux_compat.h:248 */
int linux_compat_signalfd(int fd, const void *sigset_ptr, int flags);
/* 见 src/linux_compat.h:249 */
int linux_compat_signalfd_consume(struct task *task, int sig);

/* ── src/net.h ──────────────────────────────── */
/* 真实声明位于 src/net.h */
#include "net.h"

/* 见 src/net.h:122 */
void net_init(void);
/* 见 src/net.h:125 */
const net_device_t *net_primary(void);
/* 见 src/net.h:128 */
const char *net_driver_name(net_driver_t driver);
/* 见 src/net.h:131 */
const char *net_last_error(void);
/* 见 src/net.h:139 */
int net_poll_frames(net_frame_callback_t callback, void *context, uint32_t spins);
/* 见 src/net.h:143 */
int net_dhcp(void);
/* 见 src/net.h:146 */
int net_configure(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns);
/* 见 src/net.h:149 */
int net_ping(uint32_t ip, uint32_t timeout_ms);
/* 见 src/net.h:152 */
int net_dns_resolve(const char *name, uint32_t *out_ip);
/* 见 src/net.h:155 */
int net_dns_resolve_ipv6(const char *name, uint8_t out_address[16]);
/* *  g_rtc_ntp_correction_sec（见 rtc_tz.h）。0 成功，-1 失败。  ── 见 src/net.h:159 */
int net_ntp_sync(const char *server);
/* 见 src/net.h:162 */
int net_tcp_connect(uint32_t ip, uint16_t port, net_tcp_conn_t *conn);
/* 见 src/net.h:165 */
int net_tcp_connect_start(uint32_t ip, uint16_t port, net_tcp_conn_t *conn);
/* 见 src/net.h:171 */
int net_tcp_finish_connect(net_tcp_conn_t *conn);
/* 见 src/net.h:174 */
int net_tcp_send(net_tcp_conn_t *conn, const uint8_t *data, uint32_t len);
/* 见 src/net.h:177 */
int net_tcp_recv(net_tcp_conn_t *conn, uint8_t *buf, uint32_t cap, uint32_t *len, uint32_t poll_rounds);
/* 见 src/net.h:180 */
void net_tcp_close(net_tcp_conn_t *conn);
/* 见 src/net.h:183 */
int net_tcp_listen(uint16_t port);
/* 见 src/net.h:186 */
int net_tcp_rx_available(const net_tcp_conn_t *conn);
/* 见 src/net.h:189 */
int net_tcp_conn_open(const net_tcp_conn_t *conn);
/* 见 src/net.h:192 */
void net_pump(void);
/* 见 src/net.h:195 */
void net_udp_queue(const uint8_t *pkt, uint16_t len);
/* 见 src/net.h:198 */
void net_udp_drain(int (*cb)(const uint8_t *, uint16_t, void *), void *arg);
/* 见 src/net.h:201 */
int net_pkt_is_udp(const uint8_t *pkt, uint16_t len);
/* 见 src/net.h:204 */
int net_tcp_accept(uint16_t port, net_tcp_conn_t *conn, uint32_t timeout_ms);
/* 见 src/net.h:207 */
int net_tcp_exchange(uint32_t ip, uint16_t port, const uint8_t *request, uint32_t request_len, uint8_t *response, uint32_t response_cap, uint32_t *response_len);
/* 见 src/net.h:211 */
int net_http_request(const char *method, const char *host, uint32_t ip, uint16_t port, const char *path, char *out, uint32_t out_cap, uint32_t *out_len);
/* 见 src/net.h:215 */
int net_http_get(const char *host, uint32_t ip, uint16_t port, const char *path, char *out, uint32_t out_cap, uint32_t *out_len);
/* 见 src/net.h:219 */
uint32_t net_parse_ipv4(const char *s);
/* 见 src/net.h:222 */
void net_ipv4_to_str(uint32_t ip, char out[16]);

/* ── src/net/nic_e1000e_ids.h ──────────────────────────────── */
/* 真实声明位于 src/net/nic_e1000e_ids.h */
#include "net/nic_e1000e_ids.h"

/* inline 定义 e1000e_model_name  ── 见 src/net/nic_e1000e_ids.h:169 */

/* ── src/net/nic_r8169.h ──────────────────────────────── */
/* 真实声明位于 src/net/nic_r8169.h */
#include "net/nic_r8169.h"

/* 见 src/net/nic_r8169.h:52 */
int r8169_probe(uint16_t vendor, uint16_t device);
/* 见 src/net/nic_r8169.h:58 */
const char *r8169_driver_name(void);
/* 见 src/net/nic_r8169.h:68 */
const char *r8169_device_name(void);
/* 见 src/net/nic_r8169.h:78 */
int r8169_device_mac_version(void);
/* 见 src/net/nic_r8169.h:109 */
int r8169_init_hw(const pci_device_t *dev, uint8_t mac[6], bool *mac_valid, bool *link_ready);
/* 见 src/net/nic_r8169.h:126 */
int r8169_send(const void *frame, uint16_t len);
/* 见 src/net/nic_r8169.h:141 */
int r8169_poll(int (*cb)(const uint8_t *pkt, uint16_t len, void *arg), void *arg, uint32_t spins);
/* 见 src/net/nic_r8169.h:153 */
int r8169_link_up(void);

/* ── src/net6.h ──────────────────────────────── */
/* 真实声明位于 src/net6.h */
#include "net6.h"

/* 见 src/net6.h:38 */
int net6_init(void);
/* 见 src/net6.h:41 */
int net6_tcp_connect(const char *address, uint16_t port, net6_tcp_conn_t *connection, uint32_t timeout_ms);
/* 见 src/net6.h:45 */
int net6_tcp_connect_address(const uint8_t address[16], uint16_t port, net6_tcp_conn_t *connection, uint32_t timeout_ms);
/* 见 src/net6.h:50 */
int net6_tcp_send(net6_tcp_conn_t *connection, const uint8_t *data, uint32_t length, uint32_t timeout_ms);
/* 见 src/net6.h:52 */
int net6_tcp_recv(net6_tcp_conn_t *connection, uint8_t *data, uint32_t capacity, uint32_t timeout_ms);
/* 见 src/net6.h:54 */
void net6_tcp_close(net6_tcp_conn_t *connection);
/* 见 src/net6.h:57 */
int net6_primary_address(char *out, size_t capacity);

/* ── src/net_virtio.h ──────────────────────────────── */
/* 真实声明位于 src/net_virtio.h */
#include "net_virtio.h"

/* 见 src/net_virtio.h:39 */
int virtio_net_init_hw(const pci_device_t *pdev, uint8_t mac[6], bool *mac_valid, bool *link);
/* 见 src/net_virtio.h:43 */
int virtio_net_send(const void *frame, uint16_t len);
/* 见 src/net_virtio.h:53 */
int virtio_net_poll(int (*cb)(const uint8_t *pkt, uint16_t len, void *arg), void *arg, uint32_t spins, uint64_t *rx_packets, uint64_t *rx_bytes, uint64_t *rx_dropped);

/* ── src/nvme.h ──────────────────────────────── */
/* 真实声明位于 src/nvme.h */
#include "nvme.h"

/* 见 src/nvme.h:34 */
int nvme_init(void);
/* 见 src/nvme.h:37 */
int nvme_ready(void);
/* 见 src/nvme.h:40 */
uint32_t nvme_sector_count(void);
/* 见 src/nvme.h:43 */
int nvme_read_sectors(uint32_t lba, uint8_t *buffer, uint32_t count);
/* 见 src/nvme.h:44 */
int nvme_write_sectors(uint32_t lba, const uint8_t *buffer, uint32_t count);

/* ── src/partition.h ──────────────────────────────── */
/* 真实声明位于 src/partition.h */
#include "partition.h"

/* 见 src/partition.h:59 */
int partition_parse(partition_read_fn rd, void *ctx, partition_entry_t *out, int max);
/* 见 src/partition.h:63 */
const char *partition_scheme_name(uint8_t scheme);
/* 见 src/partition.h:66 */
const char *partition_type_name(uint8_t type);

/* ── src/pci.h ──────────────────────────────── */
/* 真实声明位于 src/pci.h */
#include "pci.h"

/* 见 src/pci.h:55 */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
/* 见 src/pci.h:66 */
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
/* 见 src/pci.h:77 */
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
/* 见 src/pci.h:88 */
uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
/* 见 src/pci.h:102 */
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t *out);
/* 见 src/pci.h:115 */
void pci_enumerate_all(pci_enumerate_cb_t cb, void *ctx);
/* 见 src/pci.h:126 */
uint32_t pci_bar(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_index);
/* 见 src/pci.h:132 */
uint64_t pci_bar_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_index, int *is_io);
/* 见 src/pci.h:142 */
void pci_enable_bus_master_mmio(const pci_device_t *dev);

/* ── src/png.h ──────────────────────────────── */
/* 真实声明位于 src/png.h */
#include "png.h"

/* 见 src/png.h:37 */
int png_decode(const uint8_t *data, uint32_t size, uint8_t *out_rgb, uint32_t out_cap, int max_w, int max_h, int *out_w, int *out_h);

/* ── src/rtc.h ──────────────────────────────── */
/* 真实声明位于 src/rtc.h */
#include "rtc.h"

/* 见 src/rtc.h:36 */
void rtc_init(void);
/* 见 src/rtc.h:37 */
int rtc_read_time(rtc_time_t *time);
/* 见 src/rtc.h:38 */
uint64_t rtc_timestamp(void);
/* 见 src/rtc.h:39 */
void rtc_format_time(char *buf, size_t size);

/* ── src/rtc_tz.h ──────────────────────────────── */
/* 真实声明位于 src/rtc_tz.h */
#include "rtc_tz.h"

/* inline 定义 rtc_tz_outb  ── 见 src/rtc_tz.h:47 */
/* inline 定义 rtc_tz_inb  ── 见 src/rtc_tz.h:50 */
/* inline 定义 rtc_tz_cmos_read  ── 见 src/rtc_tz.h:55 */
/* inline 定义 rtc_tz_bcd_to_bin  ── 见 src/rtc_tz.h:59 */
/* inline 定义 rtc_tz_days_from_civil — * 与"自 1970-01-01 起的天数"之间的换算，公历下恒正确（含闰年）。  ── 见 src/rtc_tz.h:65 */
/* inline 定义 rtc_tz_civil_from_days  ── 见 src/rtc_tz.h:74 */
/* inline 定义 rtc_tz_cmos_epoch_now  ── 见 src/rtc_tz.h:94 */
/* inline 定义 rtc_tz_read_local  ── 见 src/rtc_tz.h:131 */

/* ── src/selftest.h ──────────────────────────────── */
/* 真实声明位于 src/selftest.h */
#include "selftest.h"

/* 见 src/selftest.h:18 */
int selftest_run(void);

/* ── src/shell/shell.h ──────────────────────────────── */
/* 真实声明位于 src/shell/shell.h */
#include "shell/shell.h"

/* 初始化 Shell 子系统  ── 见 src/shell/shell.h:97 */
void shell_init(void);
/* 注册一条命令  ── 见 src/shell/shell.h:100 */
void cmd_register(const command_t *cmd);
/* 批量注册命令（以 NULL 结尾的数组）  ── 见 src/shell/shell.h:103 */
void cmd_register_multiple(const command_t *cmds[]);
/* 执行单条命令  ── 见 src/shell/shell.h:106 */
void cmd_execute(const char *line);
/* 获取命令列表  ── 见 src/shell/shell.h:109 */
const command_t **cmd_get_list(void);
/* 获取命令数量  ── 见 src/shell/shell.h:112 */
uint32_t cmd_get_count(void);
/* 获取命令组名称  ── 见 src/shell/shell.h:115 */
const char *cmd_get_group_name(int group_id);
/* Shell 主循环（阻塞）  ── 见 src/shell/shell.h:118 */
void shell_run(void);
/* 输出 Shell 提示符  ── 见 src/shell/shell.h:121 */
void shell_print_prompt(void);
/* 返回 'g'（图形桌面）、't'（命令行）或 0（尚未设置过，即将出现启动选择提示）。  ── 见 src/shell/shell.h:127 */
int shell_get_startup_pref(void);
/* （比如当前文件系统后端是纯 ramfs，没有可持久化的磁盘）。  ── 见 src/shell/shell.h:130 */
int shell_set_startup_pref(int mode);
/* 简易外部命令注册（供应用程序使用）  ── 见 src/shell/shell.h:133 */
void cmd_register_external(const char *name, const char *desc, void (*handler)(int argc, char **argv));
/* 历史记录接口  ── 见 src/shell/shell.h:137 */
const char **cmd_get_history(void);
/* 见 src/shell/shell.h:138 */
int cmd_get_history_count(void);
/* 见 src/shell/shell.h:139 */
void cmd_clear_history(void);
/* 见 src/shell/shell.h:143 */
int kb_poll_key(void);
/* 当前 Alt（左/右）是否被按住  ── 见 src/shell/shell.h:144 */
int kb_alt_pressed(void);
/* 当前 Super/Win 键是否被按住  ── 见 src/shell/shell.h:145 */
int kb_super_pressed(void);
/* 当前 Ctrl 是否被按住  ── 见 src/shell/shell.h:146 */
int kb_ctrl_pressed(void);
/* NumLock 状态查询  ── 见 src/shell/shell.h:175 */
bool kb_is_numlock(void);

/* ── src/signal.h ──────────────────────────────── */
/* 真实声明位于 src/signal.h */
#include "signal.h"

/* 见 src/signal.h:116 */
int raise(int sig);
/* 见 src/signal.h:117 */
int kill(pid_t pid, int sig);

/* ── src/smp.h ──────────────────────────────── */
/* 真实声明位于 src/smp.h */
#include "smp.h"

/* 见 src/smp.h:126 */
void smp_init(void);
/* 见 src/smp.h:127 */
int smp_cpu_count(void);
/* 见 src/smp.h:128 */
void smp_send_ipi(uint32_t apic_id, uint32_t vector);
/* 见 src/smp.h:129 */
uint32_t smp_get_apic_id(void);
/* 见 src/smp.h:132 */
int smp_cpu_index(void);
/* 见 src/smp.h:134 */
void smp_lapic_eoi(void);
/* 见 src/smp.h:136 */
void smp_ap_timer_start(void);
/* 见 src/smp.h:138 */
void smp_ap_release(void);
/* 见 src/smp.h:140 */
void smp_panic_puts(const char *s);
/* 见 src/smp.h:141 */
void smp_panic_hex(uint64_t v);
/* 见 src/smp.h:143 */
void smp_sched_lock(void);
/* 见 src/smp.h:144 */
void smp_sched_unlock(void);
/* 见 src/smp.h:146 */
void spinlock_init(spinlock_t *lock);
/* 见 src/smp.h:147 */
void spinlock_acquire(spinlock_t *lock);
/* 见 src/smp.h:148 */
void spinlock_release(spinlock_t *lock);
/* 见 src/smp.h:150 */
per_cpu_t *smp_get_percpu(void);
/* 见 src/smp.h:151 */
per_cpu_t *smp_get_percpu_by_id(uint32_t apic_id);

/* ── src/stdlib.h ──────────────────────────────── */
/* 真实声明位于 src/stdlib.h */
#include "stdlib.h"

/* 见 src/stdlib.h:36 */
int atoi(const char *nptr);
/* 见 src/stdlib.h:37 */
long strtol(const char *nptr, char **endptr, int base);

/* ── src/string.h ──────────────────────────────── */
/* 真实声明位于 src/string.h */
#include "string.h"

/* 见 src/string.h:36 */
void *memset(void *s, int c, uint64_t n);
/* 见 src/string.h:37 */
void *memcpy(void *dest, const void *src, uint64_t n);

/* ── src/sys/dirent.h ──────────────────────────────── */
/* 真实声明位于 src/sys/dirent.h */
#include "sys/dirent.h"

/* 见 src/sys/dirent.h:49 */
int getdents(unsigned int fd, struct dirent *dirp, unsigned int count);

/* ── src/sys/mman.h ──────────────────────────────── */
/* 真实声明位于 src/sys/mman.h */
#include "sys/mman.h"

/* 见 src/sys/mman.h:57 */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
/* 见 src/sys/mman.h:58 */
int munmap(void *addr, size_t len);
/* 见 src/sys/mman.h:59 */
int mprotect(void *addr, size_t len, int prot);
/* 见 src/sys/mman.h:60 */
void *brk(void *addr);

/* ── src/sys/select.h ──────────────────────────────── */
/* 真实声明位于 src/sys/select.h */
#include "sys/select.h"

/* 见 src/sys/select.h:67 */
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
/* 见 src/sys/select.h:69 */
int poll(struct pollfd *fds, unsigned int nfds, int timeout);

/* ── src/sys/stat.h ──────────────────────────────── */
/* 真实声明位于 src/sys/stat.h */
#include "sys/stat.h"

/* 见 src/sys/stat.h:87 */
int fstat(int fd, struct stat *st);
/* 见 src/sys/stat.h:88 */
int stat(const char *path, struct stat *st);
/* 见 src/sys/stat.h:89 */
int lstat(const char *path, struct stat *st);

/* ── src/sys/types.h ──────────────────────────────── */
/* 真实声明位于 src/sys/types.h */
#include "sys/types.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/sys/types.h */

/* ── src/sys/wait.h ──────────────────────────────── */
/* 真实声明位于 src/sys/wait.h */
#include "sys/wait.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/sys/wait.h */

/* ── src/syscall.h ──────────────────────────────── */
/* 真实声明位于 src/syscall.h */
#include "syscall.h"

/* 见 src/syscall.h:286 */
uint64_t syscall_dispatch_frame(hbos_syscall_frame_t *frame);
/* 见 src/syscall.h:287 */
uint64_t linux_syscall_dispatch_frame(hbos_syscall_frame_t *frame);
/* 见 src/syscall.h:289 */
void linux_syscall_trace_dump(void);
/* 见 src/syscall.h:318 */
void linux_debug_set_flag(int which, int value);
/* *  pid 传 0 关闭。fork 出的子进程"跑几步就停住"且无异常可 dump 时用。  ── 见 src/syscall.h:322 */
void linux_debug_set_trace_pid(int pid);
/* *  这个是聚合的，直接用来出热点排行。默认关闭。  ── 见 src/syscall.h:327 */
void linux_syscall_prof_set(int on);
/* 见 src/syscall.h:328 */
int linux_syscall_prof_enabled(void);
/* 见 src/syscall.h:329 */
void linux_syscall_prof_reset(void);
/* 见 src/syscall.h:331 */
void linux_syscall_prof_dump(uint32_t top);
/* *  they are reported in siginfo.si_addr / ucontext REG_CR2 and REG_ERR.  ── 见 src/syscall.h:336 */
int linux_signal_prepare_exception(void *exception_frame, int signal_number, uint64_t cr2, uint64_t err_code);

/* ── src/tls.h ──────────────────────────────── */
/* 真实声明位于 src/tls.h */
#include "tls.h"

/* 见 src/tls.h:27 */
const char *tls_last_error(void);
/* 见 src/tls.h:28 */
int tls_https_get(const char *host, uint32_t ip, uint16_t port, const char *path, char *out, uint32_t out_cap, uint32_t *out_len);
/* * 宽松默认值；图片/CSS 等可降级子资源用较小值，避免坏 CDN 卡住整页。  ── 见 src/tls.h:32 */
int tls_https_get_with_idle_limit(const char *host, uint32_t ip, uint16_t port, const char *path, char *out, uint32_t out_cap, uint32_t *out_len, uint32_t idle_limit);

/* ── src/tools/cc.h ──────────────────────────────── */
/* 真实声明位于 src/tools/cc.h */
#include "tools/cc.h"

/* 见 src/tools/cc.h:41 */
void cc_set_gfx(const cc_gfx_t *hooks);
/* 见 src/tools/cc.h:43 */
int hbos_gcc_run_file(const char *path, int verbose);
/* 见 src/tools/cc.h:44 */
int hbos_gcc_run_file_capture(const char *path, char *out, uint32_t out_cap);
/* 见 src/tools/cc.h:45 */
const char *hbos_gcc_last_error(void);
/* 见 src/tools/cc.h:46 */
int hbos_gcc_last_error_line(void);
/* 见 src/tools/cc.h:47 */
int hbos_gcc_last_return(void);
/* 见 src/tools/cc.h:48 */
void tool_cc_init(void);

/* ── src/tools/tool.h ──────────────────────────────── */
/* 真实声明位于 src/tools/tool.h */
#include "tools/tool.h"

/* inline 定义 tool_outb — 端口 I/O（被 reboot/poweroff 等命令使用）  ── 见 src/tools/tool.h:42 */
/* inline 定义 tool_outw  ── 见 src/tools/tool.h:45 */
/* 各工具模块初始化  ── 见 src/tools/tool.h:50 */
void tool_help_init(void);
/* 见 src/tools/tool.h:51 */
void tool_system_init(void);
/* 见 src/tools/tool.h:52 */
void tool_debug_init(void);
/* 见 src/tools/tool.h:53 */
void tool_history_init(void);
/* 见 src/tools/tool.h:54 */
void tool_file_init(void);
/* 见 src/tools/tool.h:55 */
void tool_app_init(void);
/* 见 src/tools/tool.h:56 */
void tool_ata_init(void);
/* 见 src/tools/tool.h:57 */
void tool_disk_init(void);
/* 见 src/tools/tool.h:59 */
void tool_gui_init(void);
/* 见 src/tools/tool.h:61 */
void tool_net_init(void);
/* 见 src/tools/tool.h:63 */
void tool_cppe_init(void);
/* 见 src/tools/tool.h:64 */
void tool_audio_init(void);
/* inline 定义 tool_init_all — 聚合初始化 — 注册所有内建工具  ── 见 src/tools/tool.h:67 */

/* ── src/tty.h ──────────────────────────────── */
/* 真实声明位于 src/tty.h */
#include "tty.h"

/* 见 src/tty.h:239 */
void tty_init(void);
/* 见 src/tty.h:240 */
void tty_switch(int n);
/* 见 src/tty.h:241 */
int tty_current(void);
/* 见 src/tty.h:242 */
tty_t *tty_get(int n);
/* 见 src/tty.h:243 */
int tty_write(int n, const char *buf, uint32_t len);
/* 见 src/tty.h:244 */
int tty_read(int n, char *buf, uint32_t len);
/* 见 src/tty.h:245 */
void tty_input_char(char c);
/* 见 src/tty.h:246 */
int tty_output_ready(int n);
/* 见 src/tty.h:247 */
int tty_input_ready(int n);
/* 见 src/tty.h:248 */
int tty_register_vfs(int n, void *node);
/* fd0 读：键盘→行规程  ── 见 src/tty.h:249 */
int tty_console_read(char *buf, uint32_t len);
/* 设 tty0 前台进程组（0=无）  ── 见 src/tty.h:250 */
void tty_set_foreground(uint32_t pgid);
/* void   tty_set_foreground(uint32_t pgid);  设 tty0 前台进程组（0=无）  ── 见 src/tty.h:251 */
int tty_ioctl(int n, uint32_t cmd, void *arg);
/* /dev/tty、/dev/console 的 ioctl 操作表  ── 见 src/tty.h:254 */
const void *tty_console_ops(void);
/* const void *tty_console_ops(void);         /dev/tty、/dev/console 的 ioctl 操作表  ── 见 src/tty.h:255 */
int tty_node_is_tty(const void *node);
/* open("/dev/ptmx")：返回新 master vfs 节点  ── 见 src/tty.h:256 */
void *pty_open_master(void);
/* void  *pty_open_master(void);              open("/dev/ptmx")：返回新 master vfs 节点  ── 见 src/tty.h:257 */
int pty_master_index(const void *node);

/* ── src/types/stdbool.h ──────────────────────────────── */
/* 真实声明位于 src/types/stdbool.h */
#include "types/stdbool.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/types/stdbool.h */

/* ── src/types/stdint.h ──────────────────────────────── */
/* 真实声明位于 src/types/stdint.h */
#include "types/stdint.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/types/stdint.h */

/* ── src/ui/theme.h ──────────────────────────────── */
/* 真实声明位于 src/ui/theme.h */
#include "ui/theme.h"

/* 见 src/ui/theme.h:106 */
const gui_theme_t *ui_theme(void);
/* 见 src/ui/theme.h:112 */
int ui_theme_init(void);
/* 见 src/ui/theme.h:119 */
int ui_theme_set(const char *name);
/* 见 src/ui/theme.h:122 */
int ui_theme_set_index(int index);
/* 见 src/ui/theme.h:125 */
int ui_theme_current_index(void);
/* 见 src/ui/theme.h:131 */
int ui_theme_reload(void);
/* 见 src/ui/theme.h:139 */
int ui_theme_scan(char names[][UI_THEME_NAME_MAX], int max);
/* 见 src/ui/theme.h:142 */
int ui_theme_count(void);
/* 见 src/ui/theme.h:145 */
const gui_theme_t *ui_theme_builtin_dark(void);
/* 见 src/ui/theme.h:146 */
const gui_theme_t *ui_theme_builtin_light(void);
/* 见 src/ui/theme.h:160 */
int ui_theme_parse_json(const char *json, uint32_t len, const gui_theme_t *base, gui_theme_t *out);
/* 见 src/ui/theme.h:176 */
int ui_theme_apply_json(const void *doc, const gui_theme_t *base, gui_theme_t *out);
/* 见 src/ui/theme.h:183 */
int ui_theme_save_choice(const char *name);
/* 见 src/ui/theme.h:186 */
uint32_t ui_blend(uint32_t dst, uint32_t src);
/* 见 src/ui/theme.h:189 */
uint32_t ui_shade(uint32_t color, int amount);

/* ── src/ui/ui_app.h ──────────────────────────────── */
/* 真实声明位于 src/ui/ui_app.h */
#include "ui/ui_app.h"

/* 见 src/ui/ui_app.h:83 */
void *ui_alloc(size_t size);
/* 见 src/ui/ui_app.h:84 */
void ui_free(void *p);
/* 见 src/ui/ui_app.h:292 */
void ui_wm_init(ui_wm_t *wm, int desk_w, int desk_h);
/* 见 src/ui/ui_app.h:299 */
int ui_wm_open(ui_wm_t *wm, const char *title, int w, int h);
/* 见 src/ui/ui_app.h:302 */
void ui_wm_close(ui_wm_t *wm, int idx);
/* 见 src/ui/ui_app.h:305 */
void ui_wm_focus(ui_wm_t *wm, int idx);
/* 见 src/ui/ui_app.h:308 */
void ui_wm_focus_next(ui_wm_t *wm, int dir);
/* 见 src/ui/ui_app.h:311 */
void ui_wm_raise(ui_wm_t *wm, int idx);
/* 见 src/ui/ui_app.h:313 */
void ui_wm_move(ui_wm_t *wm, int idx, int x, int y);
/* 见 src/ui/ui_app.h:314 */
void ui_wm_resize(ui_wm_t *wm, int idx, int w, int h);
/* 见 src/ui/ui_app.h:315 */
void ui_wm_minimize(ui_wm_t *wm, int idx);
/* 见 src/ui/ui_app.h:316 */
void ui_wm_maximize(ui_wm_t *wm, int idx);
/* 见 src/ui/ui_app.h:317 */
void ui_wm_restore(ui_wm_t *wm, int idx);
/* 见 src/ui/ui_app.h:318 */
void ui_wm_toggle_maximize(ui_wm_t *wm, int idx);
/* 见 src/ui/ui_app.h:321 */
ui_window_t *ui_wm_get(ui_wm_t *wm, int idx);
/* 见 src/ui/ui_app.h:322 */
ui_window_t *ui_wm_active(ui_wm_t *wm);
/* 见 src/ui/ui_app.h:325 */
ui_rect_t ui_wm_content_rect(const ui_wm_t *wm, int idx);
/* 见 src/ui/ui_app.h:328 */
bool ui_wm_tick(ui_wm_t *wm, int ms);
/* 见 src/ui/ui_app.h:333 */
int ui_wm_hit_titlebar(const ui_wm_t *wm, int mx, int my);
/* 见 src/ui/ui_app.h:335 */
int ui_wm_hit_window(const ui_wm_t *wm, int mx, int my);
/* 见 src/ui/ui_app.h:337 */
int ui_wm_hit_border(const ui_wm_t *wm, int mx, int my, ui_edge_t *edge);
/* 见 src/ui/ui_app.h:347 */
int ui_app_register(const ui_app_vt_t *vt);
/* 见 src/ui/ui_app.h:350 */
const ui_app_vt_t *ui_app_vt_by_mode(int mode);
/* 见 src/ui/ui_app.h:353 */
int ui_app_vt_count(void);
/* 见 src/ui/ui_app.h:354 */
const ui_app_vt_t *ui_app_vt_at(int index);
/* 见 src/ui/ui_app.h:363 */
ui_app_t *ui_app_launch(ui_wm_t *wm, int mode, bool unique);
/* 见 src/ui/ui_app.h:366 */
void ui_app_close(ui_wm_t *wm, ui_app_t *app);
/* 见 src/ui/ui_app.h:369 */
ui_app_t *ui_app_find(ui_wm_t *wm, int mode);
/* 见 src/ui/ui_app.h:372 */
ui_app_t *ui_app_active(ui_wm_t *wm);

/* ── src/ui/ui_controls.h ──────────────────────────────── */
/* 真实声明位于 src/ui/ui_controls.h */
#include "ui/ui_controls.h"

/* 见 src/ui/ui_controls.h:87 */
void ui_button_init(ui_button_t *b, int x, int y, int w, int h, ui_btn_kind_t kind, const char *label);
/* 见 src/ui/ui_controls.h:90 */
bool ui_button_handle(ui_button_t *b, const ui_event_t *ev);
/* 见 src/ui/ui_controls.h:91 */
void ui_button_draw(const ui_button_t *b);
/* 见 src/ui/ui_controls.h:107 */
void ui_input_init(ui_input_t *in, int x, int y, int w, int h, const char *initial, const char *placeholder);
/* 见 src/ui/ui_controls.h:110 */
bool ui_input_handle(ui_input_t *in, const ui_event_t *ev);
/* 见 src/ui/ui_controls.h:111 */
void ui_input_draw(const ui_input_t *in);
/* 见 src/ui/ui_controls.h:113 */
void ui_input_set(ui_input_t *in, const char *text);
/* 见 src/ui/ui_controls.h:123 */
void ui_checkbox_init(ui_checkbox_t *c, int x, int y, int w, int h, const char *label, bool checked);
/* 见 src/ui/ui_controls.h:126 */
bool ui_checkbox_handle(ui_checkbox_t *c, const ui_event_t *ev);
/* 见 src/ui/ui_controls.h:127 */
void ui_checkbox_draw(const ui_checkbox_t *c);
/* 见 src/ui/ui_controls.h:136 */
void ui_switch_init(ui_switch_t *s, int x, int y, bool on);
/* 见 src/ui/ui_controls.h:138 */
bool ui_switch_handle(ui_switch_t *s, const ui_event_t *ev);
/* 见 src/ui/ui_controls.h:139 */
void ui_switch_draw(const ui_switch_t *s);
/* 见 src/ui/ui_controls.h:152 */
void ui_segment_init(ui_segment_t *s, int x, int y, int w, int h, const char **labels, int count, int active);
/* 见 src/ui/ui_controls.h:155 */
bool ui_segment_handle(ui_segment_t *s, const ui_event_t *ev, int *out_index);
/* 见 src/ui/ui_controls.h:156 */
void ui_segment_draw(const ui_segment_t *s);
/* 见 src/ui/ui_controls.h:170 */
void ui_tabs_init(ui_tabs_t *t, int x, int y, int h, const char **labels, int count, int active);
/* 见 src/ui/ui_controls.h:173 */
bool ui_tabs_handle(ui_tabs_t *t, const ui_event_t *ev, int *out_index);
/* 见 src/ui/ui_controls.h:174 */
void ui_tabs_draw(const ui_tabs_t *t);
/* 见 src/ui/ui_controls.h:187 */
void ui_scrollbar_init(ui_scrollbar_t *sb, int x, int y, int w, int h, int view_h, int content_h);
/* 见 src/ui/ui_controls.h:190 */
bool ui_scrollbar_handle(ui_scrollbar_t *sb, const ui_event_t *ev);
/* 见 src/ui/ui_controls.h:191 */
void ui_scrollbar_draw(const ui_scrollbar_t *sb);
/* 见 src/ui/ui_controls.h:193 */
void ui_scrollbar_clamp(ui_scrollbar_t *sb);
/* 见 src/ui/ui_controls.h:208 */
void ui_list_init(ui_list_t *l, int x, int y, int w, int h, int row_h, int count);
/* 见 src/ui/ui_controls.h:214 */
bool ui_list_handle(ui_list_t *l, const ui_event_t *ev, int *out_row);
/* 见 src/ui/ui_controls.h:216 */
void ui_list_draw(const ui_list_t *l);
/* 见 src/ui/ui_controls.h:218 */
ui_rect_t ui_list_row_rect(const ui_list_t *l, int index);
/* 见 src/ui/ui_controls.h:220 */
void ui_list_ensure_visible(ui_list_t *l, int index);
/* 见 src/ui/ui_controls.h:244 */
void ui_dialog_init(ui_dialog_t *d, int cx, int cy, int w, int h, ui_dlg_kind_t kind, const char *title, const char *ok_label, const char *cancel_label);
/* 见 src/ui/ui_controls.h:248 */
void ui_dialog_draw(const ui_dialog_t *d);
/* 见 src/ui/ui_controls.h:250 */
int ui_dialog_handle(ui_dialog_t *d, const ui_event_t *ev);

/* ── src/ui/ui_tokens.h ──────────────────────────────── */
/* 真实声明位于 src/ui/ui_tokens.h */
#include "ui/ui_tokens.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/ui/ui_tokens.h */

/* ── src/ui/ui_widget.h ──────────────────────────────── */
/* 真实声明位于 src/ui/ui_widget.h */
#include "ui/ui_widget.h"

/* inline 定义 ui_hit  ── 见 src/ui/ui_widget.h:72 */
/* inline 定义 ui_hit_xy  ── 见 src/ui/ui_widget.h:79 */
/* inline 定义 ui_rect_overlap  ── 见 src/ui/ui_widget.h:85 */
/* inline 定义 ui_rect_clip  ── 见 src/ui/ui_widget.h:91 */
/* 见 src/ui/ui_widget.h:152 */
void ui_widget_init(ui_widget_t *w, int x, int y, int w_, int h_, int id);
/* 见 src/ui/ui_widget.h:163 */
bool ui_widget_handle_mouse(ui_widget_t *w, const ui_event_t *ev);
/* 见 src/ui/ui_widget.h:166 */
ui_state_t ui_widget_state(const ui_widget_t *w);
/* 见 src/ui/ui_widget.h:169 */
void ui_widget_tick(ui_widget_t *w, int ms);
/* 见 src/ui/ui_widget.h:179 */
void ui_fill_round(const ui_rect_t *r, int radius, uint32_t color);
/* 见 src/ui/ui_widget.h:182 */
void ui_stroke_round(const ui_rect_t *r, int radius, int width, uint32_t color);
/* 见 src/ui/ui_widget.h:185 */
void ui_draw_shadow(const ui_rect_t *r, int radius, int level);
/* 见 src/ui/ui_widget.h:188 */
uint32_t ui_sel_over(uint32_t base);

/* ── src/unistd.h ──────────────────────────────── */
/* 真实声明位于 src/unistd.h */
#include "unistd.h"

/* ---- 文件 I/O ----  ── 见 src/unistd.h:159 */
ssize_t read(int fd, void *buf, size_t count);
/* 见 src/unistd.h:160 */
ssize_t write(int fd, const void *buf, size_t count);
/* 见 src/unistd.h:161 */
int close(int fd);
/* 见 src/unistd.h:162 */
int unlink(const char *path);
/* 见 src/unistd.h:163 */
off_t lseek(int fd, off_t offset, int whence);
/* 见 src/unistd.h:164 */
int isatty(int fd);
/* ---- 文件系统扩展 ----  ── 见 src/unistd.h:167 */
int access(const char *path, int mode);
/* 见 src/unistd.h:168 */
int ftruncate(int fd, off_t length);
/* 见 src/unistd.h:169 */
char *getcwd(char *buf, size_t size);
/* 见 src/unistd.h:170 */
int chdir(const char *path);
/* 见 src/unistd.h:171 */
int mkdir(const char *path, mode_t mode);
/* 见 src/unistd.h:172 */
int rmdir(const char *path);
/* 见 src/unistd.h:173 */
int rename(const char *old_path, const char *new_path);
/* 见 src/unistd.h:174 */
int posix_rename(const char *old_path, const char *new_path, unsigned int flags);
/* 见 src/unistd.h:176 */
int symlink(const char *target, const char *linkpath);
/* 见 src/unistd.h:177 */
int chmod(const char *path, mode_t mode);
/* 见 src/unistd.h:178 */
int chown(const char *path, uid_t uid, gid_t gid);
/* 见 src/unistd.h:179 */
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
/* ---- 文件描述符操作 ----  ── 见 src/unistd.h:182 */
int dup(int oldfd);
/* 见 src/unistd.h:183 */
int dup2(int oldfd, int newfd);
/* 见 src/unistd.h:184 */
int pipe(int pipefd[2]);
/* 见 src/unistd.h:185 */
int fcntl(int fd, int cmd, long arg);
/* 见 src/unistd.h:186 */
int ioctl(int fd, unsigned long cmd, unsigned long arg);
/* ---- 进程控制 ----  ── 见 src/unistd.h:189 */
pid_t getpid(void);
/* 见 src/unistd.h:190 */
pid_t getppid(void);
/* 见 src/unistd.h:191 */
pid_t fork(void);
/* 见 src/unistd.h:192 */
int execve(const char *path, char *const argv[], char *const envp[]);
/* 见 src/unistd.h:193 */
pid_t waitpid(pid_t pid, int *status, int options);
/* 见 src/unistd.h:195 */
unsigned int sleep(unsigned int seconds);
/* 见 src/unistd.h:196 */
int usleep(useconds_t usec);
/* 见 src/unistd.h:197 */
void _exit(int status);
/* ---- 内存管理 ----  ── 见 src/unistd.h:200 */
void *_sbrk(intptr_t increment);
/* 见 src/unistd.h:201 */
void *sbrk(intptr_t increment);
/* ---- 用户/组 ID ----  ── 见 src/unistd.h:205 */
uid_t geteuid(void);
/* 见 src/unistd.h:206 */
gid_t getegid(void);
/* 见 src/unistd.h:207 */
uid_t getuid(void);
/* 见 src/unistd.h:208 */
gid_t getgid(void);
/* 见 src/unistd.h:209 */
int setuid(uid_t uid);
/* 见 src/unistd.h:210 */
int setgid(gid_t gid);
/* 见 src/unistd.h:211 */
int getgroups(int size, gid_t list[]);
/* 见 src/unistd.h:212 */
int setgroups(int size, const gid_t list[]);
/* 见 src/unistd.h:213 */
pid_t getpgid(pid_t pid);
/* 见 src/unistd.h:217 */
int sigaction(int sig, const void *act, void *oact);
/* 见 src/unistd.h:218 */
int sigprocmask(int how, const void *set, void *oset);
/* 见 src/unistd.h:219 */
int pause(void);
/* ---- 时间 ----  ── 见 src/unistd.h:222 */
int nanosleep(const void *req, void *rem);
/* 见 src/unistd.h:223 */
int clock_gettime(int clockid, void *tp);
/* 见 src/unistd.h:224 */
clock_t times(void *buf);
/* ---- 网络套接字 ----  ── 见 src/unistd.h:227 */
int socket(int domain, int type, int protocol);
/* 见 src/unistd.h:228 */
int bind(int sockfd, const void *addr, size_t addrlen);
/* 见 src/unistd.h:229 */
int listen(int sockfd, int backlog);
/* 见 src/unistd.h:230 */
int accept(int sockfd, void *addr, size_t *addrlen);
/* 见 src/unistd.h:231 */
int connect(int sockfd, const void *addr, size_t addrlen);
/* 见 src/unistd.h:232 */
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
/* 见 src/unistd.h:233 */
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
/* ---- 系统管理 ----  ── 见 src/unistd.h:236 */
int reboot(int cmd);
/* 见 src/unistd.h:237 */
void sync(void);
/* 见 src/unistd.h:238 */
int mount(const char *src, const char *tgt, const char *fstype, unsigned long flags, const void *data);
/* 见 src/unistd.h:239 */
int umount(const char *tgt, unsigned long flags);

/* ── src/usb_ehci.h ──────────────────────────────── */
/* 真实声明位于 src/usb_ehci.h */
#include "usb_ehci.h"

/* 见 src/usb_ehci.h:42 */
int usb_ehci_register(void);
/* 见 src/usb_ehci.h:54 */
int usb_ehci_init(void);
/* 见 src/usb_ehci.h:57 */
int usb_ehci_device_count(void);
/* 见 src/usb_ehci.h:63 */
int usb_ehci_get_device_desc(int dev_idx, usb_device_desc_t *desc);
/* 见 src/usb_ehci.h:74 */
int usb_ehci_control_transfer(int dev_idx, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, void *data, uint16_t wLength);
/* 见 src/usb_ehci.h:86 */
int usb_ehci_interrupt_transfer(int dev_idx, int ep_addr, void *data, uint32_t len, uint32_t interval);
/* 见 src/usb_ehci.h:90 */
void usb_ehci_poll(void);

/* ── src/usb_hid.h ──────────────────────────────── */
/* 真实声明位于 src/usb_hid.h */
#include "usb_hid.h"

/* 见 src/usb_hid.h:82 */
int hid_init(void);
/* 见 src/usb_hid.h:83 */
int hid_device_count(void);
/* 见 src/usb_hid.h:84 */
int hid_keyboard_count(void);
/* 见 src/usb_hid.h:85 */
int hid_mouse_count(void);
/* 见 src/usb_hid.h:86 */
int hid_get_keyboard_report(int idx, hid_kbd_report_t *report);
/* 见 src/usb_hid.h:87 */
int hid_get_mouse_report(int idx, hid_mouse_report_t *report);
/* 见 src/usb_hid.h:88 */
void hid_poll(void);
/* 见 src/usb_hid.h:89 */
int usb_hid_init(void);
/* 见 src/usb_hid.h:90 */
int usb_kbd_init(void);
/* 见 src/usb_hid.h:91 */
int usb_kbd_getc(void);
/* 见 src/usb_hid.h:92 */
int usb_kbd_ready(void);
/* 见 src/usb_hid.h:93 */
int usb_mouse_init(void);
/* 见 src/usb_hid.h:94 */
int usb_mouse_get_report(hid_mouse_report_t *report);
/* 见 src/usb_hid.h:95 */
int usb_mouse_ready(void);

/* ── src/usb_hid_parser.h ──────────────────────────────── */
/* 真实声明位于 src/usb_hid_parser.h */
#include "usb_hid_parser.h"

/* 见 src/usb_hid_parser.h:60 */
int hid_parse_mouse_layouts(const uint8_t *desc, uint32_t desc_len, hid_mouse_layout_t *out, int max_layouts);
/* 见 src/usb_hid_parser.h:81 */
int hid_mouse_decode(const hid_mouse_layout_t *layout, const uint8_t *report, uint32_t report_len, int *dx, int *dy, int *wheel, uint32_t *buttons);

/* ── src/usb_host.h ──────────────────────────────── */
/* 真实声明位于 src/usb_host.h */
#include "usb_host.h"

/* 见 src/usb_host.h:74 */
int usb_host_register(const usb_host_ops_t *ops);
/* 见 src/usb_host.h:84 */
int usb_host_init_all(void);
/* 见 src/usb_host.h:87 */
int usb_host_count(void);
/* 见 src/usb_host.h:93 */
const usb_host_ops_t *usb_host_get(int host_idx);
/* 见 src/usb_host.h:96 */
void usb_host_poll_all(void);

/* ── src/usb_msc.h ──────────────────────────────── */
/* 真实声明位于 src/usb_msc.h */
#include "usb_msc.h"

/* 见 src/usb_msc.h:105 */
int msc_init(void);
/* 见 src/usb_msc.h:106 */
int msc_device_count(void);
/* 见 src/usb_msc.h:107 */
int msc_read_sector(int dev_idx, uint32_t lba, uint8_t *buf, uint32_t count);
/* 见 src/usb_msc.h:108 */
int msc_write_sector(int dev_idx, uint32_t lba, const uint8_t *buf, uint32_t count);
/* 见 src/usb_msc.h:109 */
uint32_t msc_get_block_count(int dev_idx);
/* 见 src/usb_msc.h:110 */
uint32_t msc_get_block_size(int dev_idx);

/* ── src/user/app.h ──────────────────────────────── */
/* 真实声明位于 src/user/app.h */
#include "user/app.h"

/* 见 src/user/app.h:57 */
uint32_t hbos_app_count(void);
/* 见 src/user/app.h:60 */
const hbos_app_t *hbos_app_get(uint32_t index);
/* 见 src/user/app.h:63 */
const hbos_app_t *hbos_app_find(const char *name);
/* 见 src/user/app.h:66 */
int hbos_app_run(const char *name, int argc, char **argv);
/* 见 src/user/app.h:69 */
int hbos_app_spawn(const char *name, int argc, char **argv);

/* ── src/user/hax_app.h ──────────────────────────────── */
/* 真实声明位于 src/user/hax_app.h */
#include "user/hax_app.h"

/* 见 src/user/hax_app.h:62 */
uint32_t hax_app_count(void);
/* 见 src/user/hax_app.h:65 */
const hax_app_entry_t *hax_app_at(uint32_t index);
/* 见 src/user/hax_app.h:68 */
const hax_app_entry_t *hax_app_find(const char *name);
/* 见 src/user/hax_app.h:79 */
const uint32_t *hax_app_icon_at(uint32_t index, int *tile);
/* 见 src/user/hax_app.h:85 */
int hax_app_run(const char *name, int argc, char **argv);
/* 见 src/user/hax_app.h:93 */
int hax_app_spawn(const char *name, int argc, char **argv);
/* 见 src/user/hax_app.h:101 */
int hax_seed_embedded_file(const char *name, const char *path);

/* ── src/user/ldso.h ──────────────────────────────── */
/* 真实声明位于 src/user/ldso.h */
#include "user/ldso.h"

/* 见 src/user/ldso.h:261 */
void *ldso_load(const uint8_t *data, size_t size);
/* 见 src/user/ldso.h:264 */
void *ldso_load_vfs(struct vfs_node *node);
/* * per-address-space object de-duplication.  ── 见 src/user/ldso.h:268 */
void *ldso_load_vfs_path(struct vfs_node *node, const char *path);
/* * orders them; user libc invokes the returned address in ring3.  ── 见 src/user/ldso.h:272 */
uintptr_t ldso_init_next(void *handle, uint64_t ordinal);
/* 见 src/user/ldso.h:273 */
uintptr_t ldso_fini_next(void *handle, uint64_t ordinal);
/* 见 src/user/ldso.h:276 */
uintptr_t ldso_ifunc_next(void *handle, uint64_t ordinal);
/* 见 src/user/ldso.h:277 */
int ldso_ifunc_apply(void *handle, uint64_t ordinal, uintptr_t result);
/* 见 src/user/ldso.h:280 */
uintptr_t ldso_tls_get_addr(uint32_t module_id, uint64_t offset);
/* 见 src/user/ldso.h:283 */
void ldso_release_thread_tls(struct task *task);
/* 见 src/user/ldso.h:290 */
void *ldso_resolve(const char *name);
/* 见 src/user/ldso.h:299 */
void *ldso_dlsym(void *handle, const char *name);
/* 见 src/user/ldso.h:302 */
void *ldso_dlsym_version(void *handle, const char *name, const char *version);
/* 见 src/user/ldso.h:312 */
int ldso_close(void *handle);
/* 见 src/user/ldso.h:315 */
void ldso_release_address_space(const void *owner_mm);

/* ── src/user/syscall.h ──────────────────────────────── */
/* 真实声明位于 src/user/syscall.h */
#include "user/syscall.h"

/* 见 src/user/syscall.h:120 */
long hbos_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);
/* 见 src/user/syscall.h:125 */
long hbos_syscall3(long nr, long a0, long a1, long a2);
/* 见 src/user/syscall.h:132 */
ssize_t hbos_read(int fd, void *buf, size_t count);
/* 见 src/user/syscall.h:134 */
ssize_t hbos_write(int fd, const void *buf, size_t count);
/* 见 src/user/syscall.h:136 */
int hbos_open(const char *path, int flags, int mode);
/* 见 src/user/syscall.h:138 */
int hbos_close(int fd);
/* 见 src/user/syscall.h:140 */
off_t hbos_lseek(int fd, off_t offset, int whence);
/* 见 src/user/syscall.h:142 */
int hbos_fstat(int fd, struct stat *st);
/* 见 src/user/syscall.h:144 */
int hbos_stat(const char *path, struct stat *st);
/* 见 src/user/syscall.h:146 */
int hbos_unlink(const char *path);
/* 见 src/user/syscall.h:148 */
int hbos_isatty(int fd);
/* 见 src/user/syscall.h:155 */
pid_t hbos_getpid(void);
/* 见 src/user/syscall.h:157 */
pid_t hbos_getppid(void);
/* 见 src/user/syscall.h:159 */
void hbos_exit(int status);
/* 见 src/user/syscall.h:161 */
unsigned int hbos_sleep(unsigned int seconds);
/* 见 src/user/syscall.h:163 */
int hbos_usleep(useconds_t usec);
/* 见 src/user/syscall.h:165 */
pid_t hbos_fork(void);
/* 见 src/user/syscall.h:167 */
int hbos_execve(const char *path, char *const argv[], char *const envp[]);
/* 见 src/user/syscall.h:169 */
pid_t hbos_waitpid(pid_t pid, int *status, int options);
/* 见 src/user/syscall.h:171 */
int hbos_kill(pid_t pid, int sig);
/* 见 src/user/syscall.h:187 */
int hbos_uname(struct hbos_utsname *buf);
/* 见 src/user/syscall.h:196 */
int hbos_gettimeofday(struct hbos_timeval *tv, void *tz);
/* 见 src/user/syscall.h:209 */
int hbos_access(const char *path, int mode);
/* 见 src/user/syscall.h:211 */
int hbos_ftruncate(int fd, off_t length);
/* 见 src/user/syscall.h:213 */
int hbos_mkdir(const char *path, mode_t mode);
/* 见 src/user/syscall.h:215 */
int hbos_rmdir(const char *path);
/* 见 src/user/syscall.h:217 */
int hbos_getcwd(char *buf, size_t size);
/* 见 src/user/syscall.h:219 */
int hbos_chdir(const char *path);
/* 见 src/user/syscall.h:221 */
int hbos_symlink(const char *target, const char *linkpath);
/* 见 src/user/syscall.h:223 */
int hbos_chmod(const char *path, mode_t mode);
/* 见 src/user/syscall.h:225 */
int hbos_chown(const char *path, uid_t uid, gid_t gid);
/* 见 src/user/syscall.h:227 */
ssize_t hbos_readlink(const char *path, char *buf, size_t bufsiz);
/* 见 src/user/syscall.h:234 */
int hbos_dup(int oldfd);
/* 见 src/user/syscall.h:236 */
int hbos_dup2(int oldfd, int newfd);
/* 见 src/user/syscall.h:238 */
int hbos_pipe(int pipefd[2]);
/* 见 src/user/syscall.h:240 */
int hbos_fcntl(int fd, int cmd, long arg);
/* 见 src/user/syscall.h:242 */
int hbos_ioctl(int fd, unsigned long cmd, unsigned long arg);
/* 见 src/user/syscall.h:249 */
uid_t hbos_geteuid(void);
/* 见 src/user/syscall.h:251 */
gid_t hbos_getegid(void);
/* 见 src/user/syscall.h:253 */
uid_t hbos_getuid(void);
/* 见 src/user/syscall.h:255 */
gid_t hbos_getgid(void);
/* 见 src/user/syscall.h:257 */
int hbos_setuid(uid_t uid);
/* 见 src/user/syscall.h:259 */
int hbos_setgid(gid_t gid);
/* 见 src/user/syscall.h:261 */
int hbos_getgroups(int size, gid_t list[]);
/* 见 src/user/syscall.h:263 */
int hbos_setgroups(int size, const gid_t list[]);
/* 见 src/user/syscall.h:265 */
pid_t hbos_getpgid(pid_t pid);
/* 见 src/user/syscall.h:274 */
int hbos_sigaction(int sig, const void *act, void *oact);
/* 见 src/user/syscall.h:276 */
int hbos_sigprocmask(int how, const void *set, void *oset);
/* 见 src/user/syscall.h:278 */
int hbos_pause(void);
/* 见 src/user/syscall.h:285 */
void *hbos_sbrk(intptr_t increment);
/* 见 src/user/syscall.h:287 */
void *hbos_brk(void *addr);
/* 见 src/user/syscall.h:289 */
void *hbos_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
/* 见 src/user/syscall.h:291 */
int hbos_munmap(void *addr, size_t len);
/* 见 src/user/syscall.h:293 */
int hbos_mprotect(void *addr, size_t len, int prot);
/* 见 src/user/syscall.h:300 */
int hbos_nanosleep(const struct hbos_timespec *req, struct hbos_timespec *rem);
/* 见 src/user/syscall.h:302 */
int hbos_clock_gettime(int clockid, struct hbos_timespec *tp);
/* 见 src/user/syscall.h:304 */
clock_t hbos_times(void *buf);
/* 见 src/user/syscall.h:311 */
int hbos_socket(int domain, int type, int protocol);
/* 见 src/user/syscall.h:313 */
int hbos_bind(int sockfd, const void *addr, size_t addrlen);
/* 见 src/user/syscall.h:315 */
int hbos_listen(int sockfd, int backlog);
/* 见 src/user/syscall.h:317 */
int hbos_accept(int sockfd, void *addr, size_t *addrlen);
/* 见 src/user/syscall.h:319 */
int hbos_connect(int sockfd, const void *addr, size_t addrlen);
/* 见 src/user/syscall.h:321 */
ssize_t hbos_send(int sockfd, const void *buf, size_t len, int flags);
/* 见 src/user/syscall.h:323 */
ssize_t hbos_recv(int sockfd, void *buf, size_t len, int flags);
/* 见 src/user/syscall.h:330 */
int hbos_reboot(int cmd);
/* 见 src/user/syscall.h:332 */
void hbos_sync(void);
/* 见 src/user/syscall.h:334 */
int hbos_mount(const char *src, const char *tgt, const char *fstype, unsigned long flags, const void *data);
/* 见 src/user/syscall.h:336 */
int hbos_umount(const char *tgt, unsigned long flags);
/* 见 src/user/syscall.h:343 */
void hbos_puts(const char *s);

/* ── src/version.h ──────────────────────────────── */
/* 真实声明位于 src/version.h */
#include "version.h"

/* 该头只有类型/宏，无函数声明  ── 见 src/version.h */

/* ── src/vfs.h ──────────────────────────────── */
/* 真实声明位于 src/vfs.h */
#include "vfs.h"

/* 初始化 VFS 层  ── 见 src/vfs.h:81 */
int vfs_init(void);
/* 见 src/vfs.h:82 */
int vfs_resolve_path(const char *cwd, const char *path, char *out, uint32_t cap);
/* 查找节点并跟随符号链接  ── 见 src/vfs.h:83 */
vfs_node_t *vfs_lookup(const char *path);
/* 不跟随最终符号链接  ── 见 src/vfs.h:84 */
vfs_node_t *vfs_lookup_nofollow(const char *path);
/* 按路径创建 VFS 节点  ── 见 src/vfs.h:85 */
vfs_node_t *vfs_create(const char *path);
/* 创建符号链接  ── 见 src/vfs.h:86 */
vfs_node_t *vfs_symlink(const char *path, const char *target);
/* 按路径删除 VFS 节点  ── 见 src/vfs.h:87 */
int vfs_unlink(const char *path);
/* 见 src/vfs.h:88 */
int vfs_rename(const char *old_path, const char *new_path, int replace);
/* 从 VFS 节点读取数据  ── 见 src/vfs.h:89 */
int vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count);
/* 向 VFS 节点写入数据  ── 见 src/vfs.h:90 */
int vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count);
/* 把 VFS 节点截断/扩展到 size 字节  ── 见 src/vfs.h:91 */
int vfs_truncate(vfs_node_t *node, uint32_t size);
/* 获取 VFS 节点数量  ── 见 src/vfs.h:92 */
uint32_t vfs_count(void);
/* 按索引获取 VFS 节点  ── 见 src/vfs.h:93 */
vfs_node_t *vfs_get(uint32_t index);
/* 见 src/vfs.h:100 */
vfs_node_t *vfs_register_static_file(const char *path, const void *data, uint32_t size);
/* 见 src/vfs.h:103 */
int vfs_mkdir(const char *path);
/* 见 src/vfs.h:104 */
int vfs_rmdir(const char *path);
/* 见 src/vfs.h:105 */
int vfs_opendir(const char *path);
/* 见 src/vfs.h:106 */
int vfs_readdir(const char *path, char *name, uint32_t *type);
/* 见 src/vfs.h:107 */
int vfs_readdir_at(const char *path, uint32_t index, char *name, uint32_t *type);
/* 见 src/vfs.h:108 */
int vfs_closedir(const char *path);

/* ── src/wav.h ──────────────────────────────── */
/* 真实声明位于 src/wav.h */
#include "wav.h"

/* 见 src/wav.h:35 */
int wav_parse(const uint8_t *data, uint32_t size, wav_info_t *out);

/* ── src/xhci.h ──────────────────────────────── */
/* 真实声明位于 src/xhci.h */
#include "xhci.h"

/* 见 src/xhci.h:181 */
int xhci_init(void);
/* 见 src/xhci.h:182 */
void xhci_poll(void);
/* 见 src/xhci.h:183 */
int xhci_device_count(void);
/* 见 src/xhci.h:184 */
int xhci_device_slot(int idx);
/* 见 src/xhci.h:185 */
int xhci_get_device_desc(int idx, usb_device_desc_t *desc);
/* 见 src/xhci.h:186 */
int xhci_control_transfer(int slot_id, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, void *data, uint16_t wLength);
/* 见 src/xhci.h:189 */
int xhci_bulk_transfer(int slot_id, int ep_addr, void *data, uint32_t len);
/* 见 src/xhci.h:190 */
int xhci_bulk_transfer_packet(int slot_id, int ep_addr, void *data, uint32_t len, uint32_t max_packet);
/* 见 src/xhci.h:192 */
int xhci_interrupt_transfer(int slot_id, int ep_addr, void *data, uint32_t len, uint32_t interval);
/* 见 src/xhci.h:198 */
int xhci_dev_control_transfer(int dev_idx, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, void *data, uint16_t wLength);
/* 见 src/xhci.h:201 */
int xhci_dev_interrupt_transfer(int dev_idx, int ep_addr, void *data, uint32_t len, uint32_t interval);
/* 见 src/xhci.h:203 */
int xhci_dev_bulk_transfer(int dev_idx, int ep_addr, void *data, uint32_t len);
/* 见 src/xhci.h:204 */
int xhci_dev_bulk_transfer_packet(int dev_idx, int ep_addr, void *data, uint32_t len, uint32_t max_packet);

#endif

#endif /* HBOS_ALL_H */
