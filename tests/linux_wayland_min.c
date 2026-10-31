/* 最小探针：只验证"这个编译产物在 HBOS 上能否启动"。
 * 用来把"程序加载问题"和"协议逻辑问题"分开。 */
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

static unsigned char big_buffer[4096];

int main(void) {
    out("wl-min: hello\n");
    big_buffer[0] = 1;
    big_buffer[4095] = 2;
    if (big_buffer[0] == 1 && big_buffer[4095] == 2) out("wl-min: bss ok\n");
    out("wl-min: PASS\n");
    return 0;
}
