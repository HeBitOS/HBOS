/**
 * @file    netdb.h
 * @brief   导出 19 个接口（2 类型 / 14 宏 / 3 函数）
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  hostent     主机表项
 *         addrinfo    地址解析结果
 *   宏    EAI_BADFLAGS    常量 / 特性宏
 *         EAI_NONAME      常量 / 特性宏
 *         EAI_AGAIN       常量 / 特性宏
 *         EAI_FAIL        常量 / 特性宏
 *         EAI_FAMILY      常量 / 特性宏
 *         EAI_SOCKTYPE    常量 / 特性宏
 *         EAI_SERVICE     常量 / 特性宏
 *         EAI_MEMORY      EAI内存
 *         EAI_SYSTEM      常量 / 特性宏
 *         h_addr          常量 / 特性宏
 *         AI_PASSIVE      常量 / 特性宏
 *         AI_CANONNAME    常量 / 特性宏
 *         AI_NUMERICHOST  常量 / 特性宏
 *         AI_ADDRCONFIG   常量 / 特性宏
 *   函数  freeaddrinfo   （待补）
 *         gethostbyname  （待补）
 */
#ifndef HBOS_USER_LIBC_NETDB_H
#define HBOS_USER_LIBC_NETDB_H

#include <stddef.h>
#include <stdint.h>
#include "socket.h"

/* EAI 错误码（Linux 语义，负值） */
#define EAI_BADFLAGS    -1
#define EAI_NONAME      -2
#define EAI_AGAIN       -3
#define EAI_FAIL        -4
#define EAI_FAMILY      -6
#define EAI_SOCKTYPE    -7
#define EAI_SERVICE     -8
#define EAI_MEMORY      -10
#define EAI_SYSTEM      -11

struct hostent {
    char  *h_name;
    char **h_aliases;
    int    h_addrtype;
    int    h_length;
    char **h_addr_list;
};
#define h_addr h_addr_list[0]

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE     0x0001
#define AI_CANONNAME   0x0002
#define AI_NUMERICHOST 0x0004
#define AI_ADDRCONFIG  0x0020

int    getaddrinfo(const char *node, const char *service,
                   const struct addrinfo *hints, struct addrinfo **res);
void   freeaddrinfo(struct addrinfo *res);
struct hostent *gethostbyname(const char *name);

#endif
