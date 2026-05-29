#include <stddef.h>

#include "errno.h"
#include "fcntl.h"
#include "selftest.h"
#include "string.h"
#include "sys/stat.h"
#include "unistd.h"
#include "graphics/graphics.h"
#include "user/app.h"
#include "user/syscall.h"

static inline void serial_outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t serial_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void selftest_serial_putc(char c) {
    while (!(serial_inb(0x3F8 + 5) & 0x20));
    serial_outb(0x3F8, (uint8_t)c);
}

static void selftest_serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') selftest_serial_putc('\r');
        selftest_serial_putc(*s++);
    }
}

static int selftest_fail(const char *name) {
    console_puts("[SELFTEST] POSIX/ramfs: FAIL ");
    console_puts(name);
    console_putchar('\n');
    selftest_serial_puts("[SELFTEST] POSIX/ramfs: FAIL ");
    selftest_serial_puts(name);
    selftest_serial_puts("\n");
    return -1;
}

#define CHECK(name, expr) do { if (!(expr)) return selftest_fail(name); } while (0)

int selftest_run(void) {
    char a[16];
    char b[16];
    memset(a, 0, sizeof(a));
    memcpy(a, "abcd", 5);
    CHECK("memcpy/strcmp", strcmp(a, "abcd") == 0);
    memmove(a + 1, a, 4);
    CHECK("memmove", memcmp(a, "aabcd", 5) == 0);
    CHECK("strlen", strlen("hello") == 5);

    (void)unlink("__selftest");

    int fd = open("__selftest", O_RDONLY);
    CHECK("open missing", fd < 0 && errno == ENOENT);

    fd = open("__selftest", O_CREAT | O_RDWR | O_TRUNC);
    CHECK("open create", fd >= 3);
    int fd2 = open("/__selftest", O_CREAT | O_EXCL | O_RDWR);
    CHECK("open excl existing", fd2 < 0 && errno == EEXIST);
    CHECK("write hello", write(fd, "hello", 5) == 5);
    CHECK("seek start", lseek(fd, 0, SEEK_SET) == 0);
    memset(b, 0, sizeof(b));
    CHECK("read hello", read(fd, b, 5) == 5);
    CHECK("compare hello", memcmp(b, "hello", 5) == 0);

    struct stat st;
    CHECK("fstat", fstat(fd, &st) == 0 && st.st_size == 5);
    CHECK("close", close(fd) == 0);

    fd = open("__selftest", O_WRONLY | O_APPEND);
    CHECK("open append", fd >= 3);
    CHECK("append", write(fd, " world", 6) == 6);
    CHECK("close append", close(fd) == 0);
    CHECK("stat appended", stat("__selftest", &st) == 0 && st.st_size == 11);

    fd = open("__selftest", O_RDONLY);
    CHECK("open readback", fd >= 3);
    memset(b, 0, sizeof(b));
    CHECK("readback", read(fd, b, 11) == 11);
    CHECK("compare readback", memcmp(b, "hello world", 11) == 0);
    CHECK("bad fd", read(99, b, 1) < 0 && errno == EBADF);
    CHECK("close readback", close(fd) == 0);

    fd = open("__selftest", O_WRONLY | O_TRUNC);
    CHECK("open truncate", fd >= 3);
    CHECK("write truncated", write(fd, "x", 1) == 1);
    CHECK("close truncate", close(fd) == 0);
    CHECK("stat truncated", stat("__selftest", &st) == 0 && st.st_size == 1);

    CHECK("unlink", unlink("__selftest") == 0);
    CHECK("stat unlinked", stat("__selftest", &st) < 0 && errno == ENOENT);
    CHECK("unlink missing", unlink("__selftest") < 0 && errno == ENOENT);

    (void)hbos_unlink("__syscall");
    fd = hbos_open("__syscall", O_CREAT | O_RDWR | O_TRUNC, 0);
    CHECK("syscall open", fd >= 3);
    CHECK("syscall write", hbos_write(fd, "abi", 3) == 3);
    CHECK("syscall seek", hbos_lseek(fd, 0, SEEK_SET) == 0);
    memset(b, 0, sizeof(b));
    CHECK("syscall read", hbos_read(fd, b, 3) == 3);
    CHECK("syscall compare", memcmp(b, "abi", 3) == 0);
    CHECK("syscall close", hbos_close(fd) == 0);
    CHECK("syscall unlink", hbos_unlink("__syscall") == 0);
    CHECK("app registry", hbos_app_find("hello") != NULL);

    console_puts("[SELFTEST] POSIX/ramfs: PASS\n");
    selftest_serial_puts("[SELFTEST] POSIX/ramfs: PASS\n");
    return 0;
}
