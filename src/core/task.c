/**
 * @file    task.c
 * @brief   协作式多任务调度器 — 轮转调度 (Round-Robin)
 *
 * 实现一个简单的协作式多任务系统:
 *   - 最多 64 个任务 (MAX_TASKS)，覆盖内容壳与 KDE 基础线程负载
 *   - 每个任务 8KB 栈 (TASK_STACK_SIZE)
 *   - 轮转调度: 按循环链表顺序选择下一个 READY 任务
 *   - 协作式: 任务通过 task_yield() 主动让出 CPU
 *
 * 任务状态:
 *   TASK_READY      可运行，等待调度
 *   TASK_RUNNING    当前正在运行
 *   TASK_BLOCKED    阻塞中（预留，当前未使用）
 *   TASK_TERMINATED 已终止
 *
 * 上下文切换:
 *   task_switch(prev_rsp, next_rsp) 在 task_switch.asm 中实现
 *   保存/恢复 callee-saved 寄存器: RBP, RBX, R12-R15
 *
 * 新任务创建:
 *   在任务栈上预置一个假的上下文帧，使首次调度时
 *   task_switch 的 pop/ret 序列跳转到 task_entry_trampoline，
 *   然后调用 entry(arg)。
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "task.h"
#include "vmm.h"
#include "cpu.h"
#include "heap.h"
#include "../smp.h"
#include "../linux_compat.h"
#include "../user/ldso.h"

static void task_sig_deliver(task_t *task);
#include "../signal.h"
#include "../string.h"
#include "../graphics/graphics.h"

// ============================================================
// 外部汇编原语
// ============================================================

/** 上下文切换: 保存当前 RSP，加载下一个 RSP */
extern void task_switch(uint64_t *prev_rsp, uint64_t *next_rsp, void *prev_fpu, void *next_fpu);

/** 新任务入口蹦床: pop arg → pop entry → call entry(arg) → task_exit() */
extern void task_entry_trampoline(void);

/** Ring3 入口蹦床: 构建 iretq 帧并切换到 ring3 */
extern void task_enter_ring3(void);
extern void task_resume_linux_syscall(void);

typedef struct {
    uint64_t user_entry;
    uint64_t user_stack;
    uint64_t user_argc;
    uint64_t user_argv;
} ring3_launch_ctx_t;

// ============================================================
// 内部状态
// ============================================================

static task_t task_pool[MAX_TASKS];          /**< 任务池（静态分配） */
static int task_count = 0;                   /**< 当前任务数 */
static task_t *current_task = NULL;          /**< 当前运行的任务 */
static uint32_t next_id = 0;                 /**< 下一个任务 ID */

/** 预分配的栈空间: 64 个任务 × 8KB = 512KB */
static char task_stacks[MAX_TASKS][TASK_STACK_SIZE]
    __attribute__((aligned(16)));

/** 每任务 FXSAVE/FXRSTOR 区域: 64 个任务 × 512 字节，16 字节对齐（FXSAVE 硬性要求）。 */
static uint8_t task_fpu_state[MAX_TASKS][512]
    __attribute__((aligned(16)));
static fd_table_t task_fd_tables[MAX_TASKS];
static task_mm_t task_memory_spaces[MAX_TASKS];
static uint64_t retired_address_spaces[MAX_TASKS];
static size_t retired_address_space_count;

/**
 * 将一块 FXSAVE 区域初始化为 CPU 复位后的默认状态（FCW=0x037F,
 * MXCSR=0x1F80，其余全零）。新任务从未运行过，没有"当前"FPU 状态可保存，
 * 所以第一次 FXRSTOR 到它时需要一个合法的初始镜像——尤其 MXCSR 不能是 0
 * （那样会解除所有 SIMD 异常屏蔽，导致本可静默产生 NaN/Inf 的运算改为
 * 触发 #XM）。纯内存写入，不执行任何真实 FPU 指令，因此不会影响调用者
 * （可能是任意其他任务）此刻的真实 FPU 寄存器状态。
 */
static void task_fpu_init(void *area) {
    memset(area, 0, 512);
    *(uint16_t *)((uint8_t *)area + 0)  = 0x037F; /* FCW */
    *(uint32_t *)((uint8_t *)area + 24) = 0x1F80; /* MXCSR */
}

static void task_fpu_capture(void *area) {
    /* clone()/fork() inherit the calling thread's live floating-point
     * environment.  The task's cached FXSAVE image may predate its current
     * timeslice, so snapshot hardware here instead of copying stale state. */
    __asm__ volatile("fxsave (%0)" :: "r"(area) : "memory");
}

static uint64_t task_irq_save(void) {
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
    return rflags;
}

static void task_irq_restore(uint64_t rflags) {
    if (rflags & 0x200)
        __asm__ volatile("sti" ::: "memory");
    else
        __asm__ volatile("cli" ::: "memory");
}

static void task_switch_tls(task_t *previous, task_t *next) {
    if (previous) previous->fs_base = rdmsr(MSR_FS_BASE);
    wrmsr(MSR_FS_BASE, next ? next->fs_base : 0);
}

static int task_user_address(uint64_t address) {
    return address != 0 && address < 0x0000800000000000ULL;
}

/*
 * Robust-list pointers are controlled by ring 3.  Validate every page before
 * dereferencing them so a malformed list is ignored instead of faulting the
 * kernel during thread teardown.  The currently active address space is the
 * exiting task's address space (task_kill only invokes this for a shared mm).
 */
static int task_user_mapped_range(const void *pointer, size_t length) {
    uintptr_t start = (uintptr_t)pointer;
    if (!length || !task_user_address(start) ||
        start > UINTPTR_MAX - (length - 1))
        return 0;
    uintptr_t end = start + length - 1;
    if (!task_user_address(end)) return 0;
    for (uintptr_t page = start & ~(uintptr_t)(PAGE_SIZE - 1);;
         page += PAGE_SIZE) {
        if (!vmm_get_phys(page)) return 0;
        if (page >= (end & ~(uintptr_t)(PAGE_SIZE - 1))) break;
        if (page > UINTPTR_MAX - PAGE_SIZE) return 0;
    }
    return 1;
}

static fd_table_t *task_fd_table_alloc(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_fd_tables[i].refs) continue;
        memset(&task_fd_tables[i], 0, sizeof(task_fd_tables[i]));
        task_fd_tables[i].refs = 1;
        return &task_fd_tables[i];
    }
    return NULL;
}

