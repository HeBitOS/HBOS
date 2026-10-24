#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "cpu.h"
#include "io.h"
#include "vmm.h"
#include "task.h"
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
    char buf[8];
    int n = 0;
    if (value < 0) { console_putchar('-'); value = -value; }
    do { buf[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n--) console_putchar(buf[n]);
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

static void dump_user_backtrace(uint64_t rip, uint64_t rbp) {
    console_puts("  bt[0] rip=0x");
    print_hex64(rip);
    console_putchar('\n');
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

    /* A present user write fault on a writable-before-fork COW mapping is
     * resolved in-kernel.  This stays allocation-free when the page has
     * become uniquely owned; otherwise only one 4 KiB page is copied. */
    if (vec == EXC_PF && (regs->cs & 3) == 3 &&
        (regs->err_code & (PF_P | PF_W | PF_U)) == (PF_P | PF_W | PF_U)) {
        int cow_result = vmm_handle_cow_fault(cr2_val);
        if (cow_result > 0) return;
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
        dump_user_backtrace(regs->rip, regs->rbp);
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
        console_puts(" err=0x");
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
        dump_user_backtrace(regs->rip, regs->rbp);
        linux_syscall_trace_dump();
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
