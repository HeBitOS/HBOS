/**
 * @file    cpu.h
 * @brief   x86_64 CPU 架构定义 — GDT, IDT, TSS, MSR, 控制寄存器
 *
 * 本文件定义了 HBOS 在 x86_64 long mode 下运行所需的所有 CPU 级
 * 数据结构和常量。包括:
 *   - GDT (全局描述符表) 条目格式和选择子
 *   - IDT (中断描述符表) 条目格式（64 位模式 16 字节条目）
 *   - TSS (任务状态段) 用于 ring0 栈切换
 *   - ISR 寄存器转储结构（中断处理程序参数）
 *   - CPU 异常向量号
 *   - 页错误错误码位定义
 *   - MSR 地址和 EFER 位定义
 *   - 内联汇编辅助函数（wrmsr, rdmsr, invlpg, cli, sti, hlt 等）
 */

#ifndef HBOS_CPU_H
#define HBOS_CPU_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// GDT (全局描述符表) — 8 字节条目
// ============================================================

/** GDT 条目（8 字节，与 Intel 手册格式一致） */
typedef struct {
    uint16_t limit_low;      /**< 段界限 [15:0] */
    uint16_t base_low;       /**< 基址 [15:0] */
    uint8_t  base_mid;       /**< 基址 [23:16] */
    uint8_t  access;         /**< 访问字节 (P|DPL|S|Type) */
    uint8_t  granularity;    /**< 粒度 (G|D/B|L|AVL|Limit[19:16]) */
    uint8_t  base_high;      /**< 基址 [31:24] */
} __attribute__((packed)) gdt_entry_t;

/** GDTR 寄存器格式（lgdt 指令使用） */
typedef struct {
    uint16_t limit;          /**< GDT 大小 - 1 */
    uint64_t base;           /**< GDT 基址（64 位线性地址） */
} __attribute__((packed)) gdt_ptr_t;

// ============================================================
// IDT (中断描述符表) — 64 位模式 16 字节条目
// ============================================================

/** IDT 条目（64 位模式，16 字节） */
typedef struct {
    uint16_t offset_low;     /**< 处理程序地址 [15:0] */
    uint16_t segment;        /**< 代码段选择子 */
    uint8_t  ist;            /**< 中断栈表索引 (0=不使用) */
    uint8_t  flags;          /**< P|DPL|0|Type (present, DPL, gate type) */
    uint16_t offset_mid;     /**< 处理程序地址 [31:16] */
    uint32_t offset_high;    /**< 处理程序地址 [63:32] */
    uint32_t reserved;       /**< 保留（必须为 0） */
} __attribute__((packed)) idt_entry_t;

/** IDTR 寄存器格式（lidt 指令使用） */
typedef struct {
    uint16_t limit;          /**< IDT 大小 - 1 */
    uint64_t base;           /**< IDT 基址 */
} __attribute__((packed)) idt_ptr_t;

// ============================================================
// TSS (任务状态段) — ring0 栈指针
// ============================================================

/**
 * 64 位 TSS 结构
 * 主要用于存储 ring0 栈指针（RSP0），当中断从 ring3→ring0
 * 发生时 CPU 自动从 TSS 加载 RSP。
 */
typedef struct {
    uint32_t reserved0;      /**< 保留 */
    uint64_t rsp0;           /**< ring0 栈指针 */
    uint64_t rsp1;           /**< ring1 栈指针（未使用） */
    uint64_t rsp2;           /**< ring2 栈指针（未使用） */
    uint64_t reserved1;      /**< 保留 */
    uint64_t ist[7];         /**< 中断栈表 (IST) 条目 */
    uint64_t reserved2;      /**< 保留 */
    uint16_t reserved3;      /**< 保留 */
    uint16_t iopb_offset;    /**< I/O 权限位图偏移 */
} __attribute__((packed)) tss_t;

// ============================================================
// ISR 寄存器转储 — 中断处理程序的参数
// ============================================================

