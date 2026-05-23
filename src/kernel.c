#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "tools/tool.h"

// ============================================================
// HBOS 内核主入口 — 统一通过 graphics 子系统输出
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

// ============================================================
// 外部模块接口
// ============================================================
#include "graphics/graphics.h"
#include "shell/shell.h"
#include "core/task.h"

// ============================================================
// 内核入口
// ============================================================
void kmain(void *mbi) {
    serial_init();
    serial_print("\n===== HBOS bata1 Starting =====\n");

    graphics_init(mbi);
    console_clear();

    // Banner
    console_write("\n", 1);
    console_write("========================================\n", 41);
    console_write("      HBOS - He Bit OS bata1\n", 29);
    console_write("       64-bit Operating System\n", 31);
    console_write("========================================\n\n", 41);

    // CJK 测试
    console_puts("\x1b[33m");
    console_puts("[CJK] 你好，世界！Hello World!\n");
    console_puts("[CJK] 操作系统内核测试\n");
    console_puts("[CJK] 汉字显示 一二三四五\n");
    console_puts("\x1b[0m");
    console_puts("\n");

    task_init();
    shell_init();
    tool_init_all();  // 注册所有工具模块命令

    console_puts("Type 'help' for commands\n\n");
    serial_print("[KERN] Shell ready\n");

    shell_run();
}