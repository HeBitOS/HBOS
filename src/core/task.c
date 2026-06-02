/**
 * @file    task.c
 * @brief   协作式多任务调度器 — 轮转调度 (Round-Robin)
 *
 * 实现一个简单的协作式多任务系统:
 *   - 最多 16 个任务 (MAX_TASKS)
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
#include "../smp.h"

static void task_sig_deliver(task_t *task);
#include "../signal.h"
#include "../string.h"
#include "../graphics/graphics.h"

// ============================================================
// 外部汇编原语
// ============================================================

/** 上下文切换: 保存当前 RSP，加载下一个 RSP */
extern void task_switch(uint64_t *prev_rsp, uint64_t *next_rsp);

/** 新任务入口蹦床: pop arg → pop entry → call entry(arg) → task_exit() */
extern void task_entry_trampoline(void);

/** Ring3 入口蹦床: 构建 iretq 帧并切换到 ring3 */
extern void task_enter_ring3(void);

typedef struct {
    uint64_t user_entry;
    uint64_t user_stack;
} ring3_launch_ctx_t;

// ============================================================
// 内部状态
// ============================================================

static task_t task_pool[MAX_TASKS];          /**< 任务池（静态分配） */
static int task_count = 0;                   /**< 当前任务数 */
static task_t *current_task = NULL;          /**< 当前运行的任务 */
static uint32_t next_id = 0;                 /**< 下一个任务 ID */

/** 预分配的栈空间: 16 个任务 × 8KB = 128KB */
static char task_stacks[MAX_TASKS][TASK_STACK_SIZE]
    __attribute__((aligned(16)));

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
    main_task->vm_areas = NULL;
    main_task->pml4_phys = vmm_get_pml4();
    memset(main_task->fds, 0, sizeof(main_task->fds));
    memset(main_task->sig_handler, 0, sizeof(main_task->sig_handler));
    memset(&main_task->sig_pending, 0, sizeof(main_task->sig_pending));
    memset(&main_task->sig_blocked, 0, sizeof(main_task->sig_blocked));
    main_task->sig_exit_code = 0;

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
    tcb->vm_areas = NULL;
    tcb->pml4_phys = vmm_create_address_space();
    memset(tcb->fds, 0, sizeof(tcb->fds));
    memset(tcb->sig_handler, 0, sizeof(tcb->sig_handler));
    memset(&tcb->sig_pending, 0, sizeof(tcb->sig_pending));
    memset(&tcb->sig_blocked, 0, sizeof(tcb->sig_blocked));
    tcb->sig_exit_code = 0;

    // ---- 构建初始栈帧 ----
    // 从栈顶向下填充（栈向低地址增长）
    uint64_t *sp = (uint64_t *)(tcb->stack_base + tcb->stack_size);

    *--sp = (uint64_t)entry;                   // 最高地址槽位
    *--sp = (uint64_t)arg;
    *--sp = (uint64_t)task_entry_trampoline;   // ret 跳转目标
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
int task_create_ring3(const char *name, uint64_t user_entry, uint64_t user_stack) {
    if (!user_entry || !user_stack) return -1;

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
    tcb->vm_areas = NULL;
    tcb->pml4_phys = vmm_create_address_space();
    memset(tcb->fds, 0, sizeof(tcb->fds));
    memset(tcb->sig_handler, 0, sizeof(tcb->sig_handler));
    memset(&tcb->sig_pending, 0, sizeof(tcb->sig_pending));
    memset(&tcb->sig_blocked, 0, sizeof(tcb->sig_blocked));
    tcb->sig_exit_code = 0;

    // Allocate ring3_launch_ctx_t on the task's kernel stack
    uint64_t *sp = (uint64_t *)(tcb->stack_base + tcb->stack_size);

    // Place ctx struct on stack (aligned)
    sp -= 2; // 2 uint64_t for ring3_launch_ctx_t
    ring3_launch_ctx_t *ctx = (ring3_launch_ctx_t *)sp;
    ctx->user_entry = user_entry;
    ctx->user_stack = user_stack;

    // Build the trampoline frame
    *--sp = (uint64_t)ctx;                       // arg → rdi
    *--sp = (uint64_t)task_enter_ring3;          // entry → rax
    *--sp = (uint64_t)task_entry_trampoline;     // ret target
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

/**
 * 让出 CPU — 协作式调度入口
 * 当前任务状态变为 READY，切换到下一个 READY 任务
 */
void task_yield(void) {
    smp_sched_lock();
    task_t *prev = current_task;
    task_t *next = sched_next();

    if (!next || next == prev) {
        smp_sched_unlock();
        return;
    }

    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    current_task = next;

    if (next->pml4_phys) vmm_set_pml4(next->pml4_phys);

    task_sig_deliver(next);

    task_switch(&prev->rsp, &next->rsp);
    smp_sched_unlock();
}

/**
 * 终止当前任务
 * 标记为 TERMINATED，然后切换到下一个 READY 任务。
 * 如果没有其他任务，系统停机。
 */
void task_exit(void) {
    if (!current_task) return;
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
        if (next->pml4_phys) vmm_set_pml4(next->pml4_phys);
        task_switch(&prev->rsp, &next->rsp);
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

    if (sig == SIGKILL || sig == SIGTERM) {
        target->sig_exit_code = (sig == SIGKILL) ? 9 : 15;
        if (target == current_task) {
            task_exit();
            return 0;
        }
        target->state = TASK_TERMINATED;
        return 0;
    }

    target->sig_pending.sig[sig / 64] |= (1ULL << (sig % 64));
    return 0;
}

/**
 * 简化版 fork: 克隆当前任务
 * 创建新任务，复制 fd 表和信号处理器，相同的入口函数。
 * 父任务返回子任务 ID，子任务在首次调度时返回 0。
 * @return 父进程返回子进程 ID，子进程返回 0，失败返回 -1
 */
int task_fork(void) {
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
    child->id = next_id++;
    strncpy(child->name, current_task->name, TASK_NAME_MAX);
    child->name[TASK_NAME_MAX - 1] = '\0';
    child->state = TASK_READY;
    child->entry = current_task->entry;
    child->arg = current_task->arg;
    child->exit_status = 0;
    child->parent_id = current_task->id;
    child->child_id = 0;
    child->stack_base = (uint64_t)task_stacks[idx];
    child->stack_size = TASK_STACK_SIZE;
    child->vm_areas = NULL;

    child->pml4_phys = current_task->pml4_phys ?
        vmm_clone_address_space(current_task->pml4_phys) : 0;

    memcpy(child->fds, current_task->fds, sizeof(child->fds));
    memcpy(child->sig_handler, current_task->sig_handler, sizeof(child->sig_handler));
    memset(&child->sig_pending, 0, sizeof(child->sig_pending));
    memset(&child->sig_blocked, 0, sizeof(child->sig_blocked));
    child->sig_exit_code = 0;

    uint64_t *sp = (uint64_t *)(child->stack_base + child->stack_size);
    *--sp = (uint64_t)current_task->entry;
    *--sp = (uint64_t)current_task->arg;
    *--sp = (uint64_t)task_entry_trampoline;
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
        return;
    }
    next->state = TASK_RUNNING;
    task_t *prev = current_task;
    current_task = next;
    if (next->pml4_phys) vmm_set_pml4(next->pml4_phys);
    task_sig_deliver(next);
    task_switch(&prev->rsp, &next->rsp);
    smp_sched_unlock();
}

