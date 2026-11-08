#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "cpu.h"
#include "io.h"
#include "vmm.h"
extern int linux_debug_syscall_dump;
#include "task.h"
#include "../string.h"
#include "../syscall.h"
#include "../signal.h"
#include "../smp.h"
#include "../graphics/graphics.h"

// ============================================================
// Hex printing helpers (used by isr_handler)
// ============================================================
static void print_hex64(uint64_t v) {
    int started = 0;
    for (int i = 15; i >= 0; i--) {
        int d = (v >> (i*4)) & 0xF;
        if (d || started || i == 0) {
            started = 1;
            console_putchar(d < 10 ? '0' + d : 'A' + d - 10);
        }
    }
}
static void print_hex16(uint16_t v) {
    int started = 0;
    for (int i = 3; i >= 0; i--) {
        int d = (v >> (i*4)) & 0xF;
        if (d || started || i == 0) {
            started = 1;
            console_putchar(d < 10 ? '0' + d : 'A' + d - 10);
        }
    }
}

/** 打印十进制（栈帧序号用）。
 *
 * 用户态崩溃只有 RIP 时，只能看到"最后一跳"，看不到是谁调过来的：
 * Chromium 这种 280 MB 的二进制里，一个 CHECK 失败往往发生在深层调用里。
 * 这里沿 RBP 链把返回地址全部打出来——宿主侧拿同一份 ELF 用
 * `objdump -d --start-address=<ret - base>` 就能反查到具体调用点。
 * 内核没开 SMAP/SMEP，用户页可直接读；但仍逐页用 vmm_get_phys() 校验，
 * 避免把野指针当成栈帧读崩内核。 */
