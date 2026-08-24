#include <stdint.h>
#include <string.h>
#include <poll.h>
#include <signalfd.h>
#include "syscall.h"
#include <unistd.h>

/* G1 signalfd 验证（Chromium 信号处理依赖）。阻塞 SIGUSR1 后注册
 * signalfd 关注它，kill 自身，poll POLLIN 就绪、read 拿到 siginfo。 */

static void say(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    while (n) {
        long w = write(1, s, n);
        if (w <= 0) break;
        s += w; n -= (size_t)w;
    }
}

#define SIGUSR1 10
#define SIGTERM 15

int main(void) {
    /* 1. 阻塞 SIGUSR1（signalfd 接收被阻塞信号） */
    uint64_t block = 1ULL << (SIGUSR1 - 1);
    long r = __syscall3(HBOS_SYS_SIGPROCMASK, 0 /* SIG_BLOCK */,
                        (long)&block, 0);
    if (r < 0) { say("LINUX_SIGNALFD: FAIL (sigprocmask)\n"); return 1; }

    /* 2. 创建 signalfd 关注 SIGUSR1+SIGTERM */
    uint64_t sm = (1ULL << (SIGUSR1 - 1)) | (1ULL << (SIGTERM - 1));
    int fd = (int)__syscall6(HBOS_SYS_SIGNALFD, -1, (long)&sm, 8,
                             SFD_NONBLOCK, 0, 0);
    if (fd < 0) { say("LINUX_SIGNALFD: FAIL (signalfd)\n"); return 1; }

    /* 3. kill 自身 SIGUSR1 */
    long pid = __syscall1(HBOS_SYS_GETPID, 0);
    r = __syscall3(HBOS_SYS_KILL, pid, SIGUSR1, 0);
    if (r < 0) { say("LINUX_SIGNALFD: FAIL (kill)\n"); return 1; }

    /* 4. poll POLLIN */
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, 2000) != 1 || !(p.revents & POLLIN)) {
        say("LINUX_SIGNALFD: FAIL (no POLLIN)\n"); return 1;
    }

    /* 5. read 128 字节 siginfo */
    struct signalfd_siginfo info;
    memset(&info, 0, sizeof(info));
    long n = read(fd, &info, sizeof(info));
    if (n != 128 || info.ssi_signo != SIGUSR1) {
        say("LINUX_SIGNALFD: FAIL (siginfo)\n"); return 1;
    }
    close(fd);
    say("LINUX_SIGNALFD: PASS\n");
    return 0;
}
