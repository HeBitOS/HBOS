#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "syscall.h"

static FILE _stdin  = { .fd = 0, .buf_mode = 1, .buf_size = BUFSIZ };
static FILE _stdout = { .fd = 1, .buf_mode = 1, .buf_size = BUFSIZ };
static FILE _stderr = { .fd = 2, .buf_mode = 0, .buf_size = 0 };
static FILE _file_pool[FOPEN_MAX];
static int  _file_cnt;

FILE *stdin  = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

static int _alloc_fd(void) {
    for (int i = 0; i < FOPEN_MAX; i++)
        if (!_file_pool[i].buf) { _file_cnt++; return i; }
    return -1;
}

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) return 0;
    int idx = _alloc_fd();
    if (idx < 0) return 0;
    int omode = 0;
    for (const char *p = mode; *p; p++) {
        if (*p == 'r') omode |= 0;
        else if (*p == 'w') omode |= 1;
        else if (*p == 'a') omode |= 2;
    }
    int fd = -1;
    if (omode == 0) fd = (int)__syscall3(HBOS_SYS_OPEN, (long)path, O_RDONLY, 0);
    else if (omode == 1) {
        fd = (int)__syscall3(HBOS_SYS_OPEN, (long)path, O_WRONLY | O_CREAT, 0);
        if (fd >= 0) __syscall3(HBOS_SYS_FTRUNCATE, fd, 0, 0);
    } else {
        fd = (int)__syscall3(HBOS_SYS_OPEN, (long)path, O_WRONLY | O_CREAT, 0);
        if (fd >= 0) __syscall3(HBOS_SYS_LSEEK, fd, 0, SEEK_END);
    }
    if (fd < 0) { _file_cnt--; return 0; }
    FILE *fp = &_file_pool[idx];
    memset(fp, 0, sizeof(*fp));
    fp->fd = fd; fp->buf_mode = 2; fp->buf_size = BUFSIZ;
    fp->buf = (uint8_t *)malloc(BUFSIZ);
    return fp;
}

int fclose(FILE *fp) {
    if (!fp || !fp->buf) return EOF;
    fflush(fp);
    __syscall1(HBOS_SYS_CLOSE, fp->fd);
    free(fp->buf);
    memset(fp, 0, sizeof(*fp));
    _file_cnt--;
    return 0;
}

int fflush(FILE *fp) {
    if (!fp) {
        fflush(stdout); fflush(stderr);
        for (int i = 0; i < FOPEN_MAX; i++)
            if (_file_pool[i].buf) fflush(&_file_pool[i]);
        return 0;
    }
    if (!fp->buf || fp->buf_len <= 0) return 0;
    long n = __syscall3(HBOS_SYS_WRITE, fp->fd, (long)fp->buf, fp->buf_len);
    fp->buf_len = 0;
    return n < 0 ? EOF : 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!ptr || !fp || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    size_t done = 0;
    uint8_t *dst = (uint8_t *)ptr;

    if (fp->buf && fp->buf_len > fp->buf_pos) {
        size_t avail = (size_t)(fp->buf_len - fp->buf_pos);
        if (avail > total) avail = total;
        memcpy(dst, fp->buf + fp->buf_pos, avail);
        fp->buf_pos += (long)avail; dst += avail; done += avail; total -= avail;
    }

    if (total > BUFSIZ) {
        long n = __syscall3(HBOS_SYS_READ, fp->fd, (long)dst, (long)total);
        if (n > 0) done += (size_t)n;
        fp->pos += (long)done;
        return done / size;
    }

    if (total > 0 && fp->buf) {
        long n = __syscall3(HBOS_SYS_READ, fp->fd, (long)fp->buf, fp->buf_size);
        if (n <= 0) { fp->eof = (n == 0); return done / size; }
        fp->buf_len = n; fp->buf_pos = 0;
        size_t avail = (size_t)n;
        if (avail > total) avail = total;
        memcpy(dst, fp->buf, avail);
        fp->buf_pos += (long)avail; done += avail;
    }

    fp->pos += (long)done;
    return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!ptr || !fp || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    const uint8_t *src = (const uint8_t *)ptr;

    if (!fp->buf) {
        long n = __syscall3(HBOS_SYS_WRITE, fp->fd, (long)src, (long)total);
        fp->pos += n > 0 ? n : 0;
        return n > 0 ? (size_t)n / size : 0;
    }

    if (total + (size_t)fp->buf_len >= (size_t)fp->buf_size) {
        if (fflush(fp) < 0) return 0;
    }

    if (total >= (size_t)fp->buf_size) {
        long n = __syscall3(HBOS_SYS_WRITE, fp->fd, (long)src, (long)total);
        fp->pos += n > 0 ? n : 0;
        return n > 0 ? (size_t)n / size : 0;
    }

    memcpy(fp->buf + fp->buf_len, src, total);
    fp->buf_len += (long)total;
    fp->pos += (long)total;

    if (fp->buf_mode == 1 || fp->buf_len >= fp->buf_size)
        fflush(fp);

    return nmemb;
}