static task_mm_t *task_mm_alloc(uint64_t pml4_phys,
                                uint64_t heap_start,
                                uint64_t heap_limit,
                                bool owns_pml4) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_memory_spaces[i].refs) continue;
        memset(&task_memory_spaces[i], 0, sizeof(task_memory_spaces[i]));
        task_memory_spaces[i].refs = 1;
        task_memory_spaces[i].owns_pml4 = owns_pml4;
        task_memory_spaces[i].pml4_phys = pml4_phys;
        task_memory_spaces[i].user_heap_start = heap_start;
        task_memory_spaces[i].user_brk = heap_start;
        task_memory_spaces[i].user_heap_limit = heap_limit;
        return &task_memory_spaces[i];
    }
    return NULL;
}

static void task_reap_address_spaces(void) {
    size_t write = 0;
    for (size_t i = 0; i < retired_address_space_count; i++) {
        uint64_t pml4 = retired_address_spaces[i];
        if (pml4 == vmm_get_pml4()) {
            retired_address_spaces[write++] = pml4;
            continue;
        }
        vmm_destroy_address_space(pml4);
    }
    retired_address_space_count = write;
}

static void task_retire_address_space(uint64_t pml4) {
    if (!pml4) return;
    if (pml4 != vmm_get_pml4()) {
        vmm_destroy_address_space(pml4);
        return;
    }
    if (retired_address_space_count < MAX_TASKS)
        retired_address_spaces[retired_address_space_count++] = pml4;
}

static void task_release_mm(task_t *task) {
    if (!task || !task->mm || !task->mm->refs) return;
    if (--task->mm->refs == 0) {
        uint64_t pml4 = task->mm->pml4_phys;
        bool owns_pml4 = task->mm->owns_pml4;
        ldso_release_address_space(task->mm);
        vm_area_t *area = task->mm->areas;
        while (area) {
            vm_area_t *next = area->next;
            if (area->backing_type == 1)
                linux_compat_memfd_unmap(area->backing_id);
            kfree(area);
            area = next;
        }
        memset(task->mm, 0, sizeof(*task->mm));
        if (owns_pml4) task_retire_address_space(pml4);
    }
    task->mm = NULL;
}

static int task_copy_vm_areas(task_mm_t *destination,
                              const task_mm_t *source) {
    if (!destination || !source) return -1;
    vm_area_t **tail = &destination->areas;
    for (const vm_area_t *area = source->areas; area; area = area->next) {
        vm_area_t *copy = (vm_area_t *)kmalloc(sizeof(*copy));
        if (!copy) return -1;
        *copy = *area;
        copy->next = NULL;
        if (copy->backing_type == 1 &&
            linux_compat_memfd_retain_map(copy->backing_id) < 0) {
            kfree(copy);
            return -1;
        }
        *tail = copy;
        tail = &copy->next;
    }
    return 0;
}

static void task_release_fd_table(task_t *task) {
    if (!task || !task->fd_table || !task->fd_table->refs) return;
    if (task->fd_table->refs > 1) {
        task->fd_table->refs--;
        task->fd_table = NULL;
        return;
    }
    linux_compat_release_task(task);
    task->fd_table->refs = 0;
    task->fd_table = NULL;
}

// ============================================================
// 调度器 — 轮转选择下一个 READY 任务
// ============================================================

/**
 * 从循环任务链表中查找下一个 READY 状态的任务
 * @return 下一个可运行的任务，无其他任务时返回 NULL
 */
static task_t *sched_next(void) {
    if (task_count <= 1) return NULL;  // 只有主任务，无需切换

    task_t *start = current_task;
    task_t *t = current_task;

    // 循环扫描链表
    do {
        t = t->next;
        if (!t) t = &task_pool[0];  // 回绕到链表头
        if (t->state == TASK_READY) return t;
    } while (t != start);

    return NULL;  // 没有其他可运行任务
}

// ============================================================
// 公共 API
// ============================================================

/**
 * 初始化任务系统
 * 创建主任务 (task 0) 作为初始执行上下文
 */
void task_init(void) {
    task_t *main_task = &task_pool[0];
    main_task->id = next_id++;
    strncpy(main_task->name, "main", TASK_NAME_MAX);
    main_task->name[TASK_NAME_MAX - 1] = '\0';
    main_task->state = TASK_RUNNING;
    main_task->rsp = 0;  // 首次 task_yield 时保存
    main_task->entry = NULL;
    main_task->arg = NULL;
    main_task->exit_status = 0;
    main_task->parent_id = 0;
    main_task->child_id = 0;
    main_task->next = main_task;  // 循环链表（单元素）
    main_task->stack_base = (uint64_t)task_stacks[0];
    main_task->stack_size = TASK_STACK_SIZE;
    main_task->fpu_state = task_fpu_state[0];
    task_fpu_init(main_task->fpu_state);
    main_task->mm = task_mm_alloc(vmm_get_pml4(), 0, 0, false);
    main_task->fs_base = rdmsr(MSR_FS_BASE);
    main_task->thread_group_id = main_task->id;
    main_task->parent_death_signal = 0;
    main_task->dumpable = true;
    main_task->no_new_privs = false;
    main_task->clear_child_tid = NULL;
    main_task->robust_list_head = NULL;
    main_task->robust_list_length = 0;
    memset(main_task->dynamic_tls, 0, sizeof(main_task->dynamic_tls));
    main_task->fd_table = task_fd_table_alloc();
    memset(main_task->sig_handler, 0, sizeof(main_task->sig_handler));
    memset(main_task->sig_action_flags, 0,
           sizeof(main_task->sig_action_flags));
    memset(main_task->sig_action_restorer, 0,
           sizeof(main_task->sig_action_restorer));
    memset(main_task->sig_action_mask, 0,
           sizeof(main_task->sig_action_mask));
    memset(&main_task->sig_pending, 0, sizeof(main_task->sig_pending));
    memset(&main_task->sig_blocked, 0, sizeof(main_task->sig_blocked));
    main_task->sig_exit_code = 0;
    main_task->sig_last_sender_pid = 0;
    main_task->sig_frame_active = false;
    main_task->sig_siginfo_depth = 0;
    main_task->sig_altstack_sp = 0;
    main_task->sig_altstack_size = 0;
    main_task->sig_altstack_flags = 2; /* SS_DISABLE */
    main_task->sig_suppress_delivery = false;
    main_task->host_symtab = 0;
    main_task->host_strtab = 0;
    main_task->host_symtab_count = 0;

    current_task = main_task;
    task_count = 1;
}