void pit_init(uint32_t freq_hz) {
    uint32_t divisor = 1193182 / freq_hz;
    if (divisor < 1) divisor = 1;
    if (divisor > 65535) divisor = 65535;
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x36), "Nd"(0x43));
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)(divisor & 0xFF)), "Nd"(0x40));
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)((divisor >> 8) & 0xFF)), "Nd"(0x40));
}

void task_sig_deliver(task_t *task) {
    if (!task) return;
    for (int sig = 1; sig < _NSIG; sig++) {
        int word = sig / 64;
        int bit = sig % 64;
        if (!(task->sig_pending.sig[word] & (1ULL << bit))) continue;
        if (task->sig_blocked.sig[word] & (1ULL << bit)) continue;

        task->sig_pending.sig[word] &= ~(1ULL << bit);

        if (sig == SIGKILL || sig == SIGSTOP || sig == SIGCONT) {
            if (sig == SIGKILL) {
                task->sig_exit_code = 9;
                task->state = TASK_TERMINATED;
            }
            return;
        }

        void (*handler)(int) = task->sig_handler[sig];
        if (handler == SIG_DFL || handler == NULL) {
            if (sig == SIGINT || sig == SIGTERM || sig == SIGQUIT ||
                sig == SIGILL || sig == SIGSEGV || sig == SIGFPE ||
                sig == SIGBUS || sig == SIGABRT || sig == SIGPIPE) {
                task->sig_exit_code = sig;
                task->state = TASK_TERMINATED;
            }
            return;
        } else if (handler == SIG_IGN) {
            return;
        } else {
            handler(sig);
        }
    }
}
