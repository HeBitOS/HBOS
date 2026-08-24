#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* G1 mremap 验证（Chromium base 缓冲增长）。匿名映射写入数据后：
 * 收缩数据保留、原地扩展新页清零、MAYMOVE 移动后数据保留。 */

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

#define PG 4096

int main(void) {
    /* 1. 4 页匿名映射，写入 0x5A */
    char *p = (char *)mmap(0, 4 * PG, 3 /*RW*/, 0x22 /*PRIVATE|ANON*/, -1, 0);
    if (p == MAP_FAILED) { say("LINUX_MREMAP: FAIL (mmap)\n"); return 1; }
    for (int i = 0; i < 4 * PG; i++) p[i] = (char)0x5A;

    /* 2. 收缩到 2 页：数据保留 */
    char *s1 = (char *)mremap(p, 4 * PG, 2 * PG, 0);
    if (s1 != p || s1[0] != 0x5A || s1[2 * PG - 1] != 0x5A) {
        say("LINUX_MREMAP: FAIL (shrink)\n"); return 1;
    }

    /* 3. 扩展回 4 页：旧数据(页0-1)保留，新页(2-3)清零 */
    char *s2 = (char *)mremap(s1, 2 * PG, 4 * PG, 0);
    if (s2 != s1 || s2[0] != 0x5A || s2[PG - 1] != 0x5A ||
        s2[2 * PG] != 0 || s2[3 * PG] != 0) {
        say("LINUX_MREMAP: FAIL (grow)\n"); return 1;
    }

    /* 4. MAYMOVE 扩展：地址可能变；页0-1 数据保留，新页清零 */
    char *s3 = (char *)mremap(s2, 4 * PG, 8 * PG, MREMAP_MAYMOVE);
    if (s3 == MAP_FAILED || s3[0] != 0x5A || s3[PG - 1] != 0x5A ||
        s3[2 * PG] != 0 || s3[7 * PG - 1] != 0) {
        say("LINUX_MREMAP: FAIL (maymove)\n"); return 1;
    }
    say_num("mremap: final addr ", (long)s3);
    munmap(s3, 8 * PG);
    say("LINUX_MREMAP: PASS\n");
    return 0;
}