/**
 * 中断/异常处理程序接收的寄存器快照
 * 由 interrupt_asm.asm 中的 isr_common/irq_common 在栈上构建
 * 字段顺序必须与汇编中的 push 顺序完全一致
 */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;  /**< 通用寄存器（被调用者保存） */
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;      /**< 通用寄存器（调用者保存） */
    uint64_t int_no;     /**< 中断向量号（由 stub 压入） */
    uint64_t err_code;   /**< 错误码（由 CPU 或 stub 压入） */
    uint64_t rip, cs, rflags, rsp, ss;               /**< IRET 帧（由 CPU 压入） */
} __attribute__((packed)) isr_regs_t;

// ============================================================
// GDT 选择子 — 索引 × 8 + RPL
// ============================================================

#define GDT_NULL     0   /**< NULL 描述符（必须为第一个条目） */
#define GDT_KCODE    1   /**< 内核代码段 (ring0, 64-bit) */
#define GDT_KDATA    2   /**< 内核数据段 (ring0) */
#define GDT_UCODE    3   /**< 用户代码段 (ring3, 64-bit) */
#define GDT_UDATA    4   /**< 用户数据段 (ring3) */
#define GDT_TSS_LOW  5   /**< TSS 描述符低 64 位 */
#define GDT_TSS_HIGH 6   /**< TSS 描述符高 64 位 */
#define GDT_ENTRIES  7   /**< GDT 条目总数 */

/** 段选择子 = 索引 × 8 | RPL */
#define SEL_KCODE  (GDT_KCODE * 8)        /**< 内核代码选择子 (RPL=0) */
#define SEL_KDATA  (GDT_KDATA * 8)        /**< 内核数据选择子 (RPL=0) */
#define SEL_UCODE  (GDT_UCODE * 8) | 3    /**< 用户代码选择子 (RPL=3) */
#define SEL_UDATA  (GDT_UDATA * 8) | 3    /**< 用户数据选择子 (RPL=3) */
#define SEL_TSS    (GDT_TSS_LOW * 8)      /**< TSS 选择子 */

// ============================================================
// GDT 访问字节位定义
// ============================================================

#define GDT_ACC_PRESENT    0x80   /**< P: 段存在 */
#define GDT_ACC_DPL0       0x00   /**< DPL=0 (ring0) */
#define GDT_ACC_DPL3       0x60   /**< DPL=3 (ring3) */
#define GDT_ACC_CODE       0x18   /**< 代码段: 可执行/可读 */
#define GDT_ACC_DATA       0x12   /**< 数据段: 可读写 */
#define GDT_ACC_TSS        0x09   /**< 可用 TSS (非忙) */

// ============================================================
// GDT 粒度字节位定义
// ============================================================

#define GDT_GRAN_4K     0x08       /**< G: 4KB 粒度 */
#define GDT_GRAN_64     0x20       /**< L: long mode (64 位代码段) */
#define GDT_GRAN_32     0x40       /**< D/B: 默认操作数大小 (32 位) */

// ============================================================
// IDT 标志位定义
// ============================================================

#define IDT_PRESENT     0x80       /**< P: 中断门存在 */
#define IDT_DPL0        0x00       /**< DPL=0 (仅 ring0 可触发) */
#define IDT_DPL3        0x60       /**< DPL=3 (ring3 可通过 int 触发) */
#define IDT_INT_GATE    0x0E       /**< 64 位中断门 (清除 IF) */
#define IDT_TRAP_GATE   0x0F       /**< 64 位陷阱门 (保留 IF) */

// ============================================================
// PIC 重映射 — IRQ0-15 → INT 32-47
// ============================================================

#define IRQ_BASE 0x20              /**< IRQ 基向量（PIC 重映射后） */
#define IRQ(n)   (IRQ_BASE + (n))  /**< 将 IRQ 号转换为中断向量 */

// ============================================================
// CPU 异常向量号
// ============================================================