/**
 * 创建新任务
 *
 * 在任务栈上构建初始上下文帧:
 *   栈布局（从高地址到低地址）:
 *     [entry]            ← RSP+64 (由蹦床 pop 到 rax)
 *     [arg]              ← RSP+56 (由蹦床 pop 到 rdi)
 *     [trampoline addr]  ← RSP+48 (ret 的目标地址)
 *     [RBP=0]            ← RSP+40
 *     [RBX=0]            ← RSP+32
 *     [R12=0]            ← RSP+24
 *     [R13=0]            ← RSP+16
 *     [R14=0]            ← RSP+8
 *     [R15=0]            ← RSP+0  (最终 RSP)
 *
 * @param name   任务名称（最多 31 字符）
 * @param entry  任务入口函数
 * @param arg    传递给入口函数的参数
 * @return 任务 ID，-1 表示失败
 */
int task_create(const char *name, void (*entry)(void *), void *arg) {
    if (!entry) return -1;

    int idx = -1;
    int reuse = 0;
    for (int i = 1; i < task_count; i++) {
        if (task_pool[i].state == TASK_TERMINATED) {
            idx = i;
            reuse = 1;
            break;
        }
    }
    if (idx < 0) {
        if (task_count >= MAX_TASKS) return -1;
        idx = task_count;
    }
    task_t *tcb = &task_pool[idx];

    tcb->id = next_id++;
    strncpy(tcb->name, name ? name : "task", TASK_NAME_MAX);
    tcb->name[TASK_NAME_MAX - 1] = '\0';
    tcb->state = TASK_READY;
    tcb->entry = entry;
    tcb->arg = arg;
    tcb->exit_status = 0;
    tcb->parent_id = current_task ? current_task->id : 0;
    tcb->child_id = 0;
    tcb->stack_base = (uint64_t)task_stacks[idx];
    tcb->stack_size = TASK_STACK_SIZE;
    tcb->fpu_state = task_fpu_state[idx];
    task_fpu_init(tcb->fpu_state);
    uint64_t task_pml4 = vmm_create_address_space();
    if (!task_pml4) return -1;
    tcb->mm = task_mm_alloc(task_pml4, 0, 0, true);
    if (!tcb->mm) {
        vmm_destroy_address_space(task_pml4);
        return -1;
    }
    tcb->fs_base = 0;
    tcb->thread_group_id = tcb->id;
    tcb->parent_death_signal = 0;
    tcb->dumpable = true;
    tcb->no_new_privs = current_task ? current_task->no_new_privs : false;
    tcb->clear_child_tid = NULL;
    tcb->robust_list_head = NULL;
    tcb->robust_list_length = 0;
    memset(tcb->dynamic_tls, 0, sizeof(tcb->dynamic_tls));
    tcb->fd_table = task_fd_table_alloc();
    if (!tcb->fd_table) {
        task_release_mm(tcb);
        return -1;
    }
    memset(tcb->sig_handler, 0, sizeof(tcb->sig_handler));
    memset(tcb->sig_action_flags, 0, sizeof(tcb->sig_action_flags));
    memset(tcb->sig_action_restorer, 0, sizeof(tcb->sig_action_restorer));
    memset(tcb->sig_action_mask, 0, sizeof(tcb->sig_action_mask));
    memset(&tcb->sig_pending, 0, sizeof(tcb->sig_pending));
    memset(&tcb->sig_blocked, 0, sizeof(tcb->sig_blocked));
    tcb->sig_exit_code = 0;
    tcb->sig_last_sender_pid = 0;
    tcb->sig_frame_active = false;
    tcb->sig_siginfo_depth = 0;
    tcb->sig_altstack_sp = 0;
    tcb->sig_altstack_size = 0;
    tcb->sig_altstack_flags = 2; /* SS_DISABLE */
    tcb->sig_suppress_delivery = false;
    tcb->host_symtab = 0;
    tcb->host_strtab = 0;
    tcb->host_symtab_count = 0;

    // ---- 构建初始栈帧 ----
    // 从栈顶向下填充（栈向低地址增长）
    uint64_t *sp = (uint64_t *)(tcb->stack_base + tcb->stack_size);

    *--sp = (uint64_t)entry;                   // 最高地址槽位
    *--sp = (uint64_t)arg;
    *--sp = (uint64_t)task_entry_trampoline;   // ret 跳转目标
    *--sp = 0x2;           // RFLAGS (IF enabled by trampoline)
    *--sp = 0;  // RBP
    *--sp = 0;  // RBX
    *--sp = 0;  // R12
    *--sp = 0;  // R13
    *--sp = 0;  // R14
    *--sp = 0;  // R15 (最低地址 = 最终 RSP)

    tcb->rsp = (uint64_t)sp;

    if (!reuse) {
        // 插入循环链表（在 head 之后）
        tcb->next = task_pool[0].next;
        task_pool[0].next = tcb;

        task_count++;
    }
    return tcb->id;
}

/**
 * 创建 ring3 用户任务
 *
 * 与 task_create 类似，但入口函数为 task_enter_ring3，
 * 参数为 ring3_launch_ctx_t { user_entry, user_stack }。
 * 首次调度时，task_entry_trampoline 调用 task_enter_ring3(ctx)，
 * 后者构建 iretq 帧切换到 ring3 执行用户代码。
 *
 * @param name        任务名称
 * @param user_entry  用户代码入口地址
 * @param user_stack  用户栈顶地址
 * @return 任务 ID，-1 表示失败
 */
