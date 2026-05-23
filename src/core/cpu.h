#ifndef HBOS_CPU_H
#define HBOS_CPU_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================
// x86_64 CPU definitions — GDT, IDT, MSRs, control registers
// ============================================================

// ---- GDT entries ----
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

// ---- IDT entries (16 bytes each in 64-bit mode) ----
typedef struct {
    uint16_t offset_low;
    uint16_t segment;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr_t;

// ---- TSS (Task State Segment) ----
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss_t;

// ---- Register dump from ISR ----
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no;
    uint64_t err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) isr_regs_t;

// ---- GDT selectors ----
#define GDT_NULL     0
#define GDT_KCODE    1
#define GDT_KDATA    2
#define GDT_UCODE    3
#define GDT_UDATA    4
#define GDT_TSS_LOW  5
#define GDT_TSS_HIGH 6
#define GDT_ENTRIES  7

#define SEL_KCODE  (GDT_KCODE * 8)
#define SEL_KDATA  (GDT_KDATA * 8)
#define SEL_UCODE  (GDT_UCODE * 8) | 3
#define SEL_UDATA  (GDT_UDATA * 8) | 3
#define SEL_TSS    (GDT_TSS_LOW * 8)

// GDT access byte
#define GDT_ACC_PRESENT    0x80
#define GDT_ACC_DPL0       0x00
#define GDT_ACC_DPL3       0x60
#define GDT_ACC_CODE       0x18   // code, execute/read
#define GDT_ACC_DATA       0x12   // data, read/write
#define GDT_ACC_TSS        0x09   // available TSS (not busy)

// GDT granularity byte
#define GDT_GRAN_4K     0x08
#define GDT_GRAN_64     0x20      // long mode (L-bit)
#define GDT_GRAN_32     0x40      // default operand size

// IDT flags
#define IDT_PRESENT     0x80
#define IDT_DPL0        0x00
#define IDT_DPL3        0x60
#define IDT_INT_GATE    0x0E      // 64-bit interrupt gate
#define IDT_TRAP_GATE   0x0F      // 64-bit trap gate

// ---- IRQ base (PIC remaps IRQ0→INT 32) ----
#define IRQ_BASE 0x20
#define IRQ(n)   (IRQ_BASE + (n))

// ---- CPU exception vectors ----
#define EXC_DE  0   // Divide Error
#define EXC_DB  1   // Debug
#define EXC_NMI 2   // NMI
#define EXC_BP  3   // Breakpoint
#define EXC_OF  4   // Overflow
#define EXC_BR  5   // Bound Range
#define EXC_UD  6   // Invalid Opcode
#define EXC_NM  7   // Device Not Available
#define EXC_DF  8   // Double Fault (ERR)
#define EXC_TS  10  // Invalid TSS (ERR)
#define EXC_NP  11  // Segment Not Present (ERR)
#define EXC_SS  12  // Stack Segment (ERR)
#define EXC_GP  13  // General Protection (ERR)
#define EXC_PF  14  // Page Fault (ERR)
#define EXC_MF  16  // x87 FPU
#define EXC_AC  17  // Alignment Check (ERR)
#define EXC_MC  18  // Machine Check
#define EXC_XM  19  // SIMD FPU
#define EXC_VE  20  // Virtualization

// Page Fault error code bits
#define PF_P      0x01
#define PF_W      0x02
#define PF_U      0x04
#define PF_RSVD   0x08
#define PF_I      0x10

// MSRs
#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_CSTAR  0xC0000083
#define MSR_SFMASK 0xC0000084

#define EFER_SCE  0x01
#define EFER_LME  0x08
#define EFER_LMA  0x10
#define EFER_NXE  0x20

// ---- Inline helpers ----
static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void invlpg(void *addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}
static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}
static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline void write_cr3(uint64_t v) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}
static inline void cli(void)  { __asm__ volatile("cli"); }
static inline void sti(void)  { __asm__ volatile("sti"); }
static inline void hlt(void)  { __asm__ volatile("hlt"); }

// ---- GDT/IDT API declarations ----
void gdt_init(void);
void gdt_idt_init(void);
void tss_set_stack(uint64_t rsp0);
void int_enable(void);
void int_disable(void);
bool int_get_state(void);

#endif /* HBOS_CPU_H */