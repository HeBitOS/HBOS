/**
 * @file    stdio.h
 * @brief   HBOS 标准 I/O 库 — 对标 ANSI C <stdio.h>
 *
 * 提供 FILE* 缓冲 I/O、printf/scanf 系列、文件流操作。
 *
 * @layer   user
 * @owner   src/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  FILE        HBOS 标准 I/O 库 — 对标 AN…
 *   宏    EOF           文件结束标志（−1）
 *         BUFSIZ        标准缓冲大小
 *         FILENAME_MAX  路径名最大长度
 *         FOPEN_MAX     同时打开文件数上限
 *   函数  fopen       按模式打开流
 *         fclose      关闭流并刷新
 *         fflush      刷新流缓冲
 *         fread       从流读一块
 *         fwrite      向流写一块
 *         fseek       设置流位置
 *         ftell       取流当前位置
 *         fgetc       读一个字符
 *         fputc       写一个字符
 *         ungetc      （待补）
 *         fgets       读一行到缓冲
 *         fputs       写字符串到流
 *         feof        是否已到流末尾
 *         ferror      流是否出错
 *         clearerr    （待补）
 *         fileno      （待补）
 *         printf      格式化输出到 stdout
 *         fprintf     格式化输出到流
 *         sprintf     格式化到缓冲
 *         snprintf    格式化到缓冲，返回应写长度
 *         vprintf     va_list 版 printf
 *         vfprintf    va_list 版 fprintf
 *         vsprintf    va_list 版 sprintf
 *         vsnprintf   va_list 版 snprintf
 *         scanf       HBOS 标准 I/O 库 — 对标 AN…
 *         fscanf      （待补）
 *         sscanf      （待补）
 *         getchar     从 stdin 读一字符
 *         putchar     输出一个字符
 *         puts        输出字符串 + 换行
 *         getc        从流读一字符
 *         putc        向流写一字符
 *         perror      （待补）
 *         remove      删除文件或空目录
 *         rename      重命名路径
 */
#ifndef HBOS_LIBC_STDIO_H
#define HBOS_LIBC_STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define EOF (-1)
#define BUFSIZ 4096
#define FILENAME_MAX 256
#define FOPEN_MAX 32

typedef struct {
    int   fd;
    int   flags;
    int   buf_mode;
    int   error;
    int   eof;
    long  pos;
    long  buf_pos;
    long  buf_len;
    long  buf_size;
    uint8_t *buf;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *fp);
int   fflush(FILE *fp);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int   fseek(FILE *fp, long offset, int whence);
long  ftell(FILE *fp);
int   fgetc(FILE *fp);
int   fputc(int c, FILE *fp);
int   ungetc(int c, FILE *fp);
char *fgets(char *s, int size, FILE *fp);
int   fputs(const char *s, FILE *fp);
int   feof(FILE *fp);
int   ferror(FILE *fp);
void  clearerr(FILE *fp);
int   fileno(FILE *fp);

int printf(const char *fmt, ...);
int fprintf(FILE *fp, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *fp, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

int scanf(const char *fmt, ...);
int fscanf(FILE *fp, const char *fmt, ...);
int sscanf(const char *s, const char *fmt, ...);

int getchar(void);
int putchar(int c);
int puts(const char *s);
int getc(FILE *fp);
int putc(int c, FILE *fp);

void perror(const char *s);
int remove(const char *path);
int rename(const char *old, const char *new);

#endif