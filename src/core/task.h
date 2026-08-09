#ifndef HBOS_TASK_H
#define HBOS_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../fd.h"
#include "../signal.h"

#define VM_BACKING_ANONYMOUS   0U
#define VM_BACKING_MEMFD       1U
#define VM_BACKING_VFS_PRIVATE 2U
#define VM_BACKING_VFS_SHARED  3U

typedef struct vm_area {
    uint64_t start;
    uint64_t end;
    uint32_t backing_type;
    uint32_t backing_id;
    uint64_t backing_offset;
    vfs_node_t *backing_node;
    struct vm_area *next;
} vm_area_t;

typedef struct {
    uint32_t refs;
    bool owns_pml4;
    vm_area_t *areas;
    uint64_t pml4_phys;
    uint64_t user_heap_start;
    uint64_t user_brk;
    uint64_t user_heap_limit;
} task_mm_t;

/*
 * Versioned userspace request for the lightweight clone fast path.  Fixed
 * width fields keep the ABI stable and let the kernel reject newer layouts
 * instead of silently interpreting them incorrectly.
 */
#define HBOS_CLONE_REQUEST_VERSION 1U

typedef struct {
    uint32_t version;
    uint32_t size;
    uint64_t flags;
    uint64_t entry;
    uint64_t stack;
    uint64_t argument;
    uint64_t tls;
    uint64_t parent_tid;
    uint64_t child_tid;
    uint64_t clear_child_tid;
} hbos_clone_request_t;

/* Kernel-internal snapshot used by the native Linux clone3 resume path. */
typedef struct {
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r10;
    uint64_t r8;
    uint64_t r9;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} hbos_linux_clone_context_t;

// ============================================================
// Cooperative Multitasking / Threading Framework
// ============================================================

#define TASK_NAME_MAX 32
#define TASK_STACK_SIZE 8192
#define MAX_TASKS 64
#define PIT_DEFAULT_FREQUENCY_HZ 100U
#define TASK_USER_HEAP_START 0x0000002000000000ULL
#define TASK_USER_HEAP_SIZE  (64ULL * 1024 * 1024)
#define TASK_DYNAMIC_TLS_MAX 64

typedef struct {
    uint32_t module_id;
    uint32_t page_count;
    uint64_t address;
} task_dynamic_tls_t;

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

    // FXSAVE/FXRSTOR area (512 bytes, 16-byte aligned) — x87/MMX/SSE state,
    // saved/restored on every task_switch(). Points into task_fpu_state[]
    // (task.c), sized/initialized at task creation.
    void *fpu_state;

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

    fd_table_t *fd_table;

    task_mm_t *mm;
    uint64_t fs_base;
    uint32_t thread_group_id;
    uint8_t parent_death_signal;
    bool dumpable;
    bool no_new_privs;
    uint32_t *clear_child_tid;
    void *robust_list_head;
    size_t robust_list_length;
    task_dynamic_tls_t dynamic_tls[TASK_DYNAMIC_TLS_MAX];

    void (*sig_handler[_NSIG])(int);
    uint64_t sig_action_flags[_NSIG];
    uint64_t sig_action_restorer[_NSIG];
    uint64_t sig_action_mask[_NSIG];
    sigset_t sig_pending;
    sigset_t sig_blocked;
    int sig_exit_code;
    /* Most recent kill(2)/raise(2) sender, reported as siginfo.si_pid for
     * SI_USER deliveries.  Not per-signal; adequate for the current
     * single-slot pending model. */
    uint32_t sig_last_sender_pid;

    /* One active Linux x86-64 signal frame (legacy single-argument path).
     * Nested delivery remains pending until rt_sigreturn restores the
     * interrupted syscall or synchronous exception frame. */
    bool sig_frame_active;
    /* SA_SIGINFO frames keep their interrupted context in the user-space
     * ucontext, so nesting is allowed up to this depth.  The legacy path
     * refuses delivery while an SA_SIGINFO frame is active and vice versa,
     * keeping the two restore conventions from interleaving. */
    int sig_siginfo_depth;
    /* sigaltstack(2) state.  flags is 0 (enabled) or SS_DISABLE; SS_ONSTACK
     * is never stored here, it is derived from the delivery rsp. */
    uint64_t sig_altstack_sp;
    uint64_t sig_altstack_size;
    uint64_t sig_altstack_flags;
    bool sig_suppress_delivery;
    uint64_t sig_saved_rax;
    uint64_t sig_saved_args[6];
    uint64_t sig_saved_callee[6];
    uint64_t sig_saved_rcx;
    uint64_t sig_saved_r11;
    uint64_t sig_saved_rip;
    uint64_t sig_saved_rflags;
    uint64_t sig_saved_rsp;
    sigset_t sig_saved_blocked;

    // This task's own executable's .symtab/.strtab (copied out of the ELF
    // buffer at elf64_load_and_spawn() time, since that buffer gets freed
    // right after loading — see src/elf.c). Lets a dlopen()'d shared
    // library's unresolved external symbols (printf, malloc, ...) resolve
    // back into this program's own statically-linked libc instead of only
    // ever finding symbols in other loaded shared libraries — see
    // src/user/ldso.c's host-symbol-table fallback. NULL/0 if the
    // executable had no .symtab (TCC only emits one when asked — see
    // third_party/tinycc/tcc.c's `s->do_debug` patch).
    void     *host_symtab;
    char     *host_strtab;
    uint64_t  host_symtab_count;
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
/* A nonzero pml4_phys is transferred to task_create_ring3_full, including
 * failure paths; callers must not destroy it after the call. */

// Cooperative yield — switch to next ready task
void task_yield(void);

// Terminate the current task
void task_exit(void);
void task_set_exit_status(int status);
int task_wait(uint32_t id, int *status);
int task_kill(uint32_t id, int sig);

// Fork current task (clone with same context)
int task_fork(void);
int task_fork_linux(const hbos_linux_clone_context_t *context);
int task_clone_user_thread(const hbos_clone_request_t *request);
int task_clone_linux_thread(const hbos_clone_request_t *request,
                            const hbos_linux_clone_context_t *context);
int task_set_robust_list(void *head, size_t length);
int task_get_robust_list(int tid, void **head, size_t *length);
int task_set_tid_address(uint32_t *address);

// Get current task ID
uint32_t task_get_id(void);
uint32_t task_get_process_id(void);
uint64_t task_get_fs_base(void);
int task_set_fs_base(uint64_t base);

// Get current task object
task_t *task_current(void);
const task_t *task_get_by_id(uint32_t id);

// Attach a copy of this program's own .symtab/.strtab (see src/elf.c's
// elf64_load_and_spawn) so a later dlopen()'d shared library can resolve
// symbols back into it — see src/user/ldso.c.
void task_set_host_symtab(uint32_t id, void *symtab, char *strtab, uint64_t count);

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
uint64_t pit_get_ticks(void);
uint32_t pit_get_frequency_hz(void);
uint64_t pit_ticks_from_ms(uint32_t ms);

#endif /* HBOS_TASK_H */
