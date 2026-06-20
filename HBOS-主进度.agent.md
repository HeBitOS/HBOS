# HBOS v0.1 beta2 — 项目主进度

> **What**: 自研 64 位 x86 OS，支持 BIOS/UEFI 双启动，C 语言内核 + 用户空间 App 框架
> **Bootloader**: GRUB2 (BIOS) / Limine (UEFI)
> **Repo**: https://github.com/HeBitOS/HBOS

---

## 内核启动顺序（`src/kernel.c:kmain`）

```
Phase 1: serial_init → graphics_init → console_clear
Phase 2: gdt_idt_init → pit_init → acpi_init → smp_init → TSS rsp
Phase 3: pmm_init (bitmap 4KB pages from Multiboot2 memory map)
Phase 4: vmm_init (接管 boot.asm 4-level paging, 恒等映射 0-4GB)
Phase 5: heap_init → vfs_init → task_init → net_init → selftest_run
Phase 6: shell_init → tool_init_all → shell_run (永不返回)
```

---

## 构建

```bash
make              # 构建 BIOS+.UEFI ISO
make bios         # 仅 BIOS (GRUB2)
make uefi         # 仅 UEFI (Limine)
make run          # 用 QEMU 跑 BIOS
make uefi-run     # 用 QEMU 跑 UEFI
```

**产出**:
- `build/hbos-bios.iso` — GRUB2 BIOS 启动
- `build/hbos-uefi.iso` — Limine UEFI 启动
- `build/hbos-bios.bin` — 原始内核 ELF

---

## 架构概览

```
┌──────────────────────────────────────────┐
│  Shell (src/shell/)                      │  ← 交互式 REPL
├──────────────────────────────────────────┤
│  Tools: gui, file, disk, net, cc, ...    │  ← 内建命令 (src/tools/)
├──────────────────────────────────────────┤
│  VFS → ext2 / fat32 / devfs / ramfs      │  ← 文件系统
├──────────────────────────────────────────┤
│  Net: E1000 / PCnet / RTL8139 / VirtIO   │  ← 网络 (src/net.c)
│  DHCP, ARP, ICMP, DNS, TCP, HTTP         │
├──────────────────────────────────────────┤
│  Task (cooperative, 16 max)              │  ← 任务调度
├──────────────────────────────────────────┤
│  VMM (4-level paging) + Heap + PMM       │  ← 内存
├──────────────────────────────────────────┤
│  GDT/IDT/PIC/PIT + TSS                   │  ← CPU 基础设施
├──────────────────────────────────────────┤
│  Drivers: ATA/AHCI, PS/2, USB xHCI/HID   │
└──────────────────────────────────────────┘
```

---

## 各子系统当前状态

| 子系统 | 状态 | 说明 |
|--------|------|------|
| **启动** | ✅ BIOS + UEFI 双启动 | GRUB2/Limine, 串口调试 115200 |
| **内存** | ✅ PMM + VMM + Heap | Bitmap 4KB 页, 4-level 恒等映射, 128KB bump heap |
| **中断** | ✅ GDT/IDT/PIC/PIT | 32 CPU 异常 + 16 IRQ, ring0/ring3 |
| **多核** | ✅ SMP | AP trampoline, TSS per-core |
| **ACPI** | ✅ | S5 关机, MADT CPU 检测 |
| **任务** | ✅ 协作式 | 最多 16 task, 轮转, ring3 用户任务支持 |
| **输入** | ✅ PS/2 + USB KB/Mouse | 支持 PS/2 与 USB 键盘鼠标，且无 input lag/CPU peg 现象 |
| **显示** | ✅ Framebuffer | VESA 1024x768, 24bpp, CJK 位图字体 |
| **GUI** | ⚠️ 基础可用 | 桌面 + 窗口管理 + 开始菜单, 鼠标有加速 |
| **文件系统** | ⚠️ 部分 | ramfs (默认), ext2 只读, fat32 只读, devfs |
| **网络** | ⚠️ 部分 | E1000 ✅, **PCnet ✅ (刚加的)**, RTL8139/VirtIO 未实现 |
| **TCP/IP** | ✅ 基础可用 | DHCP, ARP, ICMP ping, DNS, TCP, HTTP GET |
| **USB** | ✅ 控制器+设备实现 | 已实现 xHCI 驱动，并支持 USB 键盘、鼠标以及 USB 大容量存储 (MSC) |
| **SATA** | ⚠️ AHCI 框架 | ATA PIO 模式与 USB 存储可用, AHCI 未完成 |
| **Crypto** | ✅ | ChaCha20-Poly1305, SHA-256, X25519 |

