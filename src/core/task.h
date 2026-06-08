#ifndef HBOS_TASK_H
#define HBOS_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "../fd.h"
#include "../signal.h"

typedef struct vm_area {
    uint64_t start;
    uint64_t end;
    struct vm_area *next;
} vm_area_t;

// ============================================================
// Cooperative Multitasking / Threading Framework
// ============================================================

#define TASK_NAME_MAX 32
#define TASK_STACK_SIZE 8192
#define MAX_TASKS 16
#define TASK_USER_HEAP_START 0x0000002000000000ULL
#define TASK_USER_HEAP_SIZE  (64ULL * 1024 * 1024)

typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_TERMINATED
} task_state_t;

// Task Control Block
typedef struct task {
    uint32_t id;
    char name[TASK_NAME_MAX];
    task_state_t state;

    // Saved stack pointer (RSP) — when task yields
    uint64_t rsp;

    // Task's dedicated stack
    uint64_t stack_base;
    uint64_t stack_size;

    // Entry point
    void (*entry)(void *arg);
    void *arg;
    int exit_status;
    uint32_t parent_id;
    uint32_t child_id;

    // Link for round-robin list
    struct task *next;

    fd_entry_t fds[POSIX_MAX_FDS];

    vm_area_t *vm_areas;

    uint64_t pml4_phys;
    uint64_t user_heap_start;
    uint64_t user_brk;
    uint64_t user_heap_limit;

    void (*sig_handler[_NSIG])(int);
    sigset_t sig_pending;
    sigset_t sig_blocked;
    int sig_exit_code;
} task_t;

// ============================================================
// API
// ============================================================

// Initialize the task scheduler
void task_init(void);

// Create a new task with given name, entry function, and argument
// Returns task ID, or -1 on failure
int task_create(const char *name, void (*entry)(void *), void *arg);

// Create a ring3 user task. The task will enter ring3 via iretq.
// user_entry: virtual address of user code entry point
// user_stack: initial user stack pointer (top of stack)
// Returns task ID, or -1 on failure
int task_create_ring3(const char *name, uint64_t user_entry, uint64_t user_stack);
int task_create_ring3_as(const char *name, uint64_t user_entry,
                         uint64_t user_stack, uint64_t pml4_phys);
int task_create_ring3_full(const char *name, uint64_t user_entry,
                           uint64_t user_stack, uint64_t user_argc,
                           uint64_t user_argv, uint64_t pml4_phys);

// Cooperative yield — switch to next ready task
void task_yield(void);

// Terminate the current task
void task_exit(void);
void task_set_exit_status(int status);
int task_wait(uint32_t id, int *status);
int task_kill(uint32_t id, int sig);

// Fork current task (clone with same context)
int task_fork(void);

// Get current task ID
uint32_t task_get_id(void);

// Get current task object
task_t *task_current(void);
const task_t *task_get_by_id(uint32_t id);

// Get number of active (non-terminated) tasks
int task_get_count(void);

// Get active task by visible index, or NULL when out of range
const task_t *task_get_active(uint32_t index);

// List all tasks (for debug)
void task_list_all(void);

// Preemptive scheduling
void task_schedule(void);
void task_preempt_enable(void);
void task_preempt_disable(void);
void pit_init(uint32_t freq_hz);

#endif /* HBOS_TASK_H */
