/* 大 .bss 探针：验证"镜像跨到第 6 页时能否正常启动"。
 * 上一版 4KB 数组被 GCC 优化成两个独立变量，实际 .bss 很小，
 * 所以没测到真正的大 bss 场景。这里用 volatile 强制它留在 .bss。 */
#define SYS_WRITE 1

static long raw3(long nr, long a0, long a1, long a2) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return r;
}

static void out(const char *text) {
    long n = 0;
    while (text[n]) n++;
    raw3(SYS_WRITE, 1, (long)text, n);
}

static volatile unsigned char big_buffer[8192];

int main(void) {
    out("wl-big: hello\n");
    unsigned sum = 0;
    for (unsigned i = 0; i < sizeof(big_buffer); i++) {
        big_buffer[i] = (unsigned char)(i & 0xFF);
        sum += big_buffer[i];
    }
    out("wl-big: sum=");
    char digits[12];
    int pos = 0;
    if (!sum) digits[pos++] = '0';
    while (sum) { digits[pos++] = (char)('0' + sum % 10); sum /= 10; }
    char rev[12];
    for (int i = 0; i < pos; i++) rev[i] = digits[pos - 1 - i];
    raw3(SYS_WRITE, 1, (long)rev, pos);
    out("\nwl-big: PASS\n");
    return 0;
}