int task_create_ring3_full(const char *name, uint64_t user_entry,
                           uint64_t user_stack, uint64_t user_argc,
                           uint64_t user_argv, uint64_t pml4_phys) {
    if (!user_entry || !user_stack) {
        if (pml4_phys) vmm_destroy_address_space(pml4_phys);
        return -1;
    }

    int idx = -1;
    int reuse = 0;
    for (int i = 1; i < task_count; i++) {
        if (task_pool[i].state == TASK_TERMINATED) {
            idx = i;
            reuse = 1;
            break;
        }
    }
    if (idx < 0) {
        if (task_count >= MAX_TASKS) {
            if (pml4_phys) vmm_destroy_address_space(pml4_phys);
            return -1;
        }
        idx = task_count;
    }
    task_t *tcb = &task_pool[idx];

    tcb->id = next_id++;
    strncpy(tcb->name, name ? name : "user", TASK_NAME_MAX);
    tcb->name[TASK_NAME_MAX - 1] = '\0';
    tcb->state = TASK_READY;
    tcb->entry = NULL;
    tcb->arg = NULL;
    tcb->exit_status = 0;
    tcb->parent_id = current_task ? current_task->id : 0;
    tcb->child_id = 0;
    tcb->stack_base = (uint64_t)task_stacks[idx];
    tcb->stack_size = TASK_STACK_SIZE;
    tcb->fpu_state = task_fpu_state[idx];
    task_fpu_init(tcb->fpu_state);
    uint64_t task_pml4 = pml4_phys ? pml4_phys :
                         vmm_create_address_space();
    if (!task_pml4) return -1;
    tcb->mm = task_mm_alloc(
        task_pml4,
        TASK_USER_HEAP_START,
        TASK_USER_HEAP_START + TASK_USER_HEAP_SIZE,
        true);
    if (!tcb->mm) {
        vmm_destroy_address_space(task_pml4);
        return -1;
    }
    tcb->fs_base = 0;
    tcb->thread_group_id = tcb->id;
    tcb->parent_death_signal = 0;
    tcb->dumpable = true;
    tcb->no_new_privs = current_task ? current_task->no_new_privs : false;
    tcb->clear_child_tid = NULL;
    tcb->robust_list_head = NULL;
    tcb->robust_list_length = 0;
    memset(tcb->dynamic_tls, 0, sizeof(tcb->dynamic_tls));
    tcb->fd_table = task_fd_table_alloc();
    if (!tcb->fd_table) {
        task_release_mm(tcb);
        return -1;
    }
    memset(tcb->sig_handler, 0, sizeof(tcb->sig_handler));
    memset(tcb->sig_action_flags, 0, sizeof(tcb->sig_action_flags));
    memset(tcb->sig_action_restorer, 0, sizeof(tcb->sig_action_restorer));
    memset(tcb->sig_action_mask, 0, sizeof(tcb->sig_action_mask));
    memset(&tcb->sig_pending, 0, sizeof(tcb->sig_pending));
    memset(&tcb->sig_blocked, 0, sizeof(tcb->sig_blocked));
    tcb->sig_exit_code = 0;
    tcb->sig_last_sender_pid = 0;
    tcb->sig_frame_active = false;
    tcb->sig_siginfo_depth = 0;
    tcb->sig_altstack_sp = 0;
    tcb->sig_altstack_size = 0;
    tcb->sig_altstack_flags = 2; /* SS_DISABLE */
    tcb->sig_suppress_delivery = false;
    tcb->host_symtab = 0;
    tcb->host_strtab = 0;
    tcb->host_symtab_count = 0;

    // Allocate ring3_launch_ctx_t on the task's kernel stack
    uint64_t *sp = (uint64_t *)(tcb->stack_base + tcb->stack_size);

    // Place ctx struct on stack (aligned)
    sp -= 4; // ring3_launch_ctx_t
    ring3_launch_ctx_t *ctx = (ring3_launch_ctx_t *)sp;
    ctx->user_entry = user_entry;
    ctx->user_stack = user_stack;
    ctx->user_argc = user_argc;
    ctx->user_argv = user_argv;

    // Build the trampoline frame
    *--sp = (uint64_t)task_enter_ring3;          // entry → rax
    *--sp = (uint64_t)ctx;                       // arg → rdi
    *--sp = (uint64_t)task_entry_trampoline;     // ret target
    *--sp = 0x2;           // RFLAGS (IF enabled by trampoline)
    *--sp = 0;  // RBP
    *--sp = 0;  // RBX
    *--sp = 0;  // R12
    *--sp = 0;  // R13
    *--sp = 0;  // R14
    *--sp = 0;  // R15

    tcb->rsp = (uint64_t)sp;

    if (!reuse) {
        tcb->next = task_pool[0].next;
        task_pool[0].next = tcb;
        task_count++;
    }
    return tcb->id;
}

int task_create_ring3_as(const char *name, uint64_t user_entry,
                         uint64_t user_stack, uint64_t pml4_phys) {
    return task_create_ring3_full(name, user_entry, user_stack, 0, 0, pml4_phys);
}

int task_create_ring3(const char *name, uint64_t user_entry, uint64_t user_stack) {
    return task_create_ring3_as(name, user_entry, user_stack, 0);
}

/**
 * 让出 CPU — 协作式调度入口
 * 当前任务状态变为 READY，切换到下一个 READY 任务
 */
void task_yield(void) {
    uint64_t irq_flags = task_irq_save();
    smp_sched_lock();
    task_t *prev = current_task;
    task_t *next = sched_next();

    if (!next || next == prev) {
        smp_sched_unlock();
        task_irq_restore(irq_flags);
        /* 没有其他可运行任务：让出 CPU 等待中断（PIT 定时器、键盘、
         * 鼠标等）。否则单任务场景（GUI 桌面 / shell）会在主循环里
         * 忙等空转，QEMU 与真机上 CPU 持续 100%。PIT 在启动早期已
         * 以 100Hz 初始化，HLT 必然被周期性唤醒，不会睡死。 */
        __asm__ volatile("sti; hlt");
        return;
    }

    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    current_task = next;

    if (next->mm && next->mm->pml4_phys)
        vmm_set_pml4(next->mm->pml4_phys);
    task_reap_address_spaces();
    tss_set_stack(next->stack_base + next->stack_size);
    task_switch_tls(prev, next);

    task_sig_deliver(next);

    task_switch(&prev->rsp, &next->rsp, prev->fpu_state, next->fpu_state);
    smp_sched_unlock();
    task_irq_restore(irq_flags);
}

typedef struct hbos_robust_list {
    struct hbos_robust_list *next;
} hbos_robust_list_t;

typedef struct {
    hbos_robust_list_t list;
    long futex_offset;
    hbos_robust_list_t *list_op_pending;
} hbos_robust_list_head_t;

int task_set_robust_list(void *head, size_t length) {
    if (!current_task || !head ||
        length != sizeof(hbos_robust_list_head_t) ||
        !task_user_mapped_range(head, length))
        return -1;
    current_task->robust_list_head = head;
    current_task->robust_list_length = length;
    return 0;
}

int task_get_robust_list(int tid, void **head, size_t *length) {
    if (!current_task || !head || !length ||
        !task_user_mapped_range(head, sizeof(*head)) ||
        !task_user_mapped_range(length, sizeof(*length)))
        return -1;
    const task_t *target = tid == 0 ? current_task :
        task_get_by_id((uint32_t)tid);
    if (!target) return -1;
    *head = target->robust_list_head;
    *length = target->robust_list_length;
    return 0;
}

