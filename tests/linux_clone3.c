#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

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

typedef struct {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
    uint64_t set_tid;
    uint64_t set_tid_size;
    uint64_t cgroup;
} clone_args_t;

extern long hbos_test_clone3(void *args, unsigned long size,
                             int (*child)(void *), void *argument);

static unsigned char child_stack[16384] __attribute__((aligned(4096)));
static unsigned char child_tls[64] __attribute__((aligned(16)));
static volatile uint32_t parent_tid;
static volatile uint32_t child_tid;
static volatile int child_observed_tid;
static volatile uint64_t child_observed_fs;
static volatile int child_value;

static int child_main(void *opaque) {
    child_value = *(int *)opaque;
    child_observed_tid = (int)syscall(SYS_gettid);
    if (syscall(SYS_arch_prctl, 0x1003L,
                (long)&child_observed_fs) < 0)
        return 2;
    return 0;
}

int main(void) {
    int expected = 0x435;
    clone_args_t args = {
        .flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                 CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
                 CLONE_PARENT_SETTID | CLONE_CHILD_SETTID |
                 CLONE_CHILD_CLEARTID,
        .child_tid = (uint64_t)(uintptr_t)&child_tid,
        .parent_tid = (uint64_t)(uintptr_t)&parent_tid,
        .stack = (uint64_t)(uintptr_t)child_stack,
        .stack_size = sizeof(child_stack),
        .tls = (uint64_t)(uintptr_t)child_tls
    };
    long tid = hbos_test_clone3(&args, sizeof(args), child_main, &expected);
    if (tid <= 0) {
        puts("LINUX_CLONE3: create failed");
        return 1;
    }
    while (child_tid != 0)
        (void)syscall(SYS_futex, (long)&child_tid, 9, tid,
                      0L, 0L, 0x40000000L);
    if (parent_tid != (uint32_t)tid || child_observed_tid != (int)tid ||
        child_value != expected ||
        child_observed_fs != (uint64_t)(uintptr_t)child_tls) {
        puts("LINUX_CLONE3: state failed");
        return 2;
    }

    unsigned char oversized[96] = {0};
    oversized[88] = 1;
    if (hbos_test_clone3(&args, 63, child_main, &expected) != -22 ||
        hbos_test_clone3(oversized, sizeof(oversized),
                         child_main, &expected) != -7) {
        puts("LINUX_CLONE3: validation failed");
        return 3;
    }
    puts("LINUX_CLONE3: PASS");
    return 0;
}
