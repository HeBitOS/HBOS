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
#include "../string.h"
#include "../graphics/graphics.h"

// ============================================================
// 外部汇编原语
// ============================================================

/** 上下文切换: 保存当前 RSP，加载下一个 RSP */
extern void task_switch(uint64_t *prev_rsp, uint64_t *next_rsp);

/** 新任务入口蹦床: pop arg → pop entry → call entry(arg) → task_exit() */
extern void task_entry_trampoline(void);

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
// 内部辅助函数
// ============================================================

/** 安全字符串复制（限制最大长度） */
static void t_strncpy(char *dst, const char *src, int max_len) {
    if (max_len <= 0) return;
    int i = 0;
    if (src) {
        while (src[i] && i < max_len - 1) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

/** 字符串长度 */
static int t_strlen(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
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
    t_strncpy(main_task->name, "main", TASK_NAME_MAX);
    main_task->state = TASK_RUNNING;
    main_task->rsp = 0;  // 首次 task_yield 时保存
    main_task->entry = NULL;
    main_task->arg = NULL;
    main_task->next = main_task;  // 循环链表（单元素）
    main_task->stack_base = (uint64_t)task_stacks[0];
    main_task->stack_size = TASK_STACK_SIZE;
    memset(main_task->fds, 0, sizeof(main_task->fds));

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
    if (task_count >= MAX_TASKS) return -1;
    if (!entry) return -1;

    int idx = task_count;
    task_t *tcb = &task_pool[idx];

    tcb->id = next_id++;
    t_strncpy(tcb->name, name ? name : "task", TASK_NAME_MAX);
    tcb->state = TASK_READY;
    tcb->entry = entry;
    tcb->arg = arg;
    tcb->stack_base = (uint64_t)task_stacks[idx];
    tcb->stack_size = TASK_STACK_SIZE;
    memset(tcb->fds, 0, sizeof(tcb->fds));

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

    // 插入循环链表（在 head 之后）
    tcb->next = task_pool[0].next;
    task_pool[0].next = tcb;

    task_count++;
    return tcb->id;
}

/**
 * 让出 CPU — 协作式调度入口
 * 当前任务状态变为 READY，切换到下一个 READY 任务
 */
void task_yield(void) {
    task_t *prev = current_task;
    task_t *next = sched_next();

    // 没有其他可运行任务 — 继续当前任务
    if (!next || next == prev) return;

    // 更新状态
    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    current_task = next;

    // 上下文切换 — 保存 prev 的 RSP，恢复 next 的 RSP
    task_switch(&prev->rsp, &next->rsp);
    // 执行在此恢复（当此任务被重新调度时）
}

/**
 * 终止当前任务
 * 标记为 TERMINATED，然后切换到下一个 READY 任务。
 * 如果没有其他任务，系统停机。
 */
void task_exit(void) {
    if (!current_task) return;
    current_task->state = TASK_TERMINATED;

    // 持续尝试切换到其他 READY 任务
    while (1) {
        task_t *next = sched_next();
        if (!next) {
            // 没有更多任务 — 系统空闲
            console_puts("\n\x1b[31m[KERN] All tasks terminated, halting.\x1b[0m\n");
            while (1) __asm__ volatile("cli; hlt");
        }
        next->state = TASK_RUNNING;
        task_t *prev = current_task;
        current_task = next;
        task_switch(&prev->rsp, &next->rsp);
        // 如果回到这里，再试一次
    }
}

/** 获取当前任务 ID */
uint32_t task_get_id(void) {
    return current_task ? current_task->id : 0;
}

/** 获取当前任务 TCB 指针 */
task_t *task_current(void) {
    return current_task;
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
        int pad = 20 - t_strlen(t->name);
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
