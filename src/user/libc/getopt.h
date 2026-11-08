/**
 * @file    getopt.h
 * @brief   导出 1 个接口（0 类型 / 0 宏 / 1 函数）
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   函数  getopt      （待补）
 */
#ifndef HBOS_USER_LIBC_GETOPT_H
#define HBOS_USER_LIBC_GETOPT_H

/* Short-option getopt only (no getopt_long) — enough for the BusyBox
 * applets targeted so far; add getopt_long() if/when one needs it. */
extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt(int argc, char *const argv[], const char *optstring);

#endif
