#ifndef HBOS_TASK_H
#define HBOS_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "../fd.h"

// ============================================================
// Cooperative Multitasking / Threading Framework
// ============================================================

#define TASK_NAME_MAX 32
#define TASK_STACK_SIZE 8192
#define MAX_TASKS 16

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

    // Link for round-robin list
    struct task *next;

    fd_entry_t fds[POSIX_MAX_FDS];
} task_t;

// ============================================================
// API
// ============================================================

// Initialize the task scheduler
void task_init(void);

// Create a new task with given name, entry function, and argument
// Returns task ID, or -1 on failure
int task_create(const char *name, void (*entry)(void *), void *arg);

// Cooperative yield — switch to next ready task
void task_yield(void);

// Terminate the current task
void task_exit(void);

// Get current task ID
uint32_t task_get_id(void);

// Get current task object
task_t *task_current(void);

// Get number of active (non-terminated) tasks
int task_get_count(void);

// Get active task by visible index, or NULL when out of range
const task_t *task_get_active(uint32_t index);

// List all tasks (for debug)
void task_list_all(void);

#endif /* HBOS_TASK_H */
