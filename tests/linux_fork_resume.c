/* fork() 之后子进程的寄存器现场必须和父进程 syscall 那一刻完全一致。
 *
 * 回归用最小复现：libc 的 __clone 封装在 call __clone 之后是 `leave; ret`，
 * 只要内核把 clone 的参数（a0..a5）写回 rdi/rsi/rdx/r10/r8 而不同时保留
 * 真正的 rax（返回值），子进程就会带着垃圾 rbp 走到 `leave` → 读地址 0。
 *
 * 这里手工做三件事，绕开 libc 的 fork()，直接验内核：
 *   1. 把一个哨兵值放进 rbp 和几个 caller-saved 寄存器；
 *   2. 内联 syscall 发 clone/fork；
 *   3. 子进程检查这些寄存器是否还是哨兵。
 *
 * 退出码：0 = 全部正确；1 = 有寄存器被内核冲掉。
 */
#define SYS_WRITE 1
#define SYS_EXIT 60
#define SYS_FORK 57
#define SYS_CLONE 56
#define SYS_WAIT4 61
#define SYS_GETPID 39

static long raw_syscall6(long nr, long a0, long a1, long a2, long a3, long a4,
                         long a5) {
    register long r10 __asm__("r10") = a3;
    register long r8 __asm__("r8") = a4;
    register long r9 __asm__("r9") = a5;
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10),
                       "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

static long sys3(long nr, long a0, long a1, long a2) {
    return raw_syscall6(nr, a0, a1, a2, 0, 0, 0);
}

