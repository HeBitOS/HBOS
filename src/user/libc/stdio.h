#ifndef HBOS_USER_LIBC_STDIO_H
#define HBOS_USER_LIBC_STDIO_H

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
char *fgets(char *s, int size, FILE *fp);
int   fputs(const char *s, FILE *fp);
int   feof(FILE *fp);
int   ferror(FILE *fp);

int printf(const char *fmt, ...);
int fprintf(FILE *fp, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *fp, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

int getchar(void);
int putchar(int c);
int puts(const char *s);

#endif