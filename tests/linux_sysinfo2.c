#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <statfs.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>

/* G1 系统 ABI 补充验证：statfs/fstatfs、sched_getaffinity、AF_INET
 * socket 选项（SO_SNDBUF/SO_RCVBUF/SO_KEEPALIVE/TCP_NODELAY）。
 * 这些都是 Chromium base 层启动早期就会调用的接口。 */

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

int main(void) {
    /* 1. statfs */
    struct statfs st;
    if (statfs("/", &st) < 0 || st.f_bsize != 4096) {
        say("LINUX_SYSINFO2: FAIL (statfs)\n"); return 1;
    }
    say_num("statfs: bsize ", st.f_bsize);
    int f = open("/sysinfo2.txt", 1 | 0x40 | 0x200, 0644); /* 独立文件，别截断共享的 /a.txt */
    if (f < 0) { say("LINUX_SYSINFO2: FAIL (open)\n"); return 1; }
    if (fstatfs(f, &st) < 0 || st.f_bsize != 4096) {
        say("LINUX_SYSINFO2: FAIL (fstatfs)\n"); return 1;
    }
    say_num("fstatfs: bsize ", st.f_bsize);
    close(f);

    /* 2. sched_getaffinity：至少 1 个 CPU 位 */
    unsigned long mask[4];
    int m = sched_getaffinity(0, sizeof(mask), mask);
    if (m <= 0 || (mask[0] & 1) == 0) {
        say("LINUX_SYSINFO2: FAIL (affinity)\n"); return 1;
    }
    say_num("affinity: cpu0 bit ", (long)(mask[0] & 1));

    /* 3. AF_INET socket 选项 */
    int s = socket(2, 1, 0);
    if (s < 0) { say("LINUX_SYSINFO2: FAIL (socket)\n"); return 1; }
    int v = 0;
    socklen_t vl = sizeof(v);
    if (getsockopt(s, 1, 7, &v, &vl) < 0 || v <= 0) { /* SO_SNDBUF */
        say("LINUX_SYSINFO2: FAIL (SO_SNDBUF)\n"); return 1;
    }
    say_num("SO_SNDBUF ", v);
    v = 0;
    if (getsockopt(s, 1, 8, &v, &vl) < 0 || v <= 0) { /* SO_RCVBUF */
        say("LINUX_SYSINFO2: FAIL (SO_RCVBUF)\n"); return 1;
    }
    say_num("SO_RCVBUF ", v);
    v = 1;
    if (setsockopt(s, 6, 1, &v, sizeof(v)) < 0) { /* TCP_NODELAY */
        say("LINUX_SYSINFO2: FAIL (TCP_NODELAY)\n"); return 1;
    }
    v = 1;
    if (setsockopt(s, 1, 9, &v, sizeof(v)) < 0) { /* SO_KEEPALIVE */
        say("LINUX_SYSINFO2: FAIL (SO_KEEPALIVE)\n"); return 1;
    }
    close(s);

    say("LINUX_SYSINFO2: PASS\n");
    return 0;
}
