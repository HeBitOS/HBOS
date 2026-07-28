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
#include "png.h"

/* 2x2 真彩 PNG（像素 红/绿/蓝/黄），用于自检 inflate+png 解码链路。 */
static const unsigned char SELFTEST_PNG[78] = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,2,8,2,0,0,0,
    253,212,154,115,0,0,0,21,73,68,65,84,120,156,5,193,1,1,0,0,0,128,16,255,
    79,23,66,80,25,30,239,4,252,164,1,116,243,0,0,0,0,73,69,78,68,174,66,96,130};

static int selftest_fail(const char *name) {
    console_puts("[SELFTEST] POSIX/ramfs: FAIL ");
    console_puts(name);
    console_putchar('\n');
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

    char cwd[64];
    CHECK("getcwd root", getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/") == 0);
    (void)rmdir("/tmp/selftest-dir");
    CHECK("mkdir tmp", mkdir("/tmp/selftest-dir", 0755) == 0);
    CHECK("chdir tmp", chdir("/tmp/selftest-dir") == 0);
    CHECK("getcwd tmp", getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/tmp/selftest-dir") == 0);
    fd = open("rel.txt", O_CREAT | O_RDWR | O_TRUNC);
    CHECK("open relative", fd >= 3);
    CHECK("write relative", write(fd, "cwd", 3) == 3);
    CHECK("close relative", close(fd) == 0);
    CHECK("stat relative", stat("rel.txt", &st) == 0 && st.st_size == 3);
    CHECK("stat parent", stat("../selftest-dir/rel.txt", &st) == 0 && st.st_size == 3);
    CHECK("unlink relative", unlink("rel.txt") == 0);
    CHECK("chdir root", chdir("/") == 0);
    CHECK("rmdir tmp", rmdir("/tmp/selftest-dir") == 0);

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
#if HBOS_BUNDLE_APPS
    CHECK("app registry", hbos_app_find("uwc") != NULL);
#else
    CHECK("empty app registry", hbos_app_count() == 0);
#endif

    /* PNG 解码链路（inflate + png）：2x2 红/绿/蓝/黄，验证维度和四个像素。 */
    static unsigned char png_rgb[2 * 2 * 3];
    int pw = 0, ph = 0;
    CHECK("png decode",
          png_decode(SELFTEST_PNG, sizeof(SELFTEST_PNG), png_rgb, sizeof(png_rgb),
                     64, 64, &pw, &ph) == 0);
    CHECK("png dims", pw == 2 && ph == 2);
    CHECK("png px0 red",   png_rgb[0] == 255 && png_rgb[1] == 0   && png_rgb[2] == 0);
    CHECK("png px1 green", png_rgb[3] == 0   && png_rgb[4] == 255 && png_rgb[5] == 0);
    CHECK("png px2 blue",  png_rgb[6] == 0   && png_rgb[7] == 0   && png_rgb[8] == 255);
    CHECK("png px3 yellow",png_rgb[9] == 255 && png_rgb[10] == 255 && png_rgb[11] == 0);

    console_puts("[SELFTEST] PNG decode: PASS\n");
    console_puts("[SELFTEST] POSIX/ramfs: PASS\n");
    return 0;
}