static void print_dec(int value) {
    char buf[16];
    int n = 0;
    if (value < 0) { console_putchar('-'); value = -value; }
    do { buf[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n--) console_putchar(buf[n]);
}

/* 把一个 32 位十六进制数写成 "0x1234ABCD"（无前导零填充以外的装饰）。
 * 内核没有 snprintf（见 src/tools/distro_seed.c 的同类注记），反汇编
 * 助记符只能自己拼。返回写入长度。 */
static int fmt_hex_into(char *dst, size_t cap, const char *prefix,
                        uint32_t value) {
    size_t n = 0;
    while (prefix && *prefix && n + 1 < cap) dst[n++] = *prefix++;
    for (int shift = 28; shift >= 0; shift -= 4) {
        if (n + 1 >= cap) break;
        int d = (int)((value >> shift) & 0xF);
        dst[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    dst[n < cap ? n : cap - 1] = 0;
    return (int)n;
}

static int user_va_range_mapped(uint64_t address, uint64_t length) {
    if (!address || length == 0) return 0;
    if (address >= 0x0000800000000000ULL ||
        length > 0x0000800000000000ULL - address)
        return 0;
    uint64_t first = address & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last = (address + length - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t va = first; va <= last; va += PAGE_SIZE)
        if (!vmm_get_phys(va)) return 0;
    return 1;
}

/* 故障现场寄存器快照。地址 0x0 的 SIGSEGV 几乎总是"某个指针是 NULL"，
 * 只看 rip 无法区分是哪一个——把这几个 gpr 打出来，配合反汇编就能直接
 * 认出 [reg+off] 里哪个 reg 是空。 */
static void dump_user_trap_regs(const isr_regs_t *regs) {
    static const char *const names[8] = {"rax", "rbx", "rcx", "rdx",
                                         "rsi", "rdi", "rbp", "rsp"};
    const uint64_t values[8] = {regs->rax, regs->rbx, regs->rcx, regs->rdx,
                                regs->rsi, regs->rdi, regs->rbp, regs->rsp};
    /* 谁崩的：pid/tid 与它是不是进程主线程。共享库里 NULL 解引用时，
     * 单看地址分不清是主进程还是 clone 出来的兄弟线程——而两者的排查
     * 方向完全不同（线程要查 clone 参数，进程要查 exec 参数）。 */
    {
        /* 注意：崩溃处理器运行在**中断上下文**（开中断之后 current_task 可能
         * 已经被抢走），所以先存快照再做 print_dec 这类会写串口的慢操作，
         * 别在中途去问"当前任务是谁"。 */
        task_t *faulted = task_current();
        int fault_tid = faulted ? (int)faulted->id : -1;
        int fault_pid = faulted ? (int)task_get_process_id() : -1;
        console_puts("  fault pid=");
        print_dec(fault_pid);
        console_puts(" tid=");
        print_dec(fault_tid);
        console_puts(fault_pid == fault_tid ? " (main)\n" : " (thread)\n");
        /* print_hex64 自带 "0x" 前缀，这里不要再手写一个。 */
        console_puts("  err=");
        print_hex64(regs->err_code);
        console_puts(" cs=");
        print_hex64(regs->cs);
        console_putchar('\n');
    }
    for (int i = 0; i < 8; i++) {
        console_puts(i == 0 ? "  regs: " : " ");
        console_puts(names[i]);
        console_puts("=0x");
        print_hex64(values[i]);
        if (i == 7) console_putchar('\n');
    }
    console_puts("        r8=0x");
    print_hex64(regs->r8);
    console_puts(" r9=0x");
    print_hex64(regs->r9);
    console_puts(" r10=0x");
    print_hex64(regs->r10);
    console_puts(" r12=0x");
    print_hex64(regs->r12);
    console_puts(" r13=0x");
    print_hex64(regs->r13);
    console_puts(" r14=0x");
    print_hex64(regs->r14);
    console_puts(" r15=0x");
    print_hex64(regs->r15);
    console_putchar('\n');
}

/* 原始栈窗口。Chromium/libc 这类发行版二进制都是 -O2 编译、默认省掉
 * 帧指针（-fomit-frame-pointer），RBP 链往往一个有效帧都没有。真正的
 * 救命手段是把崩溃点附近的栈原样打出来：宿主侧拿同一份 ELF 反汇编
 * （objdump -d）就能把里面"长得像返回地址"的那几个字对回调用点。 */
#define USER_STACK_DUMP_WORDS 32
static void dump_user_stack_window(uint64_t rsp) {
    if (!user_va_range_mapped(rsp, 8)) return;
    console_puts("  stack @0x");
    print_hex64(rsp);
    console_putchar('\n');
    /* 栈窗口里的 ASCII 段
     *
     * 同一屏崩溃现场里，栈上偶尔残留可打印字符串（路径、断言表达式、
     * 命令行开关名）。按 1 字节滑窗扫前 256 字节，要求连续 >= 8 个可打印
     * 字符才认定为文本——这样不会把普通整数/指针误判成"字符串"，但一旦
     * 命中往往直接指向"崩在哪条配置或哪个路径上"，比一堆十六进制好读得多。
     * 最多打 4 段，避免把串口刷满（QEMU 串口是逐字符模拟的）。 */
    int strings_shown = 0;
    for (int off = 0; off + 8 <= 0x100 && strings_shown < 4; off++) {
        uint64_t addr = rsp + (uint64_t)off;
        if (!user_va_range_mapped(addr, 1)) break;
        const char *p = (const char *)(uintptr_t)addr;
        int len = 0;
        while (len < 64 && user_va_range_mapped(addr + (uint64_t)len, 1)) {
            char c = p[len];
            if (c < 0x20 || c > 0x7E) break;
            len++;
        }
        if (len >= 8) {
            if (strings_shown == 0) console_puts("  stack-str:");
            console_puts(" +0x");
            print_hex64((uint64_t)off);
            console_puts(" \"");
            for (int k = 0; k < len; k++) console_putchar(p[k]);
            console_puts("\"");
            strings_shown++;
            off += len - 1;
        }
    }
    if (strings_shown) console_putchar('\n');
    for (int i = 0; i < USER_STACK_DUMP_WORDS; i++) {
        uint64_t address = rsp + (uint64_t)i * 8;
        if (!user_va_range_mapped(address, 8)) break;
        uint64_t value = *(volatile uint64_t *)(uintptr_t)address;
        if ((i % 4) == 0) {
            console_puts("    +");
            print_hex64((uint64_t)i * 8);
            console_puts(": ");
        }
        /* 栈上绝大多数是小整数和标志位，打满 16 位十六进制既占带宽又难扫。
         * 高 32 位为 0 时只打低 32 位——这是排查时最常看的那一类值。 */
        if (value <= 0xFFFFFFFFULL) print_hex64(value & 0xFFFFFFFFULL);
        else print_hex64(value);
        console_putchar(((i % 4) == 3) ? '\n' : ' ');
    }
    if ((USER_STACK_DUMP_WORDS % 4) != 0) console_putchar('\n');
}

/* 把故障 rip 落到它所属的 VMA 里。只打印 rip 和它前后最近的几个区间，
 * 就能区分三种情况：
 *   1. rip 落在某个 .so 的 PT_LOAD 区间 → 用 映射起点 - 文件最小 vaddr
 *      就能换算成文件偏移；
 *   2. rip 落在堆/栈/匿名映射 → 说明跳转目标本身是野指针；
 *   3. rip 根本没被映射、或掉在区间外 → 加载器把库映射少了。 */
static void dump_user_vma_for(uint64_t rip) {
    task_t *faulted = task_current();
    /* current 在崩溃路径上可能已经被换掉（比如进程刚被 mark_dead），
     * 那样 task->mm 就是别人的地址空间，rip 自然"落在任何映射外"。
     * 遍历所有活动任务，挑出真正包含这个 rip 的那个地址空间。 */
    if (!faulted || !faulted->mm) return;
    task_mm_t *mm = faulted->mm;
    int contains = 0;
    for (vm_area_t *area = mm->areas; area; area = area->next)
        if (rip >= area->start && rip < area->end) { contains = 1; break; }
    if (!contains) {
        int active = task_get_count();
        for (int i = 0; i < active; i++) {
            const task_t *candidate = task_get_active((uint32_t)i);
            if (!candidate || !candidate->mm) continue;
            for (vm_area_t *area = candidate->mm->areas; area;
                 area = area->next) {
                if (rip >= area->start && rip < area->end) {
                    mm = candidate->mm;
                    contains = 1;
                    break;
                }
            }
            if (contains) break;
        }
    }
    if (!mm || !mm->areas) return;
    int printed = 0;
    for (vm_area_t *area = mm->areas; area; area = area->next) {
        if (printed >= 6) break;
        /* 只看 rip 附近的：完全在 rip 之下的跳过（最多保留最近一个）。 */
        if (area->end < rip) continue;
        if (area->start > rip + 0x10) break;
        console_puts(printed == 0 ? "  vma: " : "       ");
        print_hex64(area->start);
        console_puts("-");
        print_hex64(area->end);
        console_puts(" back=");
        print_dec((int)area->backing_type);
        console_puts(" off=0x");
        print_hex64(area->backing_offset);
        if (rip >= area->start && rip < area->end) {
            console_puts("  <- rip  (+0x");
            print_hex64(rip - area->start);
            console_puts(")");
        }
        console_putchar('\n');
        printed++;
    }
    if (printed == 0) {
        console_puts("  vma: (rip 不在任何映射区间内)\n");
        /* rip 不在任何 VMA 里，最可能的原因是**取指落在一段确实映射过、
         * 但 VMA 记录被 mremap 缩小时漏改的区间**（Linux 语义里 mremap
         * 缩小的尺寸必须正好等于 VMA 大小，Chromium 的分区分配器据此把
         * 一个大区拆成多段；内核这边对不上的部分返回了 EFAULT，VMA 链表
         * 就与真实页表脱节）。VMA 链表本身这时已经不可信，改从页表反查：
         * 从 rip 所在页向上/向下走，遇到 PTE 为 0 或权限变化就停，
         * 得到"真实映射段"。这一段的起止 + PTE flags 就是判断
         * "到底映射过没有、能不能执行"的唯一可靠依据。 */
        uint64_t p0 = rip & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t flags0 = vmm_get_page_flags(p0);
        if (flags0) {
            uint64_t lo2 = p0;
            for (int i = 0; i < 8192 && lo2 >= PAGE_SIZE; i++) {
                uint64_t prev = lo2 - PAGE_SIZE;
                if (!vmm_get_phys(prev) ||
                    vmm_get_page_flags(prev) != flags0) break;
                lo2 = prev;
            }
            uint64_t hi2 = p0 + PAGE_SIZE;
            for (int i = 0; i < 8192; i++) {
                if (!vmm_get_phys(hi2) ||
                    vmm_get_page_flags(hi2) != flags0) break;
                hi2 += PAGE_SIZE;
            }
            console_puts("  pte-span: ");
            print_hex64(lo2);
            console_puts("-");
            print_hex64(hi2);
            console_puts(" flags=");
            print_hex64(flags0);
            console_puts(" size=0x");
            print_hex64(hi2 - lo2);
            console_putchar('\n');
        }
        /* VMA 链表里离 rip 最近的两段：一段在下面、一段在上面。
         * mremap 把 VMA start 改错时，这里能直接看出"洞"有多大。 */
        uint64_t below_end = 0, above_start = ~(uint64_t)0;
        for (vm_area_t *area = mm->areas; area; area = area->next) {
            if (area->end <= rip && area->end > below_end)
                below_end = area->end;
            if (area->start > rip && area->start < above_start)
                above_start = area->start;
        }
        console_puts("  vma-gap: below_end=");
        print_hex64(below_end);
        console_puts(" above_start=");
        print_hex64(above_start == ~(uint64_t)0 ? 0 : above_start);
        console_putchar('\n');
    }

    /* 只看 VMA 链表是不够的：库/mmap 有些路径只建页表不登 VMA。直接把
     * rip 所在页的页表项翻出来，P/W/U/NX 一看就知道 rip 这一页到底存在
     * 不存在、能不能执行。 */
    uint64_t page = rip & ~(uint64_t)(PAGE_SIZE - 1);
    console_puts("  rip page pa=");
    print_hex64(vmm_get_phys(page));
    console_puts(" flags=");
    print_hex64(vmm_get_page_flags(page));
    /* 向上/向下各探 16 MB，找出这段连续映射的真正起止——库的映射基址
     * 用肉眼算不准，直接问页表最可靠。 */
    uint64_t lo = page;
    for (int i = 0; i < 4096 && lo; i++) {
        uint64_t prev = lo - PAGE_SIZE;
        if (!vmm_get_phys(prev)) break;
        if (vmm_get_page_flags(prev) != vmm_get_page_flags(page)) break;
        lo = prev;
    }
    uint64_t hi = page + PAGE_SIZE;
    for (int i = 0; i < 4096; i++) {
        if (!vmm_get_phys(hi)) break;
        if (vmm_get_page_flags(hi) != vmm_get_page_flags(page)) break;
        hi += PAGE_SIZE;
    }
    console_puts("  same-perm span=");
    print_hex64(lo);
    console_puts("-");
    print_hex64(hi);
    console_puts(" (rip at +0x");
    print_hex64(rip - lo);
    console_puts(")\n");
}

/* ── 反汇编 rip 前后各两条指令 ────────────────────────────────────────
 *
 * 为什么要这个：Chromium 的 crash handler 第一次调用会 `int3`（-1 作为
 * 信号值），内核这边看到的就是一条光秃秃的 `cc`；而 Chromium 的
 * `CHECK` 失败分支则是 `cc; ud2`。两者的区别**只在前后指令里**：
 *
 *   ... e8 xx xx xx xx   call logging::CheckFatal
 *   cc                   int3        <- crash handler 的 trap
 *   ud2                  CHECK 失败的兜底
 *
 * 只要把 rip 前面那几条 call 的返回地址算出来，就能顺着 `call rel32`
 * 落点去二进制里找字符串引用，拿到 (文件, 行号, 表达式) 三元组。
 * 反过来，如果前面是 `mov edx, <行号>; call ...`，那就是直接 CHECK。
 *
 * 这是个**只认常见编码**的最小反汇编器（x86-64 子集），不认识就原样
 * 打字节并标注 "?"，绝不猜。目的是省掉"重新起一次 QEMU + 手算偏移"的
 * 半天，而不是做通用反汇编。 */
/** 从 va 处解析一条指令，返回长度；op 里回填助记符与目标。
 *  只处理这次真正用到的几类：call/jmp rel32、jcc rel8/rel32、int3、
 *  ud2、ret、mov esi,imm32、lea r?, [rip+d]、push/pop、nop。 */
typedef struct {
    char     text[48];
    uint64_t call_target;   /* call/jmp rel32 的落点，0 = 不是 */
    uint32_t imm32;
} user_insn_t;

static int decode_user_insn(uint64_t va, user_insn_t *out) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)va;
    out->call_target = 0;
    out->imm32 = 0;
    if (!user_va_range_mapped(va, 16)) return 0;
    uint8_t b0 = p[0];
    char *t = out->text;
    size_t cap = sizeof(out->text);
    if (b0 == 0xCC) { strcpy(t, "int3"); return 1; }
    if (b0 == 0x90) { strcpy(t, "nop"); return 1; }
    if (b0 == 0xC3) { strcpy(t, "ret"); return 1; }
    if (b0 == 0x0F && p[1] == 0x0B) { strcpy(t, "ud2"); return 2; }
    if (b0 == 0xE8 || b0 == 0xE9) {
        int32_t rel = (int32_t)((uint32_t)p[1] | ((uint32_t)p[2] << 8) |
                                ((uint32_t)p[3] << 16) |
                                ((uint32_t)p[4] << 24));
        out->call_target = va + 5 + (uint64_t)(int64_t)rel;
        strcpy(t, b0 == 0xE8 ? "call" : "jmp");
        strcat(t, " -> ");
        fmt_hex_into(t + strlen(t), cap - strlen(t), "0x",
                     (uint32_t)out->call_target);
        return 5;
    }
    if (b0 >= 0x70 && b0 <= 0x7F) {
        int8_t rel = (int8_t)p[1];
        strcpy(t, "jcc ");
        fmt_hex_into(t + strlen(t), cap - strlen(t), "+0x",
                     (uint32_t)(int32_t)rel);
        return 2;
    }
    if (b0 == 0x0F && p[1] >= 0x80 && p[1] <= 0x8F) {
        int32_t rel = (int32_t)((uint32_t)p[2] | ((uint32_t)p[3] << 8) |
                                ((uint32_t)p[4] << 16) |
                                ((uint32_t)p[5] << 24));
        strcpy(t, "jcc32 ");
        fmt_hex_into(t + strlen(t), cap - strlen(t), "-> 0x",
                     (uint32_t)(va + 6 + (int64_t)rel));
        return 6;
    }
    /* mov esi/edi/edx/ecx, imm32：BE / BF / BA / B9。CHECK 的行号就是
     * 这么传进 CheckFatal 的。 */
    if (b0 == 0xBE || b0 == 0xBF || b0 == 0xBA || b0 == 0xB9) {
        out->imm32 = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
                     ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
        strcpy(t, "mov ");
        strcat(t, b0 == 0xBE ? "esi" : b0 == 0xBF ? "edi" :
                   b0 == 0xBA ? "edx" : "ecx");
        strcat(t, ", ");
        fmt_hex_into(t + strlen(t), cap - strlen(t), "0x", out->imm32);
        return 5;
    }
    /* lea r?, [rip+d]：只有 48/4C 8D /r 且 mod=00 rm=101。 */
    if ((b0 == 0x48 || b0 == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05) {
        int32_t d = (int32_t)((uint32_t)p[3] | ((uint32_t)p[4] << 8) |
                              ((uint32_t)p[5] << 16) |
                              ((uint32_t)p[6] << 24));
        out->call_target = va + 7 + (uint64_t)(int64_t)d;
        strcpy(t, "lea rip-relative -> ");
        fmt_hex_into(t + strlen(t), cap - strlen(t), "0x",
                     (uint32_t)out->call_target);
        return 7;
    }
    if (b0 == 0x55) { strcpy(t, "push rbp"); return 1; }
    if (b0 == 0x5D) { strcpy(t, "pop rbp"); return 1; }
    /* 其余：原样打 4 字节并明确标 "?"，绝不装作认得。 */
    strcpy(t, "? ");
    for (int i = 0; i < 4; i++) {
        static const char hexd[] = "0123456789ABCDEF";
        size_t n = strlen(t);
        if (n + 3 >= cap) break;
        t[n] = hexd[p[i] >> 4];
        t[n + 1] = hexd[p[i] & 0xF];
        t[n + 2] = ' ';
        t[n + 3] = 0;
    }
    return 4;
}

/** 读一段用户内存（诊断用）。
 *
 * 不变量：成功返回 1 并把 length 字节拷进 dst；任一页未映射则返回 0
 * 且 dst 内容不可信。
 * 前置条件：dst 可为 NULL 吗？不能 —— 调用方要读值。
 * 说明：直接走 vmm_get_phys 逐页拷，不做权限检查：这是内核自己的诊断
 * 路径，用户态已经死了，读不出东西就返回 0，不会有安全后果。 */
static int gdt_peek_user(uint64_t address, void *dst, size_t length) {
    uint8_t *out = (uint8_t *)dst;
    while (length) {
        uint64_t page = address & ~(uint64_t)0xfffULL;
        uint64_t offset = address - page;
        uint64_t chunk = 0x1000ULL - offset;
        if (chunk > length) chunk = length;
        uint64_t phys = vmm_get_phys(page);
        if (!phys) return 0;
        const uint8_t *in = (const uint8_t *)(uintptr_t)(phys + offset);
        for (uint64_t i = 0; i < chunk; i++) out[i] = in[i];
        out += chunk;
        address += chunk;
        length -= chunk;
    }
    return 1;
}

/** 打印 stack canary 的三方对照：%fs:0x28 vs 栈上保存值。
 *
 * 为什么需要：Chromium 的 `__stack_chk_fail` 路径长这样——
 *   mov  %fs:0x28,%rax
 *   cmp  -0x30(%rbp),%rax
 *   jne  __stack_chk_fail
 * 两者不等就说明"栈被写坏"或者"canary 根本没初始化"。这两种情况的
 * 修法完全不同：前者是内存越界，后者是内核没给对 AT_RANDOM / TLS，
 * 光看崩溃地址分不出来。
 *
 * 栈上 canary 的位置依赖被调用函数自己的帧布局，这里把 rbp 往下扫
 * 一段，把所有看起来像 canary（低位非零、且与 %fs:0x28 有交集）的
 * 候选都列出来对比。
 *
 * 不变量：无返回值；只往串口写，不改任何状态。
 * 前置条件：regs 必须有效（指向真实的 trap 现场）。 */
static void dump_user_canary(uint64_t rbp) {
    task_t *current = task_current();
    if (!current) return;
    uint64_t fs_base = current->fs_base;
    console_puts("  canary: fs_base=0x");
    print_hex64(fs_base);
    uint64_t fs28 = 0;
    if (gdt_peek_user(fs_base + 0x28, &fs28, sizeof(fs28))) {
        console_puts(" fs:0x28=0x");
        print_hex64(fs28);
    } else {
        console_puts(" fs:0x28=UNMAPPED");
    }
    console_puts(" rbp=0x");
    print_hex64(rbp);
    /* 精确打印几个"像 canary"的位置：Chromium 各函数把 canary 存放在
     * 自己的 rbp-N（N 因帧大小而异）。失配的那个位置被写坏 = 栈溢出；
     * 全部一致 = 不是 canary 的问题，得另找。 */
    for (uint64_t off = 0x08; off <= 0x40; off += 8) {
        uint64_t v = 0;
        if (!gdt_peek_user(rbp - off, &v, sizeof(v))) break;
        console_puts(" -");
        print_hex64(off);
        console_puts("=0x");
        print_hex64(v);
        if (v == fs28) console_puts("(CANARY)");
    }
    console_puts("\n");
}

/** 打印 rip 前 16 条 / 后 4 条指令。
 *
 * 前 16 条是为了回溯"崩之前调用了谁"——Chromium 的 crash handler 一进来
 * 就 `int3`/`ud2`，真正的调用者在 rip 之前若干条 `call` 里。 */
static void dump_user_tail(uint64_t rip) {
    /* x86 是变长指令，`rip - 48` 几乎必然落在某条指令中间，单向前向解码
     * 从错位处起跑就一路错到底（r71 的实测现象：在 `leave; ret` 上打出
     * `? FF FF C9 C3`）。
     *
     * 正确做法是反向枚举起点：真正的指令边界只能是 rip-1 … rip-15 之一
     * （x86-64 单指令最长 15 字节）。对每个候选起点前向解码，谁的落点
     * 恰好等于 rip 谁就是正确的对齐点；多个候选都命中时取**最早**的那个
     * —— 更早的起点会插进 rip 之前、不属于这条代码路径的字节。 */
    struct { uint64_t va; user_insn_t in; } hist[32];
    int n = 0;
    for (int back = 1; back <= 15 && rip >= (uint64_t)back; back++) {
        uint64_t start = rip - (uint64_t)back;
        uint64_t p = start;
        int cnt = 0;
        uint64_t vas[32];
        user_insn_t ins[32];
        for (int guard = 0; guard < 32 && p < rip; guard++) {
            user_insn_t in;
            int len = decode_user_insn(p, &in);
            if (!len) break;
            if (cnt < 32) { vas[cnt] = p; ins[cnt] = in; cnt++; }
            p += (uint64_t)len;
            if (p > rip) break;
        }
        if (p != rip || cnt == 0) continue;
        /* 命中了：把这条解码链搬到 hist（只留最后 16 条）。 */
        n = 0;
        int first = cnt > 16 ? cnt - 16 : 0;
        for (int i = first; i < cnt; i++) {
            hist[n].va = vas[i];
            hist[n].in = ins[i];
            n++;
        }
        break;
    }
    if (n == 0) {
        console_puts("  code: (rip 前无法对齐解码)\n");
    }
    int first = n > 16 ? n - 16 : 0;
    for (int i = first; i < n; i++) {
        console_puts("  code ");
        print_hex64(hist[i].va);
        console_puts(hist[i].va == rip ? ": * " : ":   ");
        console_puts(hist[i].in.text);
        /* call 落点如果可读，顺手在它前 96 字节里找一个 rip 相对 lea 的
         * 字符串引用：CheckFatal 一进去就会 lea 到 "CHECK failed: ..."，
         * 或者 lea 到文件名/表达式，直接给出行号三元组的前两段。 */
        uint64_t target = hist[i].in.call_target;
        if (target && hist[i].in.text[0] == 'c') {
            if (user_va_range_mapped(target, 8)) {
                const uint8_t *q = (const uint8_t *)(uintptr_t)target;
                for (int k = 0; k + 7 < 96; k++) {
                    if ((q[k] != 0x48 && q[k] != 0x4C) || q[k + 1] != 0x8D ||
                        (q[k + 2] & 0xC7) != 0x05)
                        continue;
                    int32_t d = (int32_t)((uint32_t)q[k + 3] |
                                          ((uint32_t)q[k + 4] << 8) |
                                          ((uint32_t)q[k + 5] << 16) |
                                          ((uint32_t)q[k + 6] << 24));
                    uint64_t sva = target + (uint64_t)k + 7 +
                                   (uint64_t)(int64_t)d;
                    if (!user_va_range_mapped(sva, 1)) continue;
                    const char *s = (const char *)(uintptr_t)sva;
                    int sl = 0;
                    while (sl < 60 && user_va_range_mapped(sva + (uint64_t)sl, 1) &&
                           s[sl] >= 0x20 && s[sl] <= 0x7E)
                        sl++;
                    if (sl >= 6) {
                        console_puts("  <- \"");
                        for (int m = 0; m < sl; m++) console_putchar(s[m]);
                        console_puts("\"");
                        break;
                    }
                }
            }
        }
        console_putchar('\n');
    }
    uint64_t cur = rip;
    for (int i = 0; i < 4; i++) {
        user_insn_t in;
        int len = decode_user_insn(cur, &in);
        if (!len) break;
        console_puts("  code ");
        print_hex64(cur);
        console_puts(":   ");
        console_puts(in.text);
        console_putchar('\n');
        cur += (uint64_t)len;
    }
}

static void dump_user_backtrace(uint64_t rip, uint64_t rbp)
{
    console_puts("  bt[0] rip=0x");
    print_hex64(rip);
    console_putchar('\n');
    /* 打印这个返回地址落在哪个 VMA 里。
     *
     * 起因：Chromium 子进程撞 `ud2` 时，bt[1..] 打出来是
     *   0x1108C4AD4D / 0x1108C2898E / 0x1108C2986F —— 这几个数值**全是偶数**
     *   的玩具级概率不可能，说明它们根本不是真实返回地址，而是栈上残留的
     *   数据被 RBP 链串起来了（Chromium 主体用 -fomit-frame-pointer，
     *   RBP 是普通局部变量）。光看数值只能猜，带上 VMA 就一目了然：
     *   真实代码地址必然落在某个可执行 VMA 内，落在 backing_type 为
     *   文件/匿名且 flags 无 X 的区间里的就是栈垃圾。 */
    uint64_t frame = rbp;
    for (int depth = 1; depth <= 16; depth++) {
        if (!user_va_range_mapped(frame, 16)) break;
        uint64_t next_rbp = *(volatile uint64_t *)(uintptr_t)frame;
        uint64_t return_address =
            *(volatile uint64_t *)(uintptr_t)(frame + 8);
        if (!return_address) break;
        console_puts("  bt[");
        print_dec(depth);
        console_puts("] ret=0x");
        print_hex64(return_address);
        /* 归属区间：在 mm/areas 里找包含它的那一段，打 "+段内偏移"。
         * 同一段内的连续几个返回地址就属于同一个 ELF 映像，可以直接按
         * （rip - 段基址）去二进制里反查。 */
        task_t *self = task_current();
        if (self && self->mm) {
            for (vm_area_t *area = self->mm->areas; area; area = area->next) {
                if (return_address >= area->start &&
                    return_address < area->end) {
                    console_puts(" @");
                    print_hex64(area->start);
                    console_puts("+");
                    print_hex64(return_address - area->start);
                    console_puts(" bt=");
                    print_dec((int)area->backing_type);
                    console_puts(" fl=");
                    print_hex64(vmm_get_page_flags(
                        return_address & ~(uint64_t)(PAGE_SIZE - 1)));
                    break;
                }
            }
        }
        console_putchar('\n');
        if (next_rbp <= frame) break;     /* 栈必须向高地址增长 */
        frame = next_rbp;
    }
}

// ============================================================
// Global Descriptor Table (GDT)
// ============================================================

// 64-bit long mode GDT entries (8 bytes each)
// GDT layout:
//   0: NULL
//   1: KCODE (64-bit ring0 code)
//   2: KDATA (64-bit ring0 data)
//   3: UCODE (64-bit ring3 code)
//   4: UDATA (64-bit ring3 data)
//   5: TSS low (low 64 bits of TSS descriptor)
//   6: TSS high (high 64 bits of TSS descriptor)

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) seg_desc_t;

static seg_desc_t gdt_entries[GDT_ENTRIES];
static tss_t      g_tss[GDT_MAX_CPUS];

static void tss_desc_fill(seg_desc_t *low, seg_desc_t *high, uintptr_t base, uint32_t limit) {
    low->limit_low  = (uint16_t)(limit & 0xFFFF);
    low->base_low   = (uint16_t)(base & 0xFFFF);
    low->base_mid   = (uint8_t)((base >> 16) & 0xFF);
    low->access     = GDT_ACC_PRESENT | GDT_ACC_TSS;
    low->granularity = (uint8_t)((limit >> 16) & 0x0F);
    low->base_high  = (uint8_t)((base >> 24) & 0xFF);

    high->limit_low = (uint16_t)((base >> 32) & 0xFFFF);
    high->base_low  = (uint16_t)((base >> 48) & 0xFFFF);
    high->base_mid  = high->access = high->granularity = high->base_high = 0;
}

extern void gdt_flush(uint64_t gdt_ptr_addr);
extern void tss_flush(uint16_t sel);

void gdt_init(void) {
    // NULL descriptor
    gdt_entries[GDT_NULL].access = 0;

    // KCODE: ring0 64-bit code
    gdt_entries[GDT_KCODE].limit_low   = 0;
    gdt_entries[GDT_KCODE].base_low    = 0;
    gdt_entries[GDT_KCODE].base_mid    = 0;
    gdt_entries[GDT_KCODE].access      = GDT_ACC_PRESENT | GDT_ACC_DPL0 | GDT_ACC_CODE;
    gdt_entries[GDT_KCODE].granularity = GDT_GRAN_4K | GDT_GRAN_64;
    gdt_entries[GDT_KCODE].base_high   = 0;

    // KDATA: ring0 data
    gdt_entries[GDT_KDATA].limit_low   = 0;
    gdt_entries[GDT_KDATA].base_low    = 0;
    gdt_entries[GDT_KDATA].base_mid    = 0;
    gdt_entries[GDT_KDATA].access      = GDT_ACC_PRESENT | GDT_ACC_DPL0 | GDT_ACC_DATA;
    gdt_entries[GDT_KDATA].granularity = GDT_GRAN_4K;
    gdt_entries[GDT_KDATA].base_high   = 0;

    // UCODE: ring3 64-bit code
    gdt_entries[GDT_UCODE].limit_low   = 0;
    gdt_entries[GDT_UCODE].base_low    = 0;
    gdt_entries[GDT_UCODE].base_mid    = 0;
    gdt_entries[GDT_UCODE].access      = GDT_ACC_PRESENT | GDT_ACC_DPL3 | GDT_ACC_CODE;
    gdt_entries[GDT_UCODE].granularity = GDT_GRAN_4K | GDT_GRAN_64;
    gdt_entries[GDT_UCODE].base_high   = 0;

    // UDATA: ring3 data
    gdt_entries[GDT_UDATA].limit_low   = 0;
    gdt_entries[GDT_UDATA].base_low    = 0;
    gdt_entries[GDT_UDATA].base_mid    = 0;
    gdt_entries[GDT_UDATA].access      = GDT_ACC_PRESENT | GDT_ACC_DPL3 | GDT_ACC_DATA;
    gdt_entries[GDT_UDATA].granularity = GDT_GRAN_4K;
    gdt_entries[GDT_UDATA].base_high   = 0;

    // TSS descriptors — one per possible CPU core, see GDT_TSS_LOW(cpu)'s
    // comment in cpu.h for why this isn't a single shared TSS.
    for (int i = 0; i < GDT_MAX_CPUS; i++) {
        tss_desc_fill(&gdt_entries[GDT_TSS_LOW(i)], &gdt_entries[GDT_TSS_HIGH(i)],
                      (uintptr_t)&g_tss[i], sizeof(tss_t) - 1);
        g_tss[i].rsp0 = 0; // will be set by tss_set_stack (BSP) / gdt_idt_load_ap (APs)
    }

    // Flush GDT and load the BSP's own TSS (slot 0)
    gdt_ptr_t gdt_ptr;
    gdt_ptr.limit = sizeof(gdt_entries) - 1;
    gdt_ptr.base  = (uint64_t)gdt_entries;
    gdt_flush((uint64_t)&gdt_ptr);
    tss_flush(SEL_TSS(0));
}

void tss_set_stack(uint64_t rsp0) {
    g_tss[0].rsp0 = rsp0;
    extern uint64_t linux_syscall_kernel_rsp;
    linux_syscall_kernel_rsp = rsp0;
}

void tss_set_stack_cpu(int cpu, uint64_t rsp0) {
    if (cpu < 0 || cpu >= GDT_MAX_CPUS) cpu = 0;
    g_tss[cpu].rsp0 = rsp0;
    /* SYSCALL 的内核栈仍只在 BSP 上维护一份；AP 上的用户态任务走 int 路径。 */
    if (cpu == 0) {
        extern uint64_t linux_syscall_kernel_rsp;
        linux_syscall_kernel_rsp = rsp0;
    }
}

// ============================================================
// Interrupt Descriptor Table (IDT) + PIC
// ============================================================

// PIC I/O ports
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_ICW4  0x01
#define ICW1_INIT  0x10

static idt_entry_t idt_entries[256];

// Stub address tables defined in interrupt_asm.asm
extern uint64_t isr_stub_table[32];
/* 单步探针开关，定义在 src/core/task_switch.asm。 */
extern int task_debug_single_step;
extern uint64_t irq_stub_table[16];
extern void syscall_int80_stub(void);
extern void linux_syscall_entry(void);

static void idt_set_entry(int vec, uint64_t handler, uint8_t flags) {
    idt_entries[vec].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt_entries[vec].segment     = SEL_KCODE;
    idt_entries[vec].ist         = 0;
    idt_entries[vec].flags       = flags;
    idt_entries[vec].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt_entries[vec].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt_entries[vec].reserved    = 0;
}

// Remap PIC IRQ0-15 → vectors 32-47
static void pic_remap(void) {
    uint8_t a1 = io_in8(PIC1_DATA);
    uint8_t a2 = io_in8(PIC2_DATA);

    io_out8(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_delay();
    io_out8(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_delay();
    io_out8(PIC1_DATA, IRQ_BASE);              io_delay(); // Master offset
    io_out8(PIC2_DATA, IRQ_BASE + 8);          io_delay(); // Slave offset
    io_out8(PIC1_DATA, 4);                     io_delay(); // Slave at IRQ2
    io_out8(PIC2_DATA, 2);                     io_delay(); // Cascade identity
    io_out8(PIC1_DATA, 0x01);                  io_delay(); // 8086 mode
    io_out8(PIC2_DATA, 0x01);                  io_delay();

    io_out8(PIC1_DATA, a1); // Restore masks
    io_out8(PIC2_DATA, a2);
}

static void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) io_out8(PIC2_CMD, 0x20);
    io_out8(PIC1_CMD, 0x20);
}

// ============================================================
// Exception name table
// ============================================================
static const char *exc_names[32] = {
    "Divide-by-zero",
    "Debug",
    "Non-maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 FPU Floating-Point Error",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved",
};

// ============================================================
// isr_handler — called from assembly common stub
// ============================================================
void isr_handler(isr_regs_t *regs) {
    uint8_t vec = (uint8_t)regs->int_no;

    // Forward IRQs to irq_handler
    if (vec >= IRQ_BASE) return;

    // CPU exception
    const char *name = (vec < 32) ? exc_names[vec] : "Unknown";
    uint64_t cr2_val = (vec == EXC_PF) ? read_cr2() : 0;

    /* 单步探针（排错用，见 task_switch.asm 的 task_debug_single_step）。
     * clone 出来的子进程打了 TF 之后，执行完第一条用户指令就会到这里，
     * regs->rip 正是"刚执行完的那条"的下一条 —— 由此可以确认子进程到底
     * 有没有真正进用户态，以及卡在哪条指令之后。
     *
     * 这里清掉 TF 放行，不当成信号处理（否则子进程会收到 SIGTRAP 直接
     * 退出，反而看不出问题）。 */
    if (vec == EXC_DB && (regs->cs & 3) == 3 &&
        task_debug_single_step) {
        /* 连续单步：把 TF 留着继续追，直到步数用完或撞上 syscall。
         * 只打前若干条（trace 上限），避免把串口打爆。 */
        static int step_trace_left = 24;
        if (step_trace_left > 0) {
            console_puts("[STEP] rip=0x");
            print_hex64(regs->rip);
            console_puts(" rax=0x");
            print_hex64(regs->rax);
            console_puts(" rdi=0x");
            print_hex64(regs->rdi);
            console_puts(" rbx=0x");
            print_hex64(regs->rbx);
            console_putchar('\n');
            step_trace_left--;
            return;
        }
        regs->rflags &= ~0x100ULL;
        task_debug_single_step = 0;
        step_trace_left = 24;
        console_puts("[STEP] trace limit reached, stop\n");
        return;
    }

    /* A present user write fault on a writable-before-fork COW mapping is
     * resolved in-kernel.  This stays allocation-free when the page has
     * become uniquely owned; otherwise only one 4 KiB page is copied. */
    if (vec == EXC_PF && (regs->cs & 3) == 3 &&
        (regs->err_code & (PF_P | PF_W | PF_U)) == (PF_P | PF_W | PF_U)) {
        int cow_result = vmm_handle_cow_fault(cr2_val);
        /* fork 出来的子进程如果"进了用户态却一条 syscall 都不发"，最常见
         * 的原因就是它一落地先写 COW 页，而这条故障没被解析成功。这条
         * 日志把每一次用户态写故障的裁决结果打出来：>0 已解析、0 不是
         * COW 页、<0 解析失败。只在真正出错时才有一行，量很小。 */
        if (cow_result <= 0) {
            console_puts("[PF] user write addr=0x");
            print_hex64(cr2_val);
            console_puts(" rip=0x");
            print_hex64(regs->rip);
            console_puts(" err=0x");
            print_hex64(regs->err_code);
            console_puts(" cow=");
            print_hex64((uint64_t)(int64_t)cow_result);
            console_puts(" pte=0x");
            print_hex64(vmm_get_page_flags(cr2_val & ~0xfffULL));
            console_putchar('\n');
            uint64_t cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            task_t *fault_task = task_current();
            console_puts("  [PF-WALK] hw="); print_hex64(cr3);
            console_puts(" sw="); print_hex64(vmm_get_pml4());
            console_puts(" task=");
            print_hex64(fault_task && fault_task->mm ?
                        fault_task->mm->pml4_phys : 0);
            console_puts(" flags="); print_hex64(regs->rflags);
            const uint64_t mask = 0x000FFFFFFFFFF000ULL;
            uint64_t table = cr3 & mask;
            for (int shift = 39; shift >= 12 && table; shift -= 9) {
                uint64_t entry = ((const uint64_t *)(uintptr_t)table)
                    [(cr2_val >> shift) & 511];
                console_putchar(' '); print_hex64(entry);
                if (!(entry & VMM_P) || (shift > 12 && (entry & VMM_PS)))
                    break;
                table = entry & mask;
            }
            console_putchar('\n');
        }
        if (cow_result > 0) return;

        /* 页表本身已允许这次写（present + W + U），CPU 却仍报写故障——
         * 只剩"陈旧 TLB 项"一种解释：某处把页从只读改成可写时没有刷新
         * 该地址的转换。实测 Chromium 多进程里
         * PartitionBucket::SlowPathAlloc+0x7cf 就撞这个（打印的 pte 是
         * 0x...267，P/W/U 全在）。
         *
         * 这里做有界自愈：刷新该页 TLB 后重试，同一地址最多 16 次，
         * 超过就照常按 SIGSEGV 处理（避免真正的权限问题变成死循环）。 */
        uint64_t write_flags = vmm_get_page_flags(cr2_val & ~0xfffULL);
        if ((write_flags & VMM_W) && (write_flags & VMM_U)) {
            static uint64_t pf_retry_addr;
            static uint32_t pf_retry_count;
            uint64_t page_addr = cr2_val & ~0xfffULL;
            if (page_addr == pf_retry_addr) {
                pf_retry_count++;
            } else {
                pf_retry_addr = page_addr;
                pf_retry_count = 1;
            }
            if (pf_retry_count <= 16) {
                __asm__ volatile("invlpg (%0)" :: "r"((void *)(uintptr_t)cr2_val)
                                 : "memory");
                uint64_t retry_cr3;
                __asm__ volatile("mov %%cr3, %0" : "=r"(retry_cr3));
                __asm__ volatile("mov %0, %%cr3" :: "r"(retry_cr3) : "memory");
                console_puts("[PF] stale-TLB retry #");
                print_hex64(pf_retry_count);
                console_puts(" addr=0x");
                print_hex64(cr2_val);
                console_putchar('\n');
                return;
            }
        }
    }

    /* User protection/not-present faults are synchronous SIGSEGV, not
     * kernel failures.  A registered SA_RESTORER handler may repair the
     * mapping and return to retry the exact faulting instruction. */
    if (vec == EXC_PF && (regs->cs & 3) == 3) {
        if (linux_signal_prepare_exception(regs, SIGSEGV, cr2_val,
                                           regs->err_code) > 0)
            return;
        task_t *task = task_current();
        if (task) task->sig_exit_code = SIGSEGV;
        task_set_exit_status(128 + SIGSEGV);
        /* 未注册处理器 = 进程真死了：留下 fault 地址 + 回溯，否则只剩
         * 一行 "run: exit 139"，无从判断崩在哪个调用链。 */
        console_puts("\n[TRAP] user SIGSEGV addr=0x");
        print_hex64(cr2_val);
        console_puts(" rip=0x");
        print_hex64(regs->rip);
        console_putchar('\n');
        /* SIGSEGV 往往发生在 -O2 编译的库里，调用者没建帧指针——只靠
         * rbp 链会只打出 bt[0] 就断尾。这里补上"故障现场寄存器"（能直接
         * 定位 load/store 的源/目的）和最近系统调用轨迹，和 #BP 路径对齐。 */
        dump_user_trap_regs(regs);
        dump_user_vma_for(regs->rip);
        dump_user_canary(regs->rbp);
        linux_open_trace_dump();
        dump_user_tail(regs->rip);
        dump_user_backtrace(regs->rip, regs->rbp);
        dump_user_stack_window(regs->rsp);
        /* syscall 环形 dump 每次崩溃都重打几百行。QEMU 的串口是逐字符
         * 模拟的，这块能占掉整个运行一半以上的时间。默认关闭，
         * `setenv syscdump 1` 打开。内核 panic 那处仍然无条件打印。 */
        if (linux_debug_syscall_dump) linux_syscall_trace_dump();
        task_exit();
    }

    /* User-mode synchronous traps must kill only the offending task.
     *
     * Chromium 的 CHECK()/IMMEDIATE_CRASH() 走 `int3`（跟 ud2 配对），
     * 而 int3 要求 IDT 门的 DPL=3；以前 vector 3 是 DPL0，于是 ring3 的
     * int3 被 CPU 升级成 #GP(错误码 0x1A = IDT[3] 门不可用)，整个内核
     * panic——"用户程序一个断言失败，整机就得重启"。除了补齐 DPL，
     * 这里把 #BP/#UD/#GP 等同步异常统一按"进程异常终止"处理，串口留下
     * RIP，方便继续排查。 */
    if ((regs->cs & 3) == 3 &&
        (vec == EXC_BP || vec == EXC_UD || vec == EXC_GP ||
         vec == EXC_DE || vec == EXC_AC || vec == EXC_MF)) {
        int signal_number = (vec == EXC_BP) ? SIGTRAP : SIGILL;
        console_puts("\n[TRAP] user ");
        console_puts(name);
        console_puts(" rip=0x");
        print_hex64(regs->rip);
        console_puts(" err=");
        print_hex64(regs->err_code);
        console_puts(" -> task terminated\n");
        {
            task_t *current = task_current();
            console_puts("  fs_base=0x");
            print_hex64(current ? current->fs_base : 0);
            console_puts(" tls_marker=");
            print_hex64((uint64_t)current);
            console_putchar('\n');
        }
        dump_user_trap_regs(regs);
        dump_user_canary(regs->rbp);
        /* 最近 16 次 open/openat 的路径与结果。Chromium 的
         * base::File::Open 失败会走 CHECK() 直接 int3，只看断点地址
         * 不知道是哪个文件没打开；这一段能直接把崩溃翻译成路径。 */
        linux_open_trace_dump();
        /* 打印 rip 处的指令字节：GP/UD 只看 rip 查不出原因 —— 得知道到底是
         * 哪条指令非法（rdmsr？特权指令？SIMD？还是取指越界）。Chromium
         * 每次 exec 子进程都在 ld.so 同一个地址上 GP Fault，没有这条只能猜。 */
        if (vec == EXC_GP || vec == EXC_UD) {
            uint64_t rip_page = regs->rip & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t phys = vmm_get_phys(rip_page);
            if (phys) {
                const uint8_t *code =
                    (const uint8_t *)(uintptr_t)
                    (phys + (regs->rip - rip_page));
                console_puts("  insn:");
                for (int i = 0; i < 16; i++) {
                    uint8_t b = code[i];
                    const char *hex = "0123456789ABCDEF";
                    console_putchar(' ');
                    console_putchar(hex[(b >> 4) & 0xF]);
                    console_putchar(hex[b & 0xF]);
                }
                console_putchar('\n');
            }
        }
        dump_user_vma_for(regs->rip);
        dump_user_tail(regs->rip);
        dump_user_backtrace(regs->rip, regs->rbp);
        dump_user_stack_window(regs->rsp);
        if (linux_debug_syscall_dump) linux_syscall_trace_dump();
        if (linux_signal_prepare_exception(regs, signal_number, regs->rip,
                                           regs->err_code) > 0)
            return;
        task_t *task = task_current();
        if (task) task->sig_exit_code = signal_number;
        task_set_exit_status(128 + signal_number);
        task_exit();
    }

    console_puts("\n\x1b[31m===== KERNEL PANIC =====\x1b[0m\n");
    /* 先走串口的应急通道：这一份不会被其他核的 console 输出搅乱，
     * 也不会因为 console 内部状态坏了而丢掉关键信息。 */
    smp_panic_puts("\n[PANIC] cpu=");
    { char c[2] = { (char)('0' + (smp_cpu_index() % 10)), 0 }; smp_panic_puts(c); }
    smp_panic_puts(" vec=");
    smp_panic_hex(vec);
    smp_panic_puts(" err=");
    smp_panic_hex(regs->err_code);
    smp_panic_puts(" rip=");
    smp_panic_hex(regs->rip);
    smp_panic_puts(" rsp=");
    smp_panic_hex(regs->rsp);
    smp_panic_puts(" cr2=");
    smp_panic_hex(cr2_val);
    smp_panic_puts("\n");
    /* 内核态崩溃也要带上"最后做了什么"：多线程并发后才出现的崩溃
     * 单线程复现不了，只有最近系统调用轨迹能指认是哪条路径。 */
    linux_syscall_trace_dump();
    console_puts("Exception: ");
    console_puts(name);
    console_puts(" (cpu=");
    console_putchar((char)('0' + (smp_cpu_index() % 10)));
    console_puts(", err=");
    {
        uint64_t e = regs->err_code;
        int started = 0;
        for (int s = 15; s >= 0; s--) {
            int digit = (int)((e >> (s * 4)) & 0xF);
            if (digit || started || s == 0) {
                started = 1;
                console_putchar(digit < 10 ? (char)('0' + digit)
                                            : (char)('A' + digit - 10));
            }
        }
    }
    console_puts(", rip=");
    {
        uint64_t r = regs->rip;
        int started = 0;
        for (int s = 15; s >= 0; s--) {
            int digit = (int)((r >> (s * 4)) & 0xF);
            if (digit || started || s == 0) {
                started = 1;
                console_putchar(digit < 10 ? (char)('0' + digit)
                                            : (char)('A' + digit - 10));
            }
        }
    }
    console_puts(")");
    console_puts(" (vector 0x");
    {
        int started = 0;
        for (int s = 7; s >= 0; s--) {
            int digit = (vec >> (s*4)) & 0xF;
            if (digit || started || s == 0) {
                started = 1;
                console_putchar(digit < 10 ? '0' + digit : 'A' + digit - 10);
            }
        }
    }
    console_puts(")\n");

    /* 内核栈扫描：打印栈上落在内核镜像（链接基址 16 MiB）里的 8 字节值，
     * 基本都是返回地址。宿主侧 objdump 一查就是完整调用链——空函数指针
     * (RIP=0x3) 这种"崩在无名地址"的场景只有靠它定位。 */
    if (vec == EXC_UD || vec == EXC_GP || vec == EXC_DE) {
        console_puts("kernel stack scan:\n");
        for (uint64_t sp = regs->rsp & ~7ULL;
             sp < (regs->rsp & ~7ULL) + 0x400; sp += 8) {
            uint64_t v = *(volatile uint64_t *)sp;
            if (v >= 0x1000000ULL && v < 0x1200000ULL) {
                console_puts("  [0x");
                print_hex64(sp);
                console_puts("]=0x");
                print_hex64(v);
                console_putchar('\n');
            }
        }
    }

    if (regs->err_code != 0) {
        console_puts("Error code: 0x");
        uint64_t ec = regs->err_code;
        int started = 0;
        for (int s = 15; s >= 0; s--) {
            int digit = (ec >> (s*4)) & 0xF;
            if (digit || started || s == 0) {
                started = 1;
                console_putchar(digit < 10 ? '0' + digit : 'A' + digit - 10);
            }
        }
        console_putchar('\n');
    }

    if (vec == EXC_PF) {
        console_puts("CR2 (fault address): 0x");
        uint64_t cr2 = cr2_val;
        int started = 0;
        for (int s = 15; s >= 0; s--) {
            int digit = (cr2 >> (s*4)) & 0xF;
            if (digit || started || s == 0) {
                started = 1;
                console_putchar(digit < 10 ? '0' + digit : 'A' + digit - 10);
            }
        }
        console_puts("\n  Flags: ");
        if (regs->err_code & PF_P)    console_puts("P ");
        if (regs->err_code & PF_W)    console_puts("W ");
        if (regs->err_code & PF_U)    console_puts("U ");
        if (regs->err_code & PF_RSVD) console_puts("RSVD ");
        if (regs->err_code & PF_I)    console_puts("I ");
        console_putchar('\n');
    }

    console_puts("\n\x1b[36m--- Register dump ---\x1b[0m\n");
    {
        task_t *current = task_current();
        console_puts("current task: id=");
        print_hex64(current ? current->id : 0);
        console_puts(" name=");
        console_puts(current ? current->name : "?");
        console_puts(" state=");
        print_hex64(current ? (uint64_t)current->state : 0);
        console_puts(" ksp=0x");
        print_hex64(current ? current->rsp : 0);
        console_putchar('\n');
        /* 原始栈转储：上面的"文本地址扫描"空手而归，说明 RSP 本身就是
         * 坏的（指向数据而不是栈）。把 RSP 附近的原始内存打出来，直接
         * 认出它是哪个结构体。 */
        console_puts("raw stack @[rsp-0x40, rsp+0x40):");
        uint64_t base = regs->rsp & ~7ULL;
        for (int64_t off = -0x40; off < 0x40; off += 8) {
            if ((off & 0x1F) == 0) console_puts("\n  ");
            uint64_t v = *(volatile uint64_t *)(base + off);
            print_hex64(v);
            console_putchar(' ');
        }
        console_putchar('\n');
    }
    console_puts("RAX=0x"); print_hex64(regs->rax);   console_puts(" RBX=0x"); print_hex64(regs->rbx); console_putchar('\n');
    console_puts("RCX=0x"); print_hex64(regs->rcx);   console_puts(" RDX=0x"); print_hex64(regs->rdx); console_putchar('\n');
    console_puts("RSI=0x"); print_hex64(regs->rsi);   console_puts(" RDI=0x"); print_hex64(regs->rdi); console_putchar('\n');
    console_puts("RBP=0x"); print_hex64(regs->rbp);   console_puts(" RSP=0x"); print_hex64(regs->rsp); console_putchar('\n');
    console_puts("RIP=0x"); print_hex64(regs->rip);   console_puts(" RFL=0x"); print_hex64(regs->rflags); console_putchar('\n');
    console_puts(" CS=0x"); print_hex16(regs->cs);    console_puts("  SS=0x"); print_hex16(regs->ss);  console_putchar('\n');

    console_puts("\n\x1b[31mSystem halted.\x1b[0m\n");
    cli();
    while (1) hlt();
}

// ============================================================
// irq_handler — called from assembly common stub
// ============================================================
static volatile uint64_t g_pit_ticks = 0;

uint64_t pit_get_ticks(void) {
    return g_pit_ticks;
}

void irq_handler(isr_regs_t *regs) {
    /* SMP：AP 的 LAPIC 定时器。它不走 8259，所以必须在这里分出去，
     * 否则会落到下面按 PIC 的 irq 号发 EOI，等于往串口/键盘的端口上
     * 乱写。 */
    if (regs->int_no == IRQ_SMP_TIMER) {
        smp_lapic_eoi();
        extern void task_schedule(void);
        task_schedule();
        return;
    }

    uint8_t irq = (uint8_t)(regs->int_no - IRQ_BASE);

    if (irq == 7 || irq == 15) {
        pic_send_eoi(irq);
        return;
    }

    if (irq == 0) {
        pic_send_eoi(0);
        g_pit_ticks++;
        extern void task_schedule(void);
        task_schedule();
        return;
    }

    if (irq == 1) {
        if (io_in8(0x64) & 1) {
            uint8_t st = io_in8(0x64);
            if (!(st & 0x20)) {
                uint8_t sc = io_in8(0x60);
                extern void kb_irq_enqueue_scancode(uint8_t sc);
                kb_irq_enqueue_scancode(sc);
            } else {
                uint8_t b = io_in8(0x60);
                extern void ps2_mouse_enqueue_byte(uint8_t b);
                ps2_mouse_enqueue_byte(b);
            }
        }
        pic_send_eoi(1);
        return;
    }

    if (irq == 12) {
        if (io_in8(0x64) & 1) {
            uint8_t st = io_in8(0x64);
            if (st & 0x20) {
                uint8_t b = io_in8(0x60);
                extern void ps2_mouse_enqueue_byte(uint8_t b);
                ps2_mouse_enqueue_byte(b);
            } else {
                uint8_t sc = io_in8(0x60);
                extern void kb_irq_enqueue_scancode(uint8_t sc);
                kb_irq_enqueue_scancode(sc);
            }
        }
        pic_send_eoi(12);
        return;
    }

    pic_send_eoi(irq);
}

// ============================================================
// Interrupt init — GDT + IDT + PIC
// ============================================================
void gdt_idt_init(void) {
    gdt_init();

    // Clear IDT and set all entry handlers as stub 0x00 (NULL handler → triple fault)
    // Not strictly needed since zeroed memory is the default
    for (int i = 0; i < 256; i++) {
        idt_entries[i].offset_low  = 0;
        idt_entries[i].segment     = 0;
        idt_entries[i].ist         = 0;
        idt_entries[i].flags       = 0;
        idt_entries[i].offset_mid  = 0;
        idt_entries[i].offset_high = 0;
        idt_entries[i].reserved    = 0;
    }

    // Register 32 CPU exception handlers
    for (int i = 0; i < 32; i++) {
        uint64_t addr = isr_stub_table[i];
        idt_set_entry(i, addr, IDT_PRESENT | IDT_DPL0 | IDT_INT_GATE);
    }
    // Page fault as trap gate (preserves interrupt flag)
    idt_set_entry(EXC_PF, isr_stub_table[EXC_PF], IDT_PRESENT | IDT_DPL0 | IDT_TRAP_GATE);
    /* #BP 必须允许 ring3 直接触发（int3 从用户态进内核），否则会被
     * CPU 当成 IDT 门权限不足 → #GP。Chromium/glibc 的断言都依赖它。 */
    idt_set_entry(EXC_BP, isr_stub_table[EXC_BP], IDT_PRESENT | IDT_DPL3 | IDT_INT_GATE);

    // Remap PIC and register IRQ handlers
    pic_remap();

    for (int i = 0; i < 16; i++) {
        uint64_t addr = irq_stub_table[i];
        idt_set_entry(IRQ_BASE + i, addr, IDT_PRESENT | IDT_DPL0 | IDT_INT_GATE);
    }

    /* SMP：AP 的 LAPIC 定时器向量（见 smp.h 的 IRQ_SMP_TIMER） */
    extern void smp_timer_stub(void);
    idt_set_entry(IRQ_SMP_TIMER, (uint64_t)smp_timer_stub,
                  IDT_PRESENT | IDT_DPL0 | IDT_INT_GATE);

    idt_set_entry(HBOS_SYSCALL_VECTOR, (uint64_t)syscall_int80_stub,
                  IDT_PRESENT | IDT_DPL3 | IDT_TRAP_GATE);

    /* Linux ELF binaries enter through the architectural SYSCALL path.
     * The assembly return uses iretq, so STAR's user-selector half is not
     * constrained by SYSRET's descriptor ordering. */
    wrmsr(MSR_STAR, (uint64_t)SEL_KCODE << 32);
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)linux_syscall_entry);
    wrmsr(MSR_SFMASK, (1ULL << 8) | (1ULL << 9) | (1ULL << 10));
    uint32_t max_extended, unused_b, unused_c, extended_features;
    __asm__ volatile("cpuid"
                     : "=a"(max_extended), "=b"(unused_b),
                       "=c"(unused_c), "=d"(extended_features)
                     : "a"(0x80000000U), "c"(0));
    bool nx_supported = false;
    if (max_extended >= 0x80000001U) {
        __asm__ volatile("cpuid"
                         : "=a"(max_extended), "=b"(unused_b),
                           "=c"(unused_c), "=d"(extended_features)
                         : "a"(0x80000001U), "c"(0));
        nx_supported = (extended_features & (1U << 20)) != 0;
    }
    uint64_t efer = rdmsr(MSR_EFER) | EFER_SCE;
    if (nx_supported) efer |= EFER_NXE;
    wrmsr(MSR_EFER, efer);
    vmm_set_nx_enabled(nx_supported);

    // Load IDT
    idt_ptr_t idt_ptr;
    idt_ptr.limit = sizeof(idt_entries) - 1;
    idt_ptr.base  = (uint64_t)idt_entries;
    __asm__ volatile("lidt (%0)" : : "r"(&idt_ptr) : "memory");
}