int fseek(FILE *fp, long offset, int whence) {
    if (!fp) return -1;
    fflush(fp);
    long ret = __syscall3(HBOS_SYS_LSEEK, fp->fd, offset, whence);
    if (ret >= 0) { fp->pos = ret; fp->buf_pos = 0; fp->buf_len = 0; }
    return ret < 0 ? -1 : 0;
}

long ftell(FILE *fp) {
    if (!fp) return -1;
    return fp->pos;
}

int fgetc(FILE *fp) {
    uint8_t c;
    if (fread(&c, 1, 1, fp) != 1) return EOF;
    return c;
}

int fputc(int c, FILE *fp) {
    uint8_t uc = (uint8_t)c;
    if (fwrite(&uc, 1, 1, fp) != 1) return EOF;
    return uc;
}

char *fgets(char *s, int size, FILE *fp) {
    if (!s || size < 2) return 0;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(fp);
        if (c == EOF) { if (i == 0) return 0; break; }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

int fputs(const char *s, FILE *fp) {
    size_t len = strlen(s);
    return fwrite(s, 1, len, fp) == len ? 0 : EOF;
}

int feof(FILE *fp)  { return fp ? fp->eof : 0; }
int ferror(FILE *fp) { return fp ? fp->error : 0; }

int getchar(void) { return fgetc(stdin); }
int putchar(int c) { return fputc(c, stdout); }
int puts(const char *s) { return fputs(s, stdout) < 0 ? EOF : fputc('\n', stdout); }

static int _print_num(char *buf, size_t n, unsigned long long val,
                      int base, int sign, int width, int pad, size_t *pos) {
    char tmp[32]; int i = 0; int neg = 0;
    if (sign && (long long)val < 0) { neg = 1; val = (unsigned long long)(-(long long)val); }
    if (val == 0) tmp[i++] = '0';
    while (val > 0) { tmp[i++] = "0123456789abcdef"[val % (unsigned)base]; val /= (unsigned)base; }
    if (neg) tmp[i++] = '-';
    while (i < width) tmp[i++] = (char)pad;
    while (i > 0 && *pos < n - 1) buf[(*pos)++] = tmp[--i];
    return 0;
}

static int _vsnprintf_core(char *buf, size_t n, const char *fmt, va_list ap) {
    size_t pos = 0;
    while (*fmt && pos < n - 1) {
        if (*fmt != '%') { buf[pos++] = *fmt++; continue; }
        fmt++;
        int width = 0, pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == 'l') fmt++;
        if (*fmt == 'l') fmt++;

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && pos < n - 1) buf[pos++] = *s++;
            break;
        }
        case 'c': buf[pos++] = (char)va_arg(ap, int); break;
        case 'd': case 'i':
            _print_num(buf, n, (unsigned long long)va_arg(ap, long long), 10, 1, width, pad, &pos); break;
        case 'u':
            _print_num(buf, n, va_arg(ap, unsigned long long), 10, 0, width, pad, &pos); break;
        case 'x':
            _print_num(buf, n, va_arg(ap, unsigned long long), 16, 0, width, pad, &pos); break;
        case 'p':
            buf[pos++] = '0'; buf[pos++] = 'x';
            _print_num(buf, n, (uintptr_t)va_arg(ap, void *), 16, 0, 0, '0', &pos); break;
        case '%': buf[pos++] = '%'; break;
        default: buf[pos++] = '%'; buf[pos++] = *fmt; break;
        }
        if (*fmt) fmt++;
    }
    buf[pos] = '\0';
    return (int)pos;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    return _vsnprintf_core(buf, n, fmt, ap);
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = _vsnprintf_core(buf, n, fmt, ap);
    va_end(ap); return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return _vsnprintf_core(buf, (size_t)0x7FFFFFFF, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = _vsnprintf_core(buf, (size_t)0x7FFFFFFF, fmt, ap);
    va_end(ap); return r;
}

int vfprintf(FILE *fp, const char *fmt, va_list ap) {
    char buf[4096];
    int r = _vsnprintf_core(buf, sizeof(buf), fmt, ap);
    if (r > 0) fwrite(buf, 1, (size_t)r, fp);
    return r;
}

int fprintf(FILE *fp, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(fp, fmt, ap);
    va_end(ap); return r;
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap); return r;
}