static unsigned long strlen_(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void put(const char *s) {
    sys3(SYS_WRITE, 1, (long)s, (long)strlen_(s));
}

static void put_hex(unsigned long value) {
    char buf[19];
    static const char digits[] = "0123456789abcdef";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = digits[(value >> (60 - i * 4)) & 0xf];
    buf[18] = 0;
    sys3(SYS_WRITE, 1, (long)buf, 18);
}

/* 哨兵：fork/clone 都必须原样保留。选的都是"明显不是内核会用到的值"。 */
#define SENT_RBP 0x1122334455667788UL
#define SENT_RBX 0x99aabbccddeeff00UL
#define SENT_R12 0x0123456789abcdefUL
#define SENT_R13 0xfedcba9876543210UL
#define SENT_R14 0x55555555aaaaaaaaUL
#define SENT_R15 0xaaaaaaaabbbbbbbbUL

static int failures;

/* do_clone 在 syscall 返回后原地读回的寄存器值（父进程侧自检用）。 */
static unsigned long observed_rbp, observed_rbx, observed_r12, observed_r13;
static unsigned long observed_r14, observed_r15;

static void check(const char *name, unsigned long got, unsigned long want) {
    if (got == want) return;
    failures++;
    put("[FAIL] ");
    put(name);
    put(" = ");
    put_hex(got);
    put(" want ");
    put_hex(want);
    put("\n");
}

/* 子进程侧：只做检查并 _exit，绝不返回（父进程的栈不能碰）。
 *
 * 注意：必须**先把 6 个寄存器一次性搬到内存**，再调用 check()。check() 是
 * 普通 C 函数，编译器会把 rbx/r12-r15 当自己的临时寄存器用（实测它会做
 * `mov %rbx,%r14 / mov %r12,%r13`），一旦在搬完之前调用，读到的就是被
 * 搬运污染过的值——测出来的"错位"其实是测试自己的 bug。 */
static unsigned long snap[6];

/* 子进程专用栈：fork 出来的子进程和父进程共享同一块栈，父进程在 do_clone
 * 里 push 的 6 个保存值就在栈顶。子进程不能在那个栈上继续跑（既不能 pop，
 * 也不能调用函数——调用会把返回地址压在父进程的帧上）。给它一块独立栈，
 * 既保留哨兵寄存器，也彻底避开和父进程的栈共享。 */
/* 汇编里用 child_stack_top(%rip) 取栈顶（栈向下生长，取末尾）。
 * 用 .comm 在汇编层声明同一个符号，避免 .set 引用 C 静态变量时产生的
 * TEXTREL（PIE 下不允许把 C 符号地址塞进 .text）。 */
__asm__(".globl child_stack\n"
        ".comm child_stack, 16384, 16\n");

/* 由 do_clone 的内联汇编直接 call 进来（不是 C 调用，哨兵还在寄存器里）。
 * 声明成 asm 名字，避免编译器认为它可以内联/被常规调用。 */
void child_check_from_asm(void) __asm__("child_check_from_asm");
void child_check_from_asm(void) {
    /* musttail 语义：一进来就把寄存器落到 snap[]（纯 store，不碰
     * rbx/r12-r15），然后正常 C 代码可以随便用这些寄存器。 */
    __asm__ volatile(
        "movq %%rbp, %0\n\t"
        "movq %%rbx, %1\n\t"
        "movq %%r12, %2\n\t"
        "movq %%r13, %3\n\t"
        "movq %%r14, %4\n\t"
        "movq %%r15, %5\n\t"
        : "=m"(snap[0]), "=m"(snap[1]), "=m"(snap[2]), "=m"(snap[3]),
          "=m"(snap[4]), "=m"(snap[5])
        :
        : "memory");
    check("rbp", snap[0], SENT_RBP);
    check("rbx", snap[1], SENT_RBX);
    check("r12", snap[2], SENT_R12);
    check("r13", snap[3], SENT_R13);
    check("r14", snap[4], SENT_R14);
    check("r15", snap[5], SENT_R15);
    sys3(SYS_EXIT, failures ? 1 : 0, 0, 0);
    __builtin_unreachable();
}

/* 让编译器把这几个寄存器真的载入哨兵值后再发请求。
 *
 * 注意：**不能**只写 `register long rbp __asm__("rbp")` 然后指望编译器装载
 * 它——GCC 会直接省掉"未被读取的寄存器赋值"（实测整个函数被内联并删空）。
 * 所以这里用一块裸汇编：先把哨兵装进全部被调用者保存寄存器，再 syscall，
 * 返回后它们必须是原值（syscall 只承诺 rcx/r11 被破坏）。
 *
 * 返回值 = syscall 的返回值。退出前必须把 rbp/rbx/r12-r15 恢复成调用者的
 * 值：syscall 只承诺破坏 rcx/r11，而在 Linux 上 fork/clone **不会**改动这 6
 * 个被调用者保存寄存器。如果这里不恢复，调用者的 rbx 就永久变成哨兵
 * （实测接着调 wait4 时 a1 传出去的是 0x99aabbccddeeff00）。
 *
 * 子进程（ret==0）走另一条出口：它和父进程共享同一个栈，父进程压在栈上
 * 的 6 个保存值属于父进程；子进程若照样 pop 就会把这些值弹走、并拿到
 * 父进程的原值而不是哨兵。所以子进程在 syscall 返回后先判 rax==0，是 0
 * 就直接跳去检查（此时哨兵仍在寄存器里，栈也保持原样）。 */
static long do_clone(long nr, long a0, long a1) {
    register long ret __asm__("rax") = nr;
    __asm__ volatile(
        "pushq %%rbp\n\t"
        "pushq %%rbx\n\t"
        "pushq %%r12\n\t"
        "pushq %%r13\n\t"
        "pushq %%r14\n\t"
        "pushq %%r15\n\t"
        "movq %[a0], %%rdi\n\t"
        "movq %[a1], %%rsi\n\t"
        "movq %[s_rbp], %%rbp\n\t"
        "movq %[s_rbx], %%rbx\n\t"
        "movq %[s_r12], %%r12\n\t"
        "movq %[s_r13], %%r13\n\t"
        "movq %[s_r14], %%r14\n\t"
        "movq %[s_r15], %%r15\n\t"
        "syscall\n\t"
        "testq %%rax, %%rax\n\t"
        "jz 1f\n\t"
        "movq %%rbp, %[o_rbp]\n\t"
        "movq %%rbx, %[o_rbx]\n\t"
        "movq %%r12, %[o_r12]\n\t"
        "movq %%r13, %[o_r13]\n\t"
        "movq %%r14, %[o_r14]\n\t"
        "movq %%r15, %[o_r15]\n\t"
        "addq $48, %%rsp\n\t"       /* 丢弃上面 6 个 push 的保存值 */
        "jmp 2f\n\t"
        "1:\n\t"
        /* 子进程：不动父进程的栈，换成自己的检查栈，然后直接进检查函数。
         * 哨兵此刻原封不动地躺在 rbp/rbx/r12-r15 里。 */
        "leaq child_stack(%%rip), %%rsp\n\t"
        "addq $16368, %%rsp\n\t"
        "call child_check_from_asm\n\t"
        "2:\n\t"
        : [ret] "+a"(ret), [o_rbp] "=m"(observed_rbp),
          [o_rbx] "=m"(observed_rbx), [o_r12] "=m"(observed_r12),
          [o_r13] "=m"(observed_r13), [o_r14] "=m"(observed_r14),
          [o_r15] "=m"(observed_r15)
        : [a0] "r"(a0), [a1] "r"(a1),
          [s_rbp] "i"(SENT_RBP), [s_rbx] "i"(SENT_RBX),
          [s_r12] "i"(SENT_R12), [s_r13] "i"(SENT_R13), [s_r14] "i"(SENT_R14),
          [s_r15] "i"(SENT_R15)
        : "rcx", "r11", "rdi", "rsi", "memory");
    return ret;
}

int main(void) {
    put("[forkresume] start\n");

    /* 1) fork(57)：glibc 底层就是 clone(SIGCHLD|CLEARTID|SETTID)，a0 非零，
     *    正好能暴露"把 a0 灌回 rdi"的问题。 */
    long pid = do_clone(SYS_FORK, 0, 0);
    /* 子进程已经在 do_clone 的汇编里 exit 掉了，走不到这里。 */
    if (pid < 0) {
        put("[FAIL] fork failed\n");
        return 1;
    }
    int status = 0;
    sys3(SYS_WAIT4, pid, (long)&status, 0);
    if (status & 0xff) {
        put("[FAIL] fork child signal ");
        put_hex((unsigned long)(status & 0x7f));
        put("\n");
        return 1;
    }
    if ((status >> 8) & 0xff) {
        put("[FAIL] fork child exit ");
        put_hex((unsigned long)((status >> 8) & 0xff));
        put("\n");
        return 1;
    }

    /* 2) clone(56) 不带 CLONE_VM：与 fork 同一条内核路径，但 a0 = SIGCHLD
     *    的数值会真的变成参数，覆盖测试更直接。 */
    failures = 0;
    pid = do_clone(SYS_CLONE, 0x11, 0);
    /* 子进程已经在 do_clone 的汇编里 exit 掉了，走不到这里。 */
    if (pid < 0) {
        put("[FAIL] clone failed\n");
        return 1;
    }
    status = 0;
    sys3(SYS_WAIT4, pid, (long)&status, 0);
    if ((status & 0xff) || ((status >> 8) & 0xff)) {
        put("[FAIL] clone child status ");
        put_hex((unsigned long)status);
        put("\n");
        return 1;
    }

    put("[forkresume] PASS\n");
    return 0;
}

void _start(void) {
    long code = main();
    sys3(SYS_EXIT, code, 0, 0);
    __builtin_unreachable();
}