static void task_robust_mark_owner_died(task_t *task,
                                        hbos_robust_list_t *node,
                                        long futex_offset) {
    const uint32_t futex_waiters = 0x80000000U;
    const uint32_t futex_owner_died = 0x40000000U;
    const uint32_t futex_tid_mask = 0x3fffffffU;
    uintptr_t raw = (uintptr_t)node;
    uintptr_t futex_address;
    if (futex_offset < 0) {
        uintptr_t magnitude = (uintptr_t)(-(futex_offset + 1)) + 1;
        if (raw < magnitude) return;
        futex_address = raw - magnitude;
    } else {
        uintptr_t offset = (uintptr_t)futex_offset;
        if (raw > UINTPTR_MAX - offset) return;
        futex_address = raw + offset;
    }
    uint32_t *futex = (uint32_t *)futex_address;
    if (((uintptr_t)futex & 3U) ||
        !task_user_mapped_range(futex, sizeof(*futex)))
        return;

    uint32_t old = __atomic_load_n(futex, __ATOMIC_ACQUIRE);
    while ((old & futex_tid_mask) == task->id) {
        uint32_t replacement =
            (old & futex_waiters) | futex_owner_died;
        if (__atomic_compare_exchange_n(futex, &old, replacement, 0,
                                        __ATOMIC_RELEASE,
                                        __ATOMIC_ACQUIRE)) {
            if (old & futex_waiters)
                (void)linux_compat_futex(futex, 1, 1, NULL);
            return;
        }
    }
}

static void task_robust_cleanup(task_t *task) {
    hbos_robust_list_head_t *head = task ?
        (hbos_robust_list_head_t *)task->robust_list_head : NULL;
    if (!head || task->robust_list_length != sizeof(*head) ||
        !task_user_mapped_range(head, sizeof(*head)))
        return;

    hbos_robust_list_t *node = head->list.next;
    hbos_robust_list_t *sentinel = &head->list;
    hbos_robust_list_t *pending = head->list_op_pending;
    long futex_offset = head->futex_offset;
    for (unsigned int visited = 0;
         node && node != sentinel && visited < 2048;
         visited++) {
        if (!task_user_mapped_range(node, sizeof(*node))) break;
        hbos_robust_list_t *next = node->next;
        if (node != pending)
            task_robust_mark_owner_died(task, node, futex_offset);
        node = next;
    }
    if (pending && task_user_mapped_range(pending, sizeof(*pending)))
        task_robust_mark_owner_died(task, pending, futex_offset);
    task->robust_list_head = NULL;
    task->robust_list_length = 0;
}

/**
 * 终止当前任务
 * 标记为 TERMINATED，然后切换到下一个 READY 任务。
 * 如果没有其他任务，系统停机。
 */
void task_exit(void) {
    if (!current_task) return;
    task_robust_cleanup(current_task);
    if (current_task->clear_child_tid) {
        uint32_t *tid = current_task->clear_child_tid;
        current_task->clear_child_tid = NULL;
        __atomic_store_n(tid, 0, __ATOMIC_RELEASE);
        (void)linux_compat_futex(tid, 1, UINT32_MAX, NULL);
    }
    ldso_release_thread_tls(current_task);
    task_release_fd_table(current_task);
    task_release_mm(current_task);
    task_irq_save();
    smp_sched_lock();
    current_task->state = TASK_TERMINATED;

    while (1) {
        task_t *next = sched_next();
        if (!next) {
            console_puts("\n\x1b[31m[KERN] All tasks terminated, halting.\x1b[0m\n");
            while (1) __asm__ volatile("cli; hlt");
        }
        next->state = TASK_RUNNING;
        task_t *prev = current_task;
        current_task = next;
        if (next->mm && next->mm->pml4_phys)
            vmm_set_pml4(next->mm->pml4_phys);
        task_reap_address_spaces();
        tss_set_stack(next->stack_base + next->stack_size);
        task_switch_tls(prev, next);
        task_switch(&prev->rsp, &next->rsp, prev->fpu_state, next->fpu_state);
    }
    smp_sched_unlock();
}

void task_set_exit_status(int status) {
    if (current_task) current_task->exit_status = status;
}

const task_t *task_get_by_id(uint32_t id) {
    for (int i = 0; i < task_count; i++) {
        if (task_pool[i].id == id) return &task_pool[i];
    }
    return NULL;
}

void task_set_host_symtab(uint32_t id, void *symtab, char *strtab, uint64_t count) {
    for (int i = 0; i < task_count; i++) {
        if (task_pool[i].id == id) {
            task_pool[i].host_symtab = symtab;
            task_pool[i].host_strtab = strtab;
            task_pool[i].host_symtab_count = count;
            return;
        }
    }
}

int task_wait(uint32_t id, int *status) {
    task_t *target = NULL;
    for (int i = 0; i < task_count; i++) {
        if (task_pool[i].id == id) {
            target = &task_pool[i];
            break;
        }
    }
    if (!target || target == current_task) return -1;

    while (target->state != TASK_TERMINATED) {
        task_yield();
    }
    if (status) *status = target->exit_status;
    return 0;
}

/** 获取当前任务 ID */
uint32_t task_get_id(void) {
    return current_task ? current_task->id : 0;
}

uint32_t task_get_process_id(void) {
    return current_task ? current_task->thread_group_id : 0;
}

int task_set_tid_address(uint32_t *address) {
    if (!current_task) return -1;
    if (address && !task_user_address((uint64_t)(uintptr_t)address))
        return -1;
    current_task->clear_child_tid = address;
    return (int)current_task->id;
}

uint64_t task_get_fs_base(void) {
    if (!current_task) return 0;
    current_task->fs_base = rdmsr(MSR_FS_BASE);
    return current_task->fs_base;
}

int task_set_fs_base(uint64_t base) {
    /* Reject non-canonical user addresses. */
    if (base >= 0x0000800000000000ULL) return -1;
    if (!current_task) return -1;
    current_task->fs_base = base;
    wrmsr(MSR_FS_BASE, base);
    return 0;
}

/** 获取当前任务 TCB 指针 */
task_t *task_current(void) {
    return current_task;
}

/**
 * 向指定任务发送信号
 * SIGKILL(9) 和 SIGTERM(15) 直接终止任务，
 * 其他信号记录到 pending 掩码中，由任务下次 yield 时处理。
 * @param id  目标任务 ID
 * @param sig 信号编号
 * @return 0 成功，-1 失败
 */