---

## 文件拆分指南（多 Agent 并行）

每个子目录有对应的 `*.agent.md`，Agent 只需读自己负责的那份：

| Agent 负责 | 参考文档 | 关键文件 |
|-----------|---------|---------|
| 内核核心 | `src/core-内核核心.agent.md` | `core/gdt_idt.c`, `pmm.c`, `vmm.c`, `heap.c`, `task.c` |
| 输入子系统 | `src/input-输入子系统.agent.md` | `input/mouse.c`, `usb_hid.c`, `xhci.c` |
| 网络 | `src/net-网络子系统.agent.md` | `net.c` (1549 行, E1000+PCnet) |
| GUI/工具 | `src/tools-GUI系统工具.agent.md` | `tools/gui.c` (4600 行), `tools/*.c` |
| Shell | `src/shell-命令行.agent.md` | `shell/shell.c` (1066 行) |
| 图形/字体 | `src/graphics-图形字体.agent.md` | `graphics/graphics.c`, `font_cjk.c` |
| GUI 框架 | `src/gui-GUI子系统.agent.md` | `gui/compositor.c`, `wm.c` |
| 文件系统 | `src/fs-文件系统.agent.md` | `vfs.c`, `fs.c`, `ext2.c`, `fat32.c` |
| 用户空间 | `src/user-用户空间.agent.md` | `user/app_runtime.c`, `user/ldso.c`, `user/progs/` |
| 加密 | `src/crypto-加密.agent.md` | `crypto/chacha20_poly1305.c`, `sha256.c`, `x25519.c` |

---

## 已知问题 (TODO)

1. **网卡 RTL8139 / VirtIO 驱动未实现** — 检测到了选不上
2. **AHCI 未完成** — 磁盘支持 ATA PIO 模式与 USB 存储模式
3. **ext2/fat32 只读** — 无写入实现
4. **GUI 基础** — 无窗口拖拽, 无应用框架, 开始菜单是静态的
5. **任务调度** — 协作式, 无抢占, 无同步原语

---

## 关键 API（其他 Agent 需要的接口）

```c
// 输出
void console_print(const char *s);         // graphics/graphics.h
void gfx_draw_pixel(int x, int y, uint32_t color);

// 内存
void *kmalloc(size_t n);                   // core/heap.h
void kfree(void *p);

// 任务
void task_yield(void);                     // core/task.h
void task_sleep(uint32_t ms);

// 文件
int vfs_open(const char *path, int flags); // vfs.h
int vfs_read(int fd, void *buf, int len);
int vfs_write(int fd, const void *buf, int len);

// PCI
int pci_find_class(uint8_t class, uint8_t subclass, uint8_t progif, pci_device_t *out);
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

// 网络
void net_init(void);
const net_device_t *net_primary(void);

// 端口 I/O（内联 asm，直接抄）
static inline void outb(uint16_t port, uint8_t val) { __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outw(uint16_t port, uint16_t val) { __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port)); }
static inline uint16_t inw(uint16_t port) { uint16_t v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
```

---

## 编码规范（Agent 必读）

- **所有代码 C99**, `__asm__ volatile` (AT&T syntax) 内联汇编
- 头文件用 `#include "..."` (相对路径), 标准头用 `#include <stdint.h>` (已内置在 `src/types/`)
- 内存用 `kmalloc`/`kfree` — 不要用 malloc
- PCI 操作用 `pci_read32`/`pci_write32`/`pci_read16`/`pci_read8` — 没有 pci_write16!
- 注册 Shell 命令:
  ```c
  static int my_cmd_fn(int argc, char *argv[]) { ... return 0; }
  static command_t my_cmd = { .name="xxx", .desc="...", .group=CMD_GROUP_SYSTEM, .fn=my_cmd_fn };
  // 在 tool_init_all() 里调用 cmd_register(&my_cmd);
  ```
- **不要**在中断上下文调用 `console_print` / `gfx_*`
- 不改 Makefile 架构, 不改 linker.ld/linker_bios.ld
