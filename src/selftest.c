#include <stddef.h>

#include "errno.h"
#include "fcntl.h"
#include "selftest.h"
#include "string.h"
#include "sys/stat.h"
#include "unistd.h"
#include "graphics/graphics.h"
#include "core/heap.h"
#include "core/task.h"
#include "user/app.h"
#include "user/syscall.h"
#include "png.h"
#include "linux_compat.h"

/* 2x2 真彩 PNG（像素 红/绿/蓝/黄），用于自检 inflate+png 解码链路。 */
static const unsigned char SELFTEST_PNG[78] = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,2,8,2,0,0,0,
    253,212,154,115,0,0,0,21,73,68,65,84,120,156,5,193,1,1,0,0,0,128,16,255,
    79,23,66,80,25,30,239,4,252,164,1,116,243,0,0,0,0,73,69,78,68,174,66,96,130};

static int selftest_fail(const char *name) {
    console_puts("[SELFTEST] FAIL ");
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

    uint8_t *heap_a = (uint8_t *)kmalloc(64);
    uint8_t *heap_b = (uint8_t *)kmalloc(128);
    void *heap_aligned = kmalloc_aligned(257, 256);
    CHECK("heap allocate", heap_a && heap_b && heap_aligned);
    CHECK("heap alignment", ((uintptr_t)heap_aligned & 255U) == 0);
    for (uint32_t i = 0; i < 64; i++) heap_a[i] = (uint8_t)i;
    kfree(heap_b);
    CHECK("heap reuse", kmalloc(96) == heap_b);
    heap_a = (uint8_t *)krealloc(heap_a, 256);
    CHECK("heap realloc", heap_a != NULL);
    for (uint32_t i = 0; i < 64; i++)
        CHECK("heap preserve", heap_a[i] == (uint8_t)i);
    kfree(heap_a);
    kfree(heap_b);
    kfree(heap_aligned);

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

    /* Linux event compatibility: eventfd readiness feeds poll and epoll
     * without a helper process or a second descriptor namespace. */
    int event_fd = linux_compat_eventfd2(0, LINUX_EFD_NONBLOCK);
    CHECK("eventfd create", event_fd >= 3);
    linux_pollfd_t poll_fd = {event_fd, LINUX_POLLIN, 0};
    CHECK("poll empty eventfd",
          linux_compat_poll(&poll_fd, 1, 0) == 0 &&
          poll_fd.revents == 0);
    uint64_t event_value = 7;
    CHECK("eventfd write",
          linux_compat_write(event_fd, &event_value,
                             sizeof(event_value)) == (long)sizeof(event_value));
    CHECK("poll ready eventfd",
          linux_compat_poll(&poll_fd, 1, 0) == 1 &&
          (poll_fd.revents & LINUX_POLLIN));

    int epoll_fd = linux_compat_epoll_create1(0);
    CHECK("epoll create", epoll_fd >= 3);
    linux_epoll_event_t watch;
    memset(&watch, 0, sizeof(watch));
    watch.events = LINUX_POLLIN;
    watch.data.u64 = 0x48424f53ULL;
    CHECK("epoll add",
          linux_compat_epoll_ctl(epoll_fd, LINUX_EPOLL_CTL_ADD,
                                 event_fd, &watch) == 0);
    linux_epoll_event_t ready_event;
    memset(&ready_event, 0, sizeof(ready_event));
    CHECK("epoll ready",
          linux_compat_epoll_wait(epoll_fd, &ready_event, 1, 0) == 1 &&
          ready_event.data.u64 == watch.data.u64 &&
          (ready_event.events & LINUX_POLLIN));
    event_value = 0;
    CHECK("eventfd read",
          linux_compat_read(event_fd, &event_value,
                            sizeof(event_value)) == (long)sizeof(event_value) &&
          event_value == 7);
    (void)linux_compat_close(epoll_fd);
    CHECK("close epoll", close(epoll_fd) == 0);
    (void)linux_compat_close(event_fd);
    CHECK("close eventfd", close(event_fd) == 0);

    int nonblock_pipe[2];
    CHECK("pipe2 nonblock",
          linux_compat_pipe2(nonblock_pipe, O_NONBLOCK) == 0);
    CHECK("pipe2 empty EAGAIN",
          read(nonblock_pipe[0], b, 1) < 0 && errno == EAGAIN);
    CHECK("close pipe2 read", close(nonblock_pipe[0]) == 0);
    CHECK("close pipe2 write", close(nonblock_pipe[1]) == 0);

    int unix_pair[2];
    CHECK("unix socketpair",
          linux_compat_unix_socketpair(1, 0, unix_pair) == 0);
    CHECK("unix socket send",
          linux_compat_unix_send(unix_pair[0], "dbus", 4, 0) == 4);
    poll_fd.fd = unix_pair[1];
    poll_fd.events = LINUX_POLLIN;
    poll_fd.revents = 0;
    CHECK("unix socket poll",
          linux_compat_poll(&poll_fd, 1, 0) == 1 &&
          (poll_fd.revents & LINUX_POLLIN));
    memset(b, 0, sizeof(b));
    CHECK("unix socket recv",
          linux_compat_unix_recv(unix_pair[1], b, sizeof(b), 0) == 4 &&
          memcmp(b, "dbus", 4) == 0);
    (void)linux_compat_close(unix_pair[0]);
    CHECK("close unix pair 0", close(unix_pair[0]) == 0);
    CHECK("unix peer EOF",
          linux_compat_unix_recv(unix_pair[1], b, sizeof(b), 0) == 0);
    (void)linux_compat_close(unix_pair[1]);
    CHECK("close unix pair 1", close(unix_pair[1]) == 0);

    struct {
        uint16_t family;
        char path[16];
    } unix_address = {1, "hbos-selftest"};
    int unix_listener = linux_compat_unix_socket(1, 0);
    int unix_client = linux_compat_unix_socket(1, 0);
    CHECK("unix stream sockets", unix_listener >= 3 && unix_client >= 3);
    CHECK("unix bind",
          linux_compat_unix_bind(unix_listener, &unix_address,
                                 sizeof(unix_address)) == 0);
    CHECK("unix listen",
          linux_compat_unix_listen(unix_listener, 2) == 0);
    CHECK("unix connect",
          linux_compat_unix_connect(unix_client, &unix_address,
                                    sizeof(unix_address)) == 0);
    int unix_server =
        linux_compat_unix_accept(unix_listener, NULL, NULL);
    CHECK("unix accept", unix_server >= 3);
    CHECK("unix accepted send",
          linux_compat_unix_send(unix_client, "ipc", 3, 0) == 3);
    memset(b, 0, sizeof(b));
    CHECK("unix accepted recv",
          linux_compat_unix_recv(unix_server, b, sizeof(b), 0) == 3 &&
          memcmp(b, "ipc", 3) == 0);
    (void)linux_compat_close(unix_server);
    CHECK("close unix server", close(unix_server) == 0);
    (void)linux_compat_close(unix_client);
    CHECK("close unix client", close(unix_client) == 0);
    (void)linux_compat_close(unix_listener);
    CHECK("close unix listener", close(unix_listener) == 0);

    uint32_t futex_word = 1;
    CHECK("futex compare EAGAIN",
          linux_compat_futex(&futex_word, 0, 0, NULL) < 0 &&
          errno == EAGAIN);
    CHECK("futex bitset rejects empty mask",
          linux_compat_futex6(&futex_word, 10, 1, NULL, NULL, 0) < 0 &&
          errno == EINVAL);
    CHECK("futex bitset wake empty",
          linux_compat_futex6(&futex_word, 10, 1, NULL, NULL,
                              0x40000000U) == 0);

    uint64_t saved_fs_base = task_get_fs_base();
    CHECK("set FS TLS base", task_set_fs_base(0x12345000ULL) == 0);
    CHECK("get FS TLS base", task_get_fs_base() == 0x12345000ULL);
    CHECK("restore FS TLS base", task_set_fs_base(saved_fs_base) == 0);
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
    console_puts("[SELFTEST] Linux event/AF_UNIX: PASS\n");
    console_puts("[SELFTEST] POSIX/ramfs: PASS\n");
    return 0;
}
