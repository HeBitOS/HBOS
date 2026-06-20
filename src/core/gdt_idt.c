#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "cpu.h"
#include "../syscall.h"
#include "../graphics/graphics.h"

// ============================================================
// Port I/O helpers (static — self-contained in this file)
// ============================================================
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void io_wait(void) { outb(0x80, 0); }

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
static tss_t      g_tss;

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
extern void tss_flush(void);

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

    // TSS descriptor
    tss_desc_fill(&gdt_entries[GDT_TSS_LOW], &gdt_entries[GDT_TSS_HIGH],
                  (uintptr_t)&g_tss, sizeof(tss_t) - 1);
    g_tss.rsp0 = 0; // will be set by tss_set_stack

    // Flush GDT and load TSS
    gdt_ptr_t gdt_ptr;
    gdt_ptr.limit = sizeof(gdt_entries) - 1;
    gdt_ptr.base  = (uint64_t)gdt_entries;
    gdt_flush((uint64_t)&gdt_ptr);
    tss_flush();
}

void tss_set_stack(uint64_t rsp0) {
    g_tss.rsp0 = rsp0;
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
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);

    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC1_DATA, IRQ_BASE);              io_wait(); // Master offset
    outb(PIC2_DATA, IRQ_BASE + 8);          io_wait(); // Slave offset
    outb(PIC1_DATA, 4);                     io_wait(); // Slave at IRQ2
    outb(PIC2_DATA, 2);                     io_wait(); // Cascade identity
    outb(PIC1_DATA, 0x01);                  io_wait(); // 8086 mode
    outb(PIC2_DATA, 0x01);                  io_wait();

    outb(PIC1_DATA, a1); // Restore masks
    outb(PIC2_DATA, a2);
}

static void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
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

    console_puts("\n\x1b[31m===== KERNEL PANIC =====\x1b[0m\n");
    console_puts("Exception: ");
    console_puts(name);
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
void irq_handler(isr_regs_t *regs) {
    uint8_t irq = (uint8_t)(regs->int_no - IRQ_BASE);

    if (irq == 7 || irq == 15) {
        pic_send_eoi(irq);
        return;
    }

    if (irq == 0) {
        pic_send_eoi(0);
        extern void task_schedule(void);
        task_schedule();
        return;
    }

    if (irq == 1) {
        if (inb(0x64) & 1) {
            uint8_t st = inb(0x64);
            if (!(st & 0x20)) {
                uint8_t sc = inb(0x60);
                extern void kb_irq_enqueue_scancode(uint8_t sc);
                kb_irq_enqueue_scancode(sc);
            } else {
                uint8_t b = inb(0x60);
                extern void ps2_mouse_enqueue_byte(uint8_t b);
                ps2_mouse_enqueue_byte(b);
            }
        }
        pic_send_eoi(1);
        return;
    }

    if (irq == 12) {
        if (inb(0x64) & 1) {
            uint8_t st = inb(0x64);
            if (st & 0x20) {
                uint8_t b = inb(0x60);
                extern void ps2_mouse_enqueue_byte(uint8_t b);
                ps2_mouse_enqueue_byte(b);
            } else {
                uint8_t sc = inb(0x60);
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

    // Remap PIC and register IRQ handlers
    pic_remap();

    for (int i = 0; i < 16; i++) {
        uint64_t addr = irq_stub_table[i];
        idt_set_entry(IRQ_BASE + i, addr, IDT_PRESENT | IDT_DPL0 | IDT_INT_GATE);
    }

    idt_set_entry(HBOS_SYSCALL_VECTOR, (uint64_t)syscall_int80_stub,
                  IDT_PRESENT | IDT_DPL3 | IDT_TRAP_GATE);

    // Load IDT
    idt_ptr_t idt_ptr;
    idt_ptr.limit = sizeof(idt_entries) - 1;
    idt_ptr.base  = (uint64_t)idt_entries;
    __asm__ volatile("lidt (%0)" : : "r"(&idt_ptr) : "memory");
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
