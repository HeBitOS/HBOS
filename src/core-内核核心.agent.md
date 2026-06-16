# Core — 内核核心 (`src/core/`)

> CPU 基础设施: GDT/IDT/中断/异常, 物理/虚拟内存管理, 堆分配器, 任务调度

## 文件清单

| 文件 | 行数 | 职责 |
|------|------|------|
| `gdt_idt.c` | 377 | GDT (ring0/ring3 段 + TSS), IDT (32 异常 + 16 IRQ + int 0x80), PIC 重映射 |
| `pmm.c` | 292 | 物理内存管理器, bitmap 方式跟踪 4KB 页 |
| `vmm.c` | 262 | 虚拟内存管理器, 4-level paging, 接管 boot.asm 恒等映射 |
| `heap.c` | 148 | 内核堆, 简单 bump allocator, 128KB 静态池 |
| `task.c` | 668 | 协作式任务调度器, ring3 用户任务, TSS ring0 栈 |
| `interrupt_asm.asm` | - | 中断入口汇编, 保存/恢复寄存器, 调 C handler |
| `task_switch.asm` | - | 上下文切换汇编 |
| `cpu.h` | - | 内联 asm helpers: `int_disable()`, `int_enable()`, `pause()`, `halt()`, `cr*` 读写 |

## 初始化顺序（`src/kernel.c:kmain`）

```
gdt_idt_init()     → GDT + IDT + PIC 重映射 (IRQ0-15 → INT 32-47)
pit_init(100)       → PIT 可编程间隔定时器 (100Hz)
acpi_init(mbi)      → 解析 ACPI 表 (S5 关机, MADT CPU 检测)
smp_init()          → 启动 AP 核 (trampoline 代码)
tss_set_stack(rsp)  → 当前 BSP 的 TSS ring0 栈指针
pmm_init(mbi)       → 从 Multiboot2 内存映射初始化 PMM
vmm_init(cr3)       → 接管页表
heap_init()         → 内核堆
task_init()         → 任务系统 (创建 idle task + ring3 shell)
```

## PMM (`pmm.c`) — 物理内存管理器

```c
// 以 4KB 为单位，bitmap 每 bit 对应一页
int  pmm_alloc_page(uint64_t *phys_out);  // 分配一页物理地址
void pmm_free_page(uint64_t phys);         // 释放
int  pmm_get_total_pages(void);            // 总页数
int  pmm_get_free_pages(void);             // 空闲页数
#define PMM_PAGE_SIZE 4096
// 内部：pmm_is_used(idx) / pmm_mark_used(idx) / pmm_mark_free(idx)
```

## VMM (`vmm.c`) — 虚拟内存管理器

```c
void vmm_init(uint64_t cr3);              // 接管 boot.asm 创建的页表
int  vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
int  vmm_unmap(uint64_t virt);
void vmm_page_fault_handler(uint64_t cr2, uint64_t error_code);
// flags: VMM_PRESENT=0x1, VMM_WRITABLE=0x2, VMM_USER=0x4, VMM_NX=0x8000000000000000
// 页表层级: PML4 → PDPT → PD → PT，每个表 512 entries
```

## Heap (`heap.c`) — 内核堆

```c
void *kmalloc(size_t n);    // 128KB bump heap, 4KB 粒度扩展到 PMM
void kfree(void *p);         // no-op (bump allocator 不回收)
void heap_init(void);        // 初始化堆
#define HEAP_INITIAL_SIZE (128 * 1024)
// 扩展: 调 pmm_alloc_page 追加 4KB 页
```

## Task (`task.c`) — 任务调度

```c
#define MAX_TASKS 16
typedef struct { ... } task_t;

void task_init(void);                                // 创建初始 idle + ring3 任务
int  task_create(void (*entry)(void), int ring);      // ring0=内核线程, ring3=用户进程
void task_yield(void);                                // 主动让出 CPU (协作式)
void task_sleep(uint32_t ms);                         // 睡眠 (基于 PIT tick)
uint32_t task_get_id(void);
task_t *task_current(void);
void tss_set_stack(uint64_t rsp);                     // 当前核 TSS ring0 栈

// 上下文切换: 保存 callee-saved regs (rbx,rbp,r12-r15,rip,rsp,cr3)
//              恢复下一个 task 的上下文
// 用户任务: 使用 IRETQ 进入 ring3
// 系统调用: int 0x80 → 进入内核 ISR → syscall_handler → 返回通过 IRETQ
```

## GDT/IDT (`gdt_idt.c`) — 段与中断

```c
void gdt_idt_init(void);
// GDT entries: null, ring0 代码/数据, ring3 代码/数据, TSS
// IDT entries: 0-31 CPU exception, 32-47 IRQ, 0x80 系统调用
// PIC remap: master 0x20→0x28, slave 0xA0→0xA8
```

## CPU helpers (`cpu.h`)

```c
static inline void int_disable(void) { __asm__ volatile("cli"); }
static inline void int_enable(void)  { __asm__ volatile("sti"); }
static inline void halt(void)        { __asm__ volatile("hlt"); }
static inline void pause_spin(void)  { __asm__ volatile("pause"); }
static inline uint64_t read_cr0/2/3/4(void);
static inline void write_cr0/2/3/4(uint64_t val);
static inline uint64_t read_msr(uint32_t msr);
static inline void write_msr(uint32_t msr, uint64_t val);
```

## 中断上下文注意事项

- **禁止在 ISR 里调**: `console_print`, `gfx_*`, `kmalloc`, `task_sleep`, `task_yield`
- **ISR 里可以做**: 读写端口 I/O, 简单计数器++, 入队原子缓冲区
- 键盘 ISR (`kb_irq_handler`): 从 0x60 读 scancode, 进 ring buffer (`kb_irq_enqueue_scancode`)
- 只能在任务上下文调 `kb_poll_key()` / `console_print()`
