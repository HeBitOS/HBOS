#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "graphics/graphics.h"
#include "shell/shell.h"
#include "core/task.h"
#include "core/cpu.h"
#include "core/pmm.h"
#include "core/vmm.h"
#include "core/heap.h"
#include "tools/tool.h"

// ============================================================
// HBOS 内核主入口
// ============================================================

#define SERIAL_PORT 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void serial_init(void) {
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x80); outb(SERIAL_PORT + 0, 0x01); outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x03); outb(SERIAL_PORT + 2, 0xC7); outb(SERIAL_PORT + 4, 0x0B);
}
static void serial_putc(char c) {
    while (!(inb(SERIAL_PORT + 5) & 0x20));
    outb(SERIAL_PORT, c);
}
static void serial_print(const char *s) {
    while (*s) { if (*s == '\n') serial_putc('\r'); serial_putc(*s++); }
}

// Forward declarations
void gdt_idt_init(void);

// ============================================================
// 内核入口
// ============================================================
void kmain(void *mbi) {
    serial_init();
    serial_print("\n===== HBOS beta1 Starting =====\n");

    graphics_init(mbi);
    console_clear();

    // 启动横幅
    console_write("\n", 1);
    console_write("========================================\n", 41);
    console_write("      HBOS - He Bit OS beta1\n", 29);
    console_write("       64-bit Operating System\n", 31);
    console_write("========================================\n\n", 41);

    // CJK 测试
    console_puts("\x1b[33m");
    console_puts("[CJK] 你好，世界！Hello World!\n");
    console_puts("[CJK] 操作系统内核测试\n");
    console_puts("[CJK] 汉字显示 一二三四五\n");
    console_puts("\x1b[0m");
    console_puts("\n");

    // Phase 2: GDT + IDT + PIC
    gdt_idt_init();

    // Set TSS ring0 stack
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    tss_set_stack(rsp);

    // Phase 3: Physical memory manager
    pmm_init(mbi);

    // Phase 4: Virtual memory manager
    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    vmm_init(cr3);

    // Phase 5: Kernel heap
    heap_init();

    // Phase 6: Task system + Shell
    task_init();
    shell_init();
    tool_init_all();

    console_puts("\nType 'help' for commands\n\n");
    serial_print("[KERN] Shell ready\n");

    shell_run();
}