int task_kill(uint32_t id, int sig) {
    if (sig <= 0 || sig >= _NSIG) return -1;

    task_t *target = NULL;
    for (int i = 0; i < task_count; i++) {
        if (task_pool[i].id == id) {
            target = &task_pool[i];
            break;
        }
    }
    if (!target || target->state == TASK_TERMINATED) return -1;

    if (sig == SIGKILL ||
        (sig == SIGTERM &&
         (target->sig_handler[sig] == SIG_DFL ||
          target->sig_handler[sig] == NULL))) {
        target->sig_exit_code = (sig == SIGKILL) ? 9 : 15;
        if (target == current_task) {
            task_exit();
            return 0;
        }
        if (current_task && target->mm == current_task->mm)
            task_robust_cleanup(target);
        if (target->clear_child_tid) {
            uint32_t *tid = target->clear_child_tid;
            target->clear_child_tid = NULL;
            __atomic_store_n(tid, 0, __ATOMIC_RELEASE);
            (void)linux_compat_futex(tid, 1, UINT32_MAX, NULL);
        }
        if (current_task && target->mm == current_task->mm)
            ldso_release_thread_tls(target);
        task_release_fd_table(target);
        task_release_mm(target);
        target->state = TASK_TERMINATED;
        return 0;
    }

    if (target->sig_handler[sig] == SIG_IGN) return 0;

    if (current_task)
        target->sig_last_sender_pid = current_task->id;
    unsigned int signal_index = (unsigned int)(sig - 1);
    target->sig_pending.sig[signal_index / 64] |=
        (1ULL << (signal_index % 64));
    return 0;
}

/**
 * 简化版 fork: 克隆当前任务
 * 创建新任务，复制 fd 表和信号处理器，相同的入口函数。
 * 父任务返回子任务 ID，子任务在首次调度时返回 0。
 * @return 父进程返回子进程 ID，子进程返回 0，失败返回 -1
 */
static int task_fork_common(const hbos_linux_clone_context_t *linux_context) {
    if (!current_task) return -1;

    int idx = -1;
    int reuse = 0;
    for (int i = 1; i < task_count; i++) {
        if (task_pool[i].state == TASK_TERMINATED) {
            idx = i;
            reuse = 1;
            break;
        }
    }
    if (idx < 0) {
        if (task_count >= MAX_TASKS) return -1;
        idx = task_count;
    }

    task_t *child = &task_pool[idx];
    task_t *saved_next = child->next;
    memset(child, 0, sizeof(*child));
    child->next = saved_next;
    child->id = next_id++;
    strncpy(child->name, current_task->name, TASK_NAME_MAX);
    child->name[TASK_NAME_MAX - 1] = '\0';
    child->state = TASK_READY;
    child->entry = linux_context ? NULL : current_task->entry;
    child->arg = linux_context ? NULL : current_task->arg;
    child->exit_status = 0;
    child->parent_id = current_task->id;
    child->child_id = 0;
    child->stack_base = (uint64_t)task_stacks[idx];
    child->stack_size = TASK_STACK_SIZE;
    child->fpu_state = task_fpu_state[idx];
    task_fpu_capture(child->fpu_state);
    uint64_t child_pml4 = current_task->mm &&
                          current_task->mm->pml4_phys ?
        vmm_clone_address_space(current_task->mm->pml4_phys) : 0;
    child->mm = task_mm_alloc(
        child_pml4,
        current_task->mm ? current_task->mm->user_heap_start : 0,
        current_task->mm ? current_task->mm->user_heap_limit : 0,
        true);
    if (!child->mm || !child->mm->pml4_phys) {
        if (child->mm)
            task_release_mm(child);
        else if (child_pml4)
            vmm_destroy_address_space(child_pml4);
        child->state = TASK_TERMINATED;
        return -1;
    }
    if (current_task->mm)
        child->mm->user_brk = current_task->mm->user_brk;
    if (current_task->mm &&
        task_copy_vm_areas(child->mm, current_task->mm) < 0) {
        task_release_mm(child);
        child->state = TASK_TERMINATED;
        return -1;
    }
    child->fs_base = task_get_fs_base();
    child->thread_group_id = child->id;
    child->parent_death_signal = current_task->parent_death_signal;
    child->dumpable = current_task->dumpable;
    child->no_new_privs = current_task->no_new_privs;
    child->clear_child_tid = NULL;
    child->robust_list_head = NULL;
    child->robust_list_length = 0;
    memcpy(child->dynamic_tls, current_task->dynamic_tls,
           sizeof(child->dynamic_tls));

    child->fd_table = task_fd_table_alloc();
    if (!child->fd_table) {
        task_release_mm(child);
        child->state = TASK_TERMINATED;
        return -1;
    }
    memcpy(child->fd_table->entries, current_task->fd_table->entries, sizeof(child->fd_table->entries));
    linux_compat_retain_task(child);
    memcpy(child->sig_handler, current_task->sig_handler, sizeof(child->sig_handler));
    memcpy(child->sig_action_flags, current_task->sig_action_flags,
           sizeof(child->sig_action_flags));
    memcpy(child->sig_action_restorer, current_task->sig_action_restorer,
           sizeof(child->sig_action_restorer));
    memcpy(child->sig_action_mask, current_task->sig_action_mask,
           sizeof(child->sig_action_mask));
    memset(&child->sig_pending, 0, sizeof(child->sig_pending));
    child->sig_blocked = current_task->sig_blocked;
    child->sig_exit_code = 0;
    child->sig_last_sender_pid = current_task->sig_last_sender_pid;
    child->sig_frame_active = false;
    child->sig_siginfo_depth = 0;
    child->sig_altstack_sp = current_task->sig_altstack_sp;
    child->sig_altstack_size = current_task->sig_altstack_size;
    child->sig_altstack_flags = current_task->sig_altstack_flags;
    child->sig_suppress_delivery = false;
    /* fork()'d child restarts from the same entry point in a cloned copy
     * of the parent's address space (see vmm_clone_address_space above) --
     * same executable, so the parent's own .symtab/.strtab (if any) is
     * still valid and at the same addresses for the child too. */
    child->host_symtab = current_task->host_symtab;
    child->host_strtab = current_task->host_strtab;
    child->host_symtab_count = current_task->host_symtab_count;

    uint64_t *sp = (uint64_t *)(child->stack_base + child->stack_size);
    void *launch_context;
    uint64_t launch_entry;
    if (linux_context) {
        sp -= sizeof(*linux_context) / sizeof(*sp);
        hbos_linux_clone_context_t *context =
            (hbos_linux_clone_context_t *)sp;
        *context = *linux_context;
        launch_context = context;
        launch_entry = (uint64_t)(uintptr_t)task_resume_linux_syscall;
    } else {
        launch_context = current_task->arg;
        launch_entry = (uint64_t)(uintptr_t)current_task->entry;
    }
    *--sp = launch_entry;
    *--sp = (uint64_t)launch_context;
    *--sp = (uint64_t)task_entry_trampoline;
    *--sp = 0x2;           // RFLAGS (IF enabled by trampoline)
    *--sp = 0;  // RBP
    *--sp = 0;  // RBX
    *--sp = 0;  // R12
    *--sp = 0;  // R13
    *--sp = 0;  // R14
    *--sp = 0;  // R15
    child->rsp = (uint64_t)sp;

    if (!reuse) {
        child->next = task_pool[0].next;
        task_pool[0].next = child;
        task_count++;
    }

    current_task->child_id = child->id;
    return child->id;
}

