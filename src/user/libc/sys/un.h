/**
 * @file    un.h
 * @brief   导出 1 个接口（1 类型 / 0 宏 / 0 函数）
 *
 * @layer   user
 * @owner   src/user/libc/sys/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  sockaddr_un  UNIX 域套接字地址
 */
#ifndef HBOS_USER_LIBC_SYS_UN_H
#define HBOS_USER_LIBC_SYS_UN_H

#include <stdint.h>

struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[108];
};

#endif