/* AP 核心跳板（smp_trampoline.asm）目前只搭了一份没有 TSS 描述符的临时
 * GDT 用来进长模式，也从没加载过 IDT——ap_entry() 以前直接上线就
 * sti;hlt 空转，凑巧没暴露问题，但只要这个核心真收到一次中断/异常就会
 * 直接三重故障。这里补上：加载 gdt_idt_init() 已经建好的共享 GDT/IDT
 * （表本身是只读共用的，不重建），并把这个核心自己的 TSS 描述符指向
 * 独立的 rsp0。 */
void gdt_idt_load_ap(int cpu_idx, uint64_t rsp0) {
    gdt_ptr_t gdt_ptr;
    gdt_ptr.limit = sizeof(gdt_entries) - 1;
    gdt_ptr.base  = (uint64_t)gdt_entries;
    gdt_flush((uint64_t)&gdt_ptr);

    if (cpu_idx >= 0 && cpu_idx < GDT_MAX_CPUS) {
        g_tss[cpu_idx].rsp0 = rsp0;
        tss_flush(SEL_TSS(cpu_idx));
    }

    idt_ptr_t idt_ptr2;
    idt_ptr2.limit = sizeof(idt_entries) - 1;
    idt_ptr2.base  = (uint64_t)idt_entries;
    __asm__ volatile("lidt (%0)" : : "r"(&idt_ptr2) : "memory");
}

// ============================================================
// Interrupt management API
// ============================================================
void int_enable(void)  { sti(); }
void int_disable(void) { cli(); }
bool int_get_state(void) {
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    return (rflags & 0x200) != 0;
}