int task_fork(void) {
    return task_fork_common(NULL);
}

int task_fork_linux(const hbos_linux_clone_context_t *context) {
    if (!context || !context->rip || !context->rsp) return -1;
    return task_fork_common(context);
}

static int task_clone_thread_common(
    const hbos_clone_request_t *request,
    const hbos_linux_clone_context_t *linux_context) {
    /*
     * This is intentionally the pthread-shaped subset of Linux clone().
     * It shares the address space and fd table directly, so context switches
     * do not copy state or involve a manager process.
     */
    enum {
        CLONE_VM = 0x00000100,
        CLONE_FS = 0x00000200,
        CLONE_FILES = 0x00000400,
        CLONE_SIGHAND = 0x00000800,
        CLONE_THREAD = 0x00010000,
        CLONE_SYSVSEM = 0x00040000,
        CLONE_SETTLS = 0x00080000,
        CLONE_PARENT_SETTID = 0x00100000,
        CLONE_CHILD_CLEARTID = 0x00200000,
        CLONE_CHILD_SETTID = 0x01000000
    };
    const uint64_t required = CLONE_VM | CLONE_FILES |
                              CLONE_SIGHAND | CLONE_THREAD;
    const uint64_t supported = required | CLONE_FS | CLONE_SYSVSEM |
                               CLONE_SETTLS | CLONE_PARENT_SETTID |
                               CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID;

    if (!current_task || !request ||
        request->version != HBOS_CLONE_REQUEST_VERSION ||
        request->size != sizeof(*request))
        return -1;
    if ((request->flags & required) != required ||
        (request->flags & ~(supported | 0xffULL)) != 0)
        return -1;
    if (!task_user_address(request->entry) ||
        !task_user_address(request->stack) ||
        request->argument >= 0x0000800000000000ULL)
        return -1;
    if ((request->flags & CLONE_SETTLS) &&
        !task_user_address(request->tls))
        return -1;
    if ((request->flags & CLONE_PARENT_SETTID) &&
        !task_user_address(request->parent_tid))
        return -1;
    if ((request->flags & CLONE_CHILD_SETTID) &&
        !task_user_address(request->child_tid))
        return -1;
    if ((request->flags & CLONE_CHILD_CLEARTID) &&
        !task_user_address(request->clear_child_tid ?
            request->clear_child_tid : request->child_tid))
        return -1;
    if (!current_task->fd_table || !current_task->mm ||
        !current_task->mm->pml4_phys)
        return -1;

    int idx = -1;
    int reuse = 0;
    for (int i = 1; i < task_count; i++) {
        if (task_pool[i].state == TASK_TERMINATED) {
            idx = i;
            reuse = 1;
            break;
        }
    }
    if (idx < 0) {
        if (task_count >= MAX_TASKS) return -1;
        idx = task_count;
    }

    task_t *thread = &task_pool[idx];
    task_t *saved_next = thread->next;
    memset(thread, 0, sizeof(*thread));
    thread->next = saved_next;
    thread->id = next_id++;
    strncpy(thread->name, current_task->name, TASK_NAME_MAX);
    thread->name[TASK_NAME_MAX - 1] = '\0';
    thread->state = TASK_READY;
    thread->parent_id = current_task->id;
    thread->stack_base = (uint64_t)task_stacks[idx];
    thread->stack_size = TASK_STACK_SIZE;
    thread->fpu_state = task_fpu_state[idx];
    task_fpu_capture(thread->fpu_state);

    thread->mm = current_task->mm;
    thread->mm->refs++;
    thread->fs_base = (request->flags & CLONE_SETTLS) ?
                      request->tls : task_get_fs_base();
    thread->thread_group_id = current_task->thread_group_id;
    thread->parent_death_signal = current_task->parent_death_signal;
    thread->dumpable = current_task->dumpable;
    thread->no_new_privs = current_task->no_new_privs;
    thread->clear_child_tid =
        (request->flags & CLONE_CHILD_CLEARTID) ?
        (uint32_t *)(uintptr_t)(request->clear_child_tid ?
            request->clear_child_tid : request->child_tid) : NULL;
    thread->robust_list_head = NULL;
    thread->robust_list_length = 0;

    thread->fd_table = current_task->fd_table;
    thread->fd_table->refs++;
    memcpy(thread->sig_handler, current_task->sig_handler,
           sizeof(thread->sig_handler));
    memcpy(thread->sig_action_flags, current_task->sig_action_flags,
           sizeof(thread->sig_action_flags));
    memcpy(thread->sig_action_restorer, current_task->sig_action_restorer,
           sizeof(thread->sig_action_restorer));
    memcpy(thread->sig_action_mask, current_task->sig_action_mask,
           sizeof(thread->sig_action_mask));
    memset(&thread->sig_pending, 0, sizeof(thread->sig_pending));
    thread->sig_blocked = current_task->sig_blocked;
    thread->sig_exit_code = 0;
    thread->sig_frame_active = false;
    thread->sig_suppress_delivery = false;
    thread->host_symtab = current_task->host_symtab;
    thread->host_strtab = current_task->host_strtab;
    thread->host_symtab_count = current_task->host_symtab_count;

    uint64_t *sp = (uint64_t *)(thread->stack_base + thread->stack_size);
    void *launch_context = NULL;
    void (*launch_entry)(void) = NULL;
    if (linux_context) {
        sp -= sizeof(*linux_context) / sizeof(*sp);
        hbos_linux_clone_context_t *ctx =
            (hbos_linux_clone_context_t *)sp;
        *ctx = *linux_context;
        launch_context = ctx;
        launch_entry = task_resume_linux_syscall;
    } else {
        sp -= sizeof(ring3_launch_ctx_t) / sizeof(*sp);
        ring3_launch_ctx_t *ctx = (ring3_launch_ctx_t *)sp;
        ctx->user_entry = request->entry;
        ctx->user_stack = request->stack;
        ctx->user_argc = request->argument;
        ctx->user_argv = 0;
        launch_context = ctx;
        launch_entry = task_enter_ring3;
    }
    *--sp = (uint64_t)launch_entry;
    *--sp = (uint64_t)launch_context;
    *--sp = (uint64_t)task_entry_trampoline;
    *--sp = 0x2;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    thread->rsp = (uint64_t)sp;

    if (request->flags & CLONE_PARENT_SETTID)
        __atomic_store_n((uint32_t *)(uintptr_t)request->parent_tid,
                         thread->id, __ATOMIC_RELEASE);
    if (request->flags & CLONE_CHILD_SETTID)
        __atomic_store_n((uint32_t *)(uintptr_t)request->child_tid,
                         thread->id, __ATOMIC_RELEASE);

    if (!reuse) {
        thread->next = task_pool[0].next;
        task_pool[0].next = thread;
        task_count++;
    }
    current_task->child_id = thread->id;
    return (int)thread->id;
}

