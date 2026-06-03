/**
 * @file    kernel.c
 * @brief   HBOS 内核主入口 — 系统初始化与启动流程
 *
 * 启动顺序（由 boot.asm 在设置好 long mode + 页表后调用 kmain）：
 *   Phase 1: 串口初始化 → 图形初始化 → 启动横幅 + CJK 测试
 *   Phase 2: GDT + IDT + PIC → ACPI 初始化 → TSS ring0 栈设置
 *   Phase 3: 物理内存管理器 (PMM, bitmap-based)
 *   Phase 4: 虚拟内存管理器 (VMM, 4-level paging)
 *   Phase 5: 内核堆 → VFS + 任务系统 + POSIX 自测
 *   Phase 6: Shell 命令注册 → 进入交互式 Shell 主循环
 */

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
#include "selftest.h"
#include "vfs.h"
#include "acpi.h"
#include "smp.h"
#include "version.h"
#include "net.h"

// ============================================================
// 串口调试输出 (COM1, I/O 端口 0x3F8)
// 用于在内核早期阶段（图形初始化之前）输出调试信息
// ============================================================

/** COM1 串口基地址 */
#define SERIAL_PORT 0x3F8

/** 向 I/O 端口写入一个字节 */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/** 从 I/O 端口读取一个字节 */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/**
 * 初始化 COM1 串口为 115200 波特率
 * 配置: 8N1, DLAB 设置除数锁存器, FIFO 启用
 */
static void serial_init(void) {
    outb(SERIAL_PORT + 1, 0x00);   // 禁用中断
    outb(SERIAL_PORT + 3, 0x80);   // 启用 DLAB（除数锁存器访问）
    outb(SERIAL_PORT + 0, 0x01);   // 除数低字节 = 1 → 115200 baud
    outb(SERIAL_PORT + 1, 0x00);   // 除数高字节 = 0
    outb(SERIAL_PORT + 3, 0x03);   // 8N1, 禁用 DLAB
    outb(SERIAL_PORT + 2, 0xC7);   // 启用 FIFO, 14 字节触发
    outb(SERIAL_PORT + 4, 0x0B);   // DTR+RTS+OUT2
}

/** 向串口发送一个字符（忙等待直到发送缓冲区为空） */
static void serial_putc(char c) {
    while (!(inb(SERIAL_PORT + 5) & 0x20));  // 等待 LSR 第 5 位（THR 空）
    outb(SERIAL_PORT, c);
}

/** 向串口发送一个以 NUL 结尾的字符串（\n 自动转为 \r\n） */
static void serial_print(const char *s) {
    while (*s) { if (*s == '\n') serial_putc('\r'); serial_putc(*s++); }
}

// Forward declarations
void gdt_idt_init(void);

// ============================================================
// 内核入口 — 由 boot.asm 在 64 位 long mode 下调用
// @param mbi  Multiboot2 信息结构指针（GRUB 传入）
// ============================================================
void kmain(void *mbi) {
    // ---- Phase 1: 早期输出与图形 ----
    serial_init();
    serial_print("\n===== " HBOS_VERSION_NAME " Starting =====\n");

    // 初始化图形终端（优先 framebuffer，失败则回退到 VGA 文本模式）
    graphics_init(mbi);
    console_clear();

    // 启动横幅
    console_puts("      " HBOS_VERSION_NAME "\n");
    console_write("       64-bit Operating System\n", 31);
    console_write("========================================\n\n", 41);

    // ---- Phase 2: CPU 异常/中断基础设施 ----
    // GDT（全局描述符表）: ring0/ring3 代码段和数据段 + TSS
    // IDT（中断描述符表）: 32 个 CPU 异常 + 16 个 IRQ + int 0x80 系统调用
    // PIC 重映射: IRQ0-15 → INT 32-47
    gdt_idt_init();
    pit_init(100);
    acpi_init(mbi);  // ACPI 解析（用于关机支持的 S5 睡眠状态 + MADT CPU 检测）
    smp_init();     // SMP 多核初始化（启动 AP）

    // 设置 TSS ring0 栈指针（中断发生时 CPU 自动切换到内核栈）
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    tss_set_stack(rsp);

    // ---- Phase 3: 物理内存管理器 ----
    // 从 Multiboot2 内存映射构建位图，跟踪每个 4KB 页的分配状态
    pmm_init(mbi);

    // ---- Phase 4: 虚拟内存管理器 ----
    // 接管 boot.asm 创建的 4-level 页表（恒等映射 0-4GB）
    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    vmm_init(cr3);

    // ---- Phase 5: 内核堆 ----
    // 简单的 bump 分配器，128KB 静态池，用于 kmalloc/kfree
    heap_init();

    // ---- Phase 5.5: 文件系统 + 任务 + 自测 ----
    // VFS: 基于 ramfs 的内存文件系统（可选 HBFS 磁盘后端）
    // 任务: 协作式轮转调度器，最多 16 个任务
    // 自测: POSIX open/read/write/close/stat/unlink 冒烟测试
    vfs_init();
    task_init();
    net_init();
    selftest_run();

    // ---- Phase 6: Shell ----
    // 注册所有内置命令（help, ls, cat, echo 等）
    // 然后进入交互式 read-eval-print 循环
    shell_init();
    tool_init_all();

    serial_print("[KERN] Shell ready\n");

    shell_run();  // 永不返回
}
