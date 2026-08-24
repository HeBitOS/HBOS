#include <stdint.h>
#include <string.h>
#include <poll.h>
#include <timerfd.h>
#include <unistd.h>

/* G1 timerfd 验证：Chromium MessagePump 定时器依赖。一次性 + 周期定时、
 * poll POLLIN 就绪、read 到期计数、gettime 剩余时间。 */

static void say(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    while (n) {
        long w = write(1, s, n);
        if (w <= 0) break;
        s += w; n -= (size_t)w;
    }
}

static void say_num(const char *tag, long v) {
    char b[48];
    int i = 0;
    while (tag[i]) { b[i] = tag[i]; i++; }
    if (v < 0) { b[i++] = '-'; v = -v; }
    char tmp[16]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t) b[i++] = tmp[--t];
    b[i++] = '\n'; b[i] = 0;
    say(b);
}

static int wait_pollin(int fd, int ms) {
    struct pollfd p = { fd, POLLIN, 0 };
    return poll(&p, 1, ms) == 1 && (p.revents & POLLIN);
}

int main(void) {
    int fd = timerfd_create(1 /* CLOCK_MONOTONIC */, TFD_NONBLOCK);
    if (fd < 0) { say("LINUX_TIMERFD: FAIL (create)\n"); return 1; }

    /* 1. 一次性 100ms */
    struct itimerspec ts;
    memset(&ts, 0, sizeof(ts));
    ts.it_value.tv_nsec = 100 * 1000000L; /* 100ms */
    if (timerfd_settime(fd, 0, &ts, 0) < 0) {
        say("LINUX_TIMERFD: FAIL (settime)\n"); return 1;
    }
    if (!wait_pollin(fd, 2000)) {
        say("LINUX_TIMERFD: FAIL (no poll after 100ms)\n"); return 1;
    }
    uint64_t exp = 0;
    if (read(fd, &exp, sizeof(exp)) != sizeof(exp) || exp < 1) {
        say("LINUX_TIMERFD: FAIL (read count)\n"); return 1;
    }
    say_num("tfd: one-shot exp ", (long)exp);

    /* 一次性后应 disarm：gettime 的 it_value 应为 0 */
    struct itimerspec cur;
    if (timerfd_gettime(fd, &cur) < 0) {
        say("LINUX_TIMERFD: FAIL (gettime)\n"); return 1;
    }
    if (cur.it_value.tv_sec != 0 || cur.it_value.tv_nsec != 0) {
        say("LINUX_TIMERFD: FAIL (not disarmed)\n"); return 1;
    }

    /* 2. 周期 50ms：读两次应各 >=1 */
    memset(&ts, 0, sizeof(ts));
    ts.it_interval.tv_nsec = 50 * 1000000L;
    ts.it_value.tv_nsec = 50 * 1000000L;
    if (timerfd_settime(fd, 0, &ts, 0) < 0) {
        say("LINUX_TIMERFD: FAIL (settime periodic)\n"); return 1;
    }
    uint64_t total = 0;
    for (int i = 0; i < 3; i++) {
        if (!wait_pollin(fd, 2000)) {
            say("LINUX_TIMERFD: FAIL (periodic poll)\n"); return 1;
        }
        uint64_t e2 = 0;
        if (read(fd, &e2, sizeof(e2)) != sizeof(e2) || e2 < 1) {
            say("LINUX_TIMERFD: FAIL (periodic read)\n"); return 1;
        }
        total += e2;
    }
    say_num("tfd: periodic total ", (long)total);
    if (total < 3) {
        say("LINUX_TIMERFD: FAIL (periodic count)\n"); return 1;
    }
    close(fd);
    say("LINUX_TIMERFD: PASS\n");
    return 0;
}