int task_clone_user_thread(const hbos_clone_request_t *request) {
    return task_clone_thread_common(request, NULL);
}

int task_clone_linux_thread(const hbos_clone_request_t *request,
                            const hbos_linux_clone_context_t *context) {
    if (!context || context->rip != request->entry ||
        context->rsp != request->stack)
        return -1;
    return task_clone_thread_common(request, context);
}

/** 获取活跃任务数（不含 TERMINATED） */
int task_get_count(void) {
    int c = 0;
    for (int i = 0; i < task_count; i++) {
        if (task_pool[i].state != TASK_TERMINATED) c++;
    }
    return c;
}

const task_t *task_get_active(uint32_t index) {
    uint32_t visible = 0;
    for (int i = 0; i < task_count; i++) {
        if (task_pool[i].state == TASK_TERMINATED) continue;
        if (visible == index) return &task_pool[i];
        visible++;
    }
    return NULL;
}

/** 列出所有活跃任务到控制台 */
void task_list_all(void) {
    console_puts("\n\x1b[33mTask List\x1b[0m\n");
    console_puts("  \x1b[36mID  Name                 State\x1b[0m\n");
    for (int i = 0; i < task_count; i++) {
        task_t *t = &task_pool[i];
        if (t->state == TASK_TERMINATED) continue;

        // 打印 ID
        char buf[16];
        int bi = 0, id = t->id;
        do { buf[bi++] = '0' + (id % 10); id /= 10; } while (id);
        console_puts("  ");
        for (int j = bi - 1; j >= 0; j--) console_putchar(buf[j]);
        console_puts("  ");

        // 打印名称
        console_puts(t->name);
        int pad = 20 - (int)strlen(t->name);
        for (int p = 0; p < pad; p++) console_putchar(' ');

        // 打印状态
        const char *state_str;
        switch (t->state) {
            case TASK_READY:      state_str = "READY"; break;
            case TASK_RUNNING:    state_str = "RUNNING"; break;
            case TASK_BLOCKED:    state_str = "BLOCKED"; break;
            case TASK_TERMINATED: state_str = "TERMINATED"; break;
            default:              state_str = "UNKNOWN"; break;
        }
        console_puts(state_str);
        console_putchar('\n');
    }
    console_puts("\n");
}

static volatile int preempt_count = 0;

void task_preempt_disable(void) {
    preempt_count++;
}

void task_preempt_enable(void) {
    if (preempt_count > 0) preempt_count--;
}

void task_schedule(void) {
    if (preempt_count > 0) return;
    if (!current_task || current_task->state == TASK_TERMINATED) return;
    uint64_t irq_flags = task_irq_save();
    smp_sched_lock();
    if (current_task->state == TASK_RUNNING)
        current_task->state = TASK_READY;
    task_t *next = current_task->next;
    int checked = 0;
    while (next->state != TASK_READY && checked < task_count) {
        next = next->next;
        checked++;
    }
    if (next->state != TASK_READY) {
        smp_sched_unlock();
        task_irq_restore(irq_flags);
        return;
    }
    next->state = TASK_RUNNING;
    task_t *prev = current_task;
    current_task = next;
    if (next->mm && next->mm->pml4_phys)
        vmm_set_pml4(next->mm->pml4_phys);
    task_reap_address_spaces();
    tss_set_stack(next->stack_base + next->stack_size);
    task_switch_tls(prev, next);
    task_sig_deliver(next);
    task_switch(&prev->rsp, &next->rsp, prev->fpu_state, next->fpu_state);
    smp_sched_unlock();
    task_irq_restore(irq_flags);
}

static volatile uint32_t pit_frequency_hz;

uint32_t pit_get_frequency_hz(void) {
    return pit_frequency_hz;
}

uint64_t pit_ticks_from_ms(uint32_t ms) {
    uint32_t frequency = pit_frequency_hz;
    if (!frequency || !ms) return 0;
    return ((uint64_t)ms * frequency + 999U) / 1000U;
}

void pit_init(uint32_t freq_hz) {
    if (!freq_hz) freq_hz = PIT_DEFAULT_FREQUENCY_HZ;
    uint32_t divisor = 1193182 / freq_hz;
    if (divisor < 1) divisor = 1;
    if (divisor > 65535) divisor = 65535;
    pit_frequency_hz = 1193182U / divisor;
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x36), "Nd"(0x43));
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)(divisor & 0xFF)), "Nd"(0x40));
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)((divisor >> 8) & 0xFF)), "Nd"(0x40));
}

void task_sig_deliver(task_t *task) {
    if (!task) return;
    for (int sig = 1; sig < _NSIG; sig++) {
        int word = (sig - 1) / 64;
        int bit = (sig - 1) % 64;
        if (!(task->sig_pending.sig[word] & (1ULL << bit))) continue;
        if (task->sig_blocked.sig[word] & (1ULL << bit)) continue;

        if (sig == SIGKILL || sig == SIGSTOP || sig == SIGCONT) {
            task->sig_pending.sig[word] &= ~(1ULL << bit);
            if (sig == SIGKILL) {
                task->sig_exit_code = 9;
                task->state = TASK_TERMINATED;
            }
            return;
        }

        void (*handler)(int) = task->sig_handler[sig];
        if (handler == SIG_DFL || handler == NULL) {
            task->sig_pending.sig[word] &= ~(1ULL << bit);
            if (sig == SIGINT || sig == SIGTERM || sig == SIGQUIT ||
                sig == SIGILL || sig == SIGSEGV || sig == SIGFPE ||
                sig == SIGBUS || sig == SIGABRT || sig == SIGPIPE) {
                task->sig_exit_code = sig;
                task->state = TASK_TERMINATED;
            }
            return;
        } else if (handler == SIG_IGN) {
            task->sig_pending.sig[word] &= ~(1ULL << bit);
            return;
        } else {
            /* A user handler must never be called at CPL0.  Leave it pending;
             * linux_signal_prepare_return() builds the ring3 return frame at
             * the next native syscall boundary. */
            return;
        }
    }
}
