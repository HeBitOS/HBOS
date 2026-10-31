/* 栈对齐探针：打印 main 入口附近的 rsp % 16 和几个局部变量的地址对齐。
 *
 * SysV ABI 要求函数体内 rsp%16 == 8（进函数前 0，call 压返回地址后变 8），
 * GCC 据此把 16 字节对齐的局部变量放在 rsp+8/24/... 上。如果实际只有 8
 * 字节对齐，movaps/movdqa 会 #GP，任务直接死掉 —— 表现是 rsp=0 的
 * [SCHED] BAD CTX。这个探针就是为了把这种情况一眼看出来。 */
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

static void out_hex(unsigned long value) {
    char buf[18];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int nib = (int)((value >> ((15 - i) * 4)) & 0xF);
        buf[2 + i] = (char)(nib < 10 ? '0' + nib : 'A' + nib - 10);
    }
    raw3(SYS_WRITE, 1, (long)buf, 18);
}

int main(void) {
    unsigned long sp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    out("align: rsp=");
    out_hex(sp);
    out(" rsp%16=");
    char d[4];
    d[0] = (char)('0' + (sp & 15));
    raw3(SYS_WRITE, 1, (long)d, 1);
    out("\n");

    /* 谁调的 main？返回地址落在哪个模块：
     *   0x0000011000xxxx = 本程序自己（PIE 基址 0x1100000000）
     *   0x0000012xxxxxxxx = 内核 / 内置运行时
     *   其他（如 0x7f..）= 共享库（libc.so.6）
     * main 的栈帧里 rsp+8 是第一个被 callee-save 挤下去的位置，
     * 这里直接从栈上取返回地址更省事：__builtin_return_address(0)。 */
    unsigned long caller = (unsigned long)__builtin_return_address(0);
    out("align: caller=");
    out_hex(caller);
    out("\n");

    unsigned long main_addr = (unsigned long)(void *)main;
    out("align: main@");
    out_hex(main_addr);
    out("\n");

    /* 一个 16 字节对齐的结构体：如果编译器在这里生成 movaps，
     * 而栈实际只有 8 对齐，这条就会把任务打死。 */
    struct { long a, b; } local;
    local.a = 1;
    local.b = 2;
    (void)local;
    out("align: survived\n");
    out("align: PASS\n");
    return 0;
}
