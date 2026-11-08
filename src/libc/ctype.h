/**
 * @file    ctype.h
 * @brief   HBOS 标准库 — 对标 ANSI C <ctype.h>
 *
 * @layer   user
 * @owner   src/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   函数  isalnum     （待补）
 *         isalpha     是否字母
 *         isblank     （待补）
 *         iscntrl     （待补）
 *         isdigit     是否数字
 *         isgraph     （待补）
 *         islower     （待补）
 *         isprint     （待补）
 *         ispunct     （待补）
 *         isspace     是否空白
 *         isupper     （待补）
 *         isxdigit    （待补）
 *         toupper     转大写
 *         tolower     转小写
 */
#ifndef HBOS_LIBC_CTYPE_H
#define HBOS_LIBC_CTYPE_H

int isalnum(int c);
int isalpha(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);

int toupper(int c);
int tolower(int c);

#endif