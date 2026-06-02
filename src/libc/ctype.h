/**
 * @file    ctype.h
 * @brief   HBOS 标准库 — 对标 ANSI C <ctype.h>
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