#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "task.h"
#include "../string.h"
#include "../graphics/graphics.h"

// ============================================================
// External assembly context-switch primitive
// ============================================================
extern void task_switch(uint64_t *prev_rsp, uint64_t *next_rsp);
extern void task_entry_trampoline(void);

// ============================================================
// Internal state
// ============================================================
static task_t task_pool[MAX_TASKS];
static int task_count = 0;
static task_t *current_task = NULL;
static uint32_t next_id = 0;

// Pre-allocated stacks in BSS (16 tasks × 8 KB = 128 KB)
static char task_stacks[MAX_TASKS][TASK_STACK_SIZE]
    __attribute__((aligned(16)));

// ============================================================
// Internal helpers
// ============================================================
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

static int t_strlen(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

// ============================================================
// Round-robin scheduler: find next READY task
// ============================================================
static task_t *sched_next(void) {
    if (task_count <= 1) return NULL;

    task_t *start = current_task;
    task_t *t = current_task;

    // Scan circularly for the next READY task
    do {
        t = t->next;
        if (!t) t = &task_pool[0];
        if (t->state == TASK_READY) return t;
    } while (t != start);

    return NULL; // no other ready task
}

// ============================================================
// Public API
// ============================================================

void task_init(void) {
    // Create the main task (task 0) — the initial execution context
    task_t *main_task = &task_pool[0];
    main_task->id = next_id++;
    t_strncpy(main_task->name, "main", TASK_NAME_MAX);
    main_task->state = TASK_RUNNING;
    main_task->rsp = 0; // saved on first yield
    main_task->entry = NULL;
    main_task->arg = NULL;
    main_task->next = main_task; // circular list (single element)
    main_task->stack_base = (uint64_t)task_stacks[0];
    main_task->stack_size = TASK_STACK_SIZE;
    memset(main_task->fds, 0, sizeof(main_task->fds));

    current_task = main_task;
    task_count = 1;
}

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

    // ---- Set up initial stack frame ----
    // task_switch pushes: rbp, rbx, r12, r13, r14, r15 (in that order)
    // task_switch pops:  r15, r14, r13, r12, rbx, rbp (reverse order, LIFO)
    // After popping 6 regs, `ret` pops the return address.
    // The trampoline then pops: arg->rdi, entry->rax, then calls entry(arg).
    //
    // We build the stack from TOP down using *--sp:
    //   first *--sp = highest address, last *--sp = lowest address = final RSP.
    // Memory layout from low to high (popped order):
    //   [RSP+0]  = R15              (popped 1st by task_switch)
    //   [RSP+8]  = R14              (popped 2nd)
    //   [RSP+16] = R13              (popped 3rd)
    //   [RSP+24] = R12              (popped 4th)
    //   [RSP+32] = RBX              (popped 5th)
    //   [RSP+40] = RBP              (popped 6th)
    //   [RSP+48] = trampoline addr  (popped by ret)
    //   [RSP+56] = arg              (popped by trampoline -> rdi)
    //   [RSP+64] = entry            (popped by trampoline -> rax)
    uint64_t *sp = (uint64_t *)(tcb->stack_base + tcb->stack_size);

    // Start from highest address, work downward
    *--sp = (uint64_t)entry;                   // HIGHEST slot (RSP+64 after restore)
    *--sp = (uint64_t)arg;                     // RSP+56
    *--sp = (uint64_t)task_entry_trampoline;   // RSP+48  (return address for ret)
    *--sp = 0;                                 // RSP+40  RBP
    *--sp = 0;                                 // RSP+32  RBX
    *--sp = 0;                                 // RSP+24  R12
    *--sp = 0;                                 // RSP+16  R13
    *--sp = 0;                                 // RSP+8   R14
    *--sp = 0;                                 // RSP+0   R15  (LOWEST — final RSP)

    tcb->rsp = (uint64_t)sp;

    // Insert into circular list (after head)
    tcb->next = task_pool[0].next;
    task_pool[0].next = tcb;

    task_count++;
    return tcb->id;
}

void task_yield(void) {
    task_t *prev = current_task;
    task_t *next = sched_next();

    // No other ready task — stay in current
    if (!next || next == prev) return;

    // Update states
    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    current_task = next;

    // Context switch — saves prev's RSP, restores next's RSP
    task_switch(&prev->rsp, &next->rsp);
    // Execution resumes here when this task is re-scheduled
}

void task_exit(void) {
    if (!current_task) return;
    current_task->state = TASK_TERMINATED;

    // Keep trying to switch to another ready task
    while (1) {
        task_t *next = sched_next();
        if (!next) {
            // No more tasks — system idle
            console_puts("\n\x1b[31m[KERN] All tasks terminated, halting.\x1b[0m\n");
            while (1) __asm__ volatile("cli; hlt");
        }
        next->state = TASK_RUNNING;
        task_t *prev = current_task;
        current_task = next;
        task_switch(&prev->rsp, &next->rsp);
        // If we come back here, try again
    }
}

uint32_t task_get_id(void) {
    return current_task ? current_task->id : 0;
}

task_t *task_current(void) {
    return current_task;
}

int task_get_count(void) {
    int c = 0;
    for (int i = 0; i < task_count; i++) {
        if (task_pool[i].state != TASK_TERMINATED) c++;
    }
    return c;
}

void task_list_all(void) {
    console_puts("\n\x1b[33mTask List\x1b[0m\n");
    console_puts("  \x1b[36mID  Name                 State\x1b[0m\n");
    for (int i = 0; i < task_count; i++) {
        task_t *t = &task_pool[i];
        if (t->state == TASK_TERMINATED) continue;

        // Print ID
        char buf[16];
        int bi = 0, id = t->id;
        do { buf[bi++] = '0' + (id % 10); id /= 10; } while (id);
        console_puts("  ");
        for (int j = bi - 1; j >= 0; j--) console_putchar(buf[j]);
        console_puts("  ");

        // Print name
        console_puts(t->name);
        int pad = 20 - t_strlen(t->name);
        for (int p = 0; p < pad; p++) console_putchar(' ');

        // Print state
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