#define EXC_DE  0   /**< #DE: 除法错误 */
#define EXC_DB  1   /**< #DB: 调试 */
#define EXC_NMI 2   /**< NMI: 不可屏蔽中断 */
#define EXC_BP  3   /**< #BP: 断点 */
#define EXC_OF  4   /**< #OF: 溢出 */
#define EXC_BR  5   /**< #BR: 超出界限 */
#define EXC_UD  6   /**< #UD: 无效操作码 */
#define EXC_NM  7   /**< #NM: 设备不可用 (x87 FPU) */
#define EXC_DF  8   /**< #DF: 双重错误 (有错误码) */
#define EXC_TS  10  /**< #TS: 无效 TSS (有错误码) */
#define EXC_NP  11  /**< #NP: 段不存在 (有错误码) */
#define EXC_SS  12  /**< #SS: 栈段错误 (有错误码) */
#define EXC_GP  13  /**< #GP: 通用保护错误 (有错误码) */
#define EXC_PF  14  /**< #PF: 页错误 (有错误码) */
#define EXC_MF  16  /**< #MF: x87 FPU 浮点错误 */
#define EXC_AC  17  /**< #AC: 对齐检查 (有错误码) */
#define EXC_MC  18  /**< #MC: 机器检查 */
#define EXC_XM  19  /**< #XM: SIMD 浮点异常 */
#define EXC_VE  20  /**< #VE: 虚拟化异常 */

// ============================================================
// 页错误 (Page Fault) 错误码位定义
// ============================================================

#define PF_P      0x01   /**< P: 页存在 (0=缺页, 1=保护违规) */
#define PF_W      0x02   /**< W: 写访问 (0=读, 1=写) */
#define PF_U      0x04   /**< U: 用户模式访问 (0=内核, 1=用户) */
#define PF_RSVD   0x08   /**< RSVD: 保留位违规 */
#define PF_I      0x10   /**< I: 取指令错误 */

// ============================================================
// MSR (Model-Specific Register) 地址
// ============================================================

#define MSR_EFER   0xC0000080   /**< 扩展功能启用寄存器 */
#define MSR_STAR   0xC0000081   /**< SYSCALL 目标 CS/SS */
#define MSR_LSTAR  0xC0000082   /**< SYSCALL 64 位入口点 */
#define MSR_CSTAR  0xC0000083   /**< SYSCALL 兼容模式入口点 */
#define MSR_SFMASK 0xC0000084   /**< SYSCALL RFLAGS 掩码 */

// ============================================================
// EFER (Extended Feature Enable Register) 位定义
// ============================================================

#define EFER_SCE  0x01   /**< SCE: SYSCALL 启用 */
#define EFER_LME  0x08   /**< LME: Long Mode 启用 */
#define EFER_LMA  0x10   /**< LMA: Long Mode 活跃（只读） */
#define EFER_NXE  0x20   /**< NXE: No-Execute 启用 */

// ============================================================
// 内联汇编辅助函数
// ============================================================

/** 写入 MSR (wrmsr 指令) */
static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/** 读取 MSR (rdmsr 指令) */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/** 使单个 TLB 条目失效 (invlpg 指令) */
static inline void invlpg(void *addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

/** 读取 CR2（页错误线性地址） */
static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

/** 读取 CR3（页表基址） */
static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

/** 写入 CR3（切换页表，隐式刷新 TLB） */
static inline void write_cr3(uint64_t v) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

/** 禁用中断 (cli 指令) */
static inline void cli(void)  { __asm__ volatile("cli"); }

/** 启用中断 (sti 指令) */
static inline void sti(void)  { __asm__ volatile("sti"); }

/** 暂停 CPU 直到下一个中断 (hlt 指令) */
static inline void hlt(void)  { __asm__ volatile("hlt"); }

// ============================================================
// GDT/IDT API 声明（实现在 gdt_idt.c）
// ============================================================

void gdt_init(void);
void gdt_idt_init(void);
void tss_set_stack(uint64_t rsp0);
void int_enable(void);
void int_disable(void);
bool int_get_state(void);

#endif /* HBOS_CPU_H */
