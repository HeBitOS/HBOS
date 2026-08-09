#include <dlfcn.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef int (*value_fn_t)(void);
typedef void (*set_fn_t)(int);

static int events[16];
static int event_count;
static unsigned char tls_thread_stack[16384] __attribute__((aligned(16)));
static unsigned char tls_thread_tcb[64] __attribute__((aligned(16)));
static volatile int tls_child_tid;
static volatile int tls_child_initial;
static volatile int tls_child_bumped;
static volatile int tls_child_zero;
static volatile int tls_child_zero_bumped;

#define DL_WORKERS 4
#define DL_ITERATIONS 64

static unsigned char dl_thread_stacks[DL_WORKERS][16384]
    __attribute__((aligned(16)));
static unsigned char dl_thread_tcbs[DL_WORKERS][64]
    __attribute__((aligned(16)));
static volatile int dl_child_tids[DL_WORKERS];
static volatile int dl_workers_ready;
static volatile int dl_worker_failures;
static void *dl_worker_handles[DL_WORKERS];

typedef struct {
    int index;
} dl_worker_args_t;

static dl_worker_args_t dl_worker_args[DL_WORKERS];

typedef struct {
    value_fn_t value;
    value_fn_t bump;
    value_fn_t zero;
    value_fn_t zero_bump;
} tls_worker_args_t;

static int tls_worker(void *opaque) {
    tls_worker_args_t *args = (tls_worker_args_t *)opaque;
    tls_child_initial = args->value();
    tls_child_bumped = args->bump();
    tls_child_zero = args->zero();
    tls_child_zero_bumped = args->zero_bump();
    return 0;
}

static int dl_worker(void *opaque) {
    dl_worker_args_t *args = (dl_worker_args_t *)opaque;
    int failed = 0;
    void *handle = dlopen("/lib/liblinux_dep_root.so", RTLD_NOW);
    dl_worker_handles[args->index] = handle;
    if (!handle) failed = 1;

    __sync_add_and_fetch(&dl_workers_ready, 1);
    while (__atomic_load_n(&dl_workers_ready, __ATOMIC_ACQUIRE) < DL_WORKERS)
        sched_yield();

    if (handle) {
        for (int i = 0; i < DL_WORKERS; i++)
            if (dl_worker_handles[i] != handle) failed = 1;
        for (int i = 0; i < DL_ITERATIONS; i++) {
            void *temporary =
                dlopen("/lib/liblinux_dep_root.so", RTLD_NOW);
            value_fn_t answer = temporary ?
                (value_fn_t)dlsym(temporary, "hbos_dep_root_answer") : NULL;
            if (temporary != handle || !answer || answer() != 42)
                failed = 1;
            sched_yield();
            if (temporary && dlclose(temporary) != 0) failed = 1;
        }

        /* Leave an unconsumed error in every child.  The parent's dlerror()
         * must remain clear, proving that error state is thread-local. */
        if (dlsym(handle, "hbos_symbol_that_does_not_exist") != NULL)
            failed = 1;
        if (dlclose(handle) != 0) failed = 1;
    }
    if (failed) __sync_add_and_fetch(&dl_worker_failures, 1);
    return 0;
}

void __attribute__((used, noinline)) hbos_record_event(int event) {
    if (event_count < (int)(sizeof(events) / sizeof(events[0])))
        events[event_count++] = event;
}

static int expect_events(const int *expected, int count) {
    if (event_count != count) return 0;
    for (int i = 0; i < count; i++)
        if (events[i] != expected[i]) return 0;
    return 1;
}

static int call_value(void *handle, const char *name, int expected) {
    value_fn_t function = (value_fn_t)dlsym(handle, name);
    return function && function() == expected;
}

int main(void) {
    void *leaf = dlopen("/lib/liblinux_dep_leaf.so", RTLD_NOW);
    if (!leaf || !call_value(leaf, "hbos_dep_leaf_value", 40)) {
        puts("LINUX_DLOPEN_DEPS: LEAF_FAIL");
        return 1;
    }
    static const int leaf_initialized[] = {10, 11};
    if (!expect_events(leaf_initialized, 2)) {
        puts("LINUX_DLOPEN_DEPS: LEAF_INIT_ORDER");
        return 8;
    }
    value_fn_t leaf_value =
        (value_fn_t)dlsym(leaf, "hbos_dep_leaf_value");
    value_fn_t leaf_bump =
        (value_fn_t)dlsym(leaf, "hbos_dep_leaf_bump");
    value_fn_t leaf_zero =
        (value_fn_t)dlsym(leaf, "hbos_dep_leaf_zero");
    value_fn_t leaf_zero_bump =
        (value_fn_t)dlsym(leaf, "hbos_dep_leaf_zero_bump");
    set_fn_t leaf_set = (set_fn_t)dlsym(leaf, "hbos_dep_leaf_set");
    if (!leaf_value || !leaf_bump || !leaf_zero || !leaf_zero_bump ||
        !leaf_set || leaf_bump() != 41 || leaf_zero() != 0 ||
        leaf_zero_bump() != 1) {
        puts("LINUX_DLOPEN_DEPS: TLS_PARENT_FAIL");
        return 15;
    }
    leaf_set(40);
    tls_worker_args_t tls_args = {
        leaf_value, leaf_bump, leaf_zero, leaf_zero_bump
    };
    int tls_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                    CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
                    CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID;
    int tls_tid = clone(tls_worker,
                        tls_thread_stack + sizeof(tls_thread_stack),
                        tls_flags, &tls_args, tls_thread_tcb,
                        (void *)&tls_child_tid);
    if (tls_tid < 0) {
        puts("LINUX_DLOPEN_DEPS: TLS_CLONE_FAIL");
        return 16;
    }
    while (tls_child_tid != 0)
        (void)syscall(SYS_futex, (long)&tls_child_tid, 9, tls_tid,
                      0L, 0L, 0x40000000L);
    if (tls_child_initial != 40 || tls_child_bumped != 41 ||
        tls_child_zero != 0 || tls_child_zero_bumped != 1 ||
        leaf_value() != 40 || leaf_zero() != 1) {
        puts("LINUX_DLOPEN_DEPS: TLS_ISOLATION_FAIL");
        return 17;
    }

    void *root = dlopen("/lib/liblinux_dep_root.so", RTLD_NOW);
    if (!root || !call_value(root, "hbos_dep_root_answer", 42)) {
        puts("LINUX_DLOPEN_DEPS: ROOT_FAIL");
        return 2;
    }
    if (!call_value(leaf, "hbos_versioned_value", 202) ||
        !call_value(root, "hbos_dep_root_versioned", 101)) {
        puts("LINUX_DLOPEN_DEPS: SYMBOL_VERSION_FAIL");
        return 18;
    }
    value_fn_t version_1 = (value_fn_t)dlvsym(
        leaf, "hbos_versioned_value", "HBOS_1.0");
    value_fn_t version_2 = (value_fn_t)dlvsym(
        leaf, "hbos_versioned_value", "HBOS_2.0");
    if (!version_1 || version_1() != 101 ||
        !version_2 || version_2() != 202 ||
        dlvsym(leaf, "hbos_versioned_value", "HBOS_MISSING") != NULL) {
        puts("LINUX_DLOPEN_DEPS: DLVSYM_FAIL");
        return 19;
    }
    int *default_tls = (int *)dlsym(leaf, "hbos_versioned_tls");
    int *version_1_tls = (int *)dlvsym(
        leaf, "hbos_versioned_tls", "HBOS_1.0");
    if (!default_tls || *default_tls != 302 ||
        !version_1_tls || *version_1_tls != 301 ||
        !call_value(root, "hbos_dep_root_versioned_tls", 301)) {
        puts("LINUX_DLOPEN_DEPS: TLS_SYMBOL_VERSION_FAIL");
        return 20;
    }
    if (!call_value(leaf, "hbos_ifunc_value", 303) ||
        !call_value(leaf, "hbos_dep_leaf_local_ifunc", 404) ||
        !call_value(root, "hbos_dep_root_ifunc", 303)) {
        puts("LINUX_DLOPEN_DEPS: IFUNC_FAIL");
        return 21;
    }
    static const int root_initialized[] = {10, 11, 20, 21};
    if (!expect_events(root_initialized, 4)) {
        puts("LINUX_DLOPEN_DEPS: ROOT_INIT_ORDER");
        return 9;
    }
    void *root_again = dlopen("/lib/liblinux_dep_root.so", RTLD_NOW);
    if (!root_again || root_again != root) {
        puts("LINUX_DLOPEN_DEPS: DEDUP_FAIL");
        return 3;
    }
    if (!expect_events(root_initialized, 4)) {
        puts("LINUX_DLOPEN_DEPS: INIT_REPEATED");
        return 10;
    }
    if (dlclose(root) != 0 ||
        !call_value(root_again, "hbos_dep_root_answer", 42)) {
        puts("LINUX_DLOPEN_DEPS: ROOT_REF_FAIL");
        return 4;
    }
    if (!expect_events(root_initialized, 4)) {
        puts("LINUX_DLOPEN_DEPS: EARLY_FINI");
        return 11;
    }
    if (dlclose(root_again) != 0 ||
        dlsym(root_again, "hbos_dep_root_answer") != NULL) {
        puts("LINUX_DLOPEN_DEPS: ROOT_CLOSE_FAIL");
        return 5;
    }
    static const int root_finalized[] = {10, 11, 20, 21, 24, 25};
    if (!expect_events(root_finalized, 6)) {
        puts("LINUX_DLOPEN_DEPS: ROOT_FINI_ORDER");
        return 12;
    }
    if (!call_value(leaf, "hbos_dep_leaf_value", 40) ||
        dlclose(leaf) != 0) {
        puts("LINUX_DLOPEN_DEPS: LEAF_REF_FAIL");
        return 6;
    }
    static const int first_cycle[] = {10, 11, 20, 21, 24, 25, 14, 15};
    if (!expect_events(first_cycle, 8)) {
        puts("LINUX_DLOPEN_DEPS: LEAF_FINI_ORDER");
        return 13;
    }

    /* With no live handle, four threads race through the first load, symbol
     * lookup, reference churn, and final close.  Their base handles keep the
     * graph resident until every thread has entered, so constructors and
     * destructors must still run exactly once for the whole cycle. */
    int dl_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                   CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
                   CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID;
    for (int i = 0; i < DL_WORKERS; i++) {
        dl_worker_args[i].index = i;
        int tid = clone(dl_worker,
                        dl_thread_stacks[i] + sizeof(dl_thread_stacks[i]),
                        dl_flags, &dl_worker_args[i], dl_thread_tcbs[i],
                        (void *)&dl_child_tids[i]);
        if (tid < 0) {
            puts("LINUX_DLOPEN_DEPS: CONCURRENT_CLONE_FAIL");
            return 22;
        }
    }
    for (int i = 0; i < DL_WORKERS; i++) {
        while (dl_child_tids[i] != 0)
            (void)syscall(SYS_futex, (long)&dl_child_tids[i], 9,
                          dl_child_tids[i], 0L, 0L, 0x40000000L);
    }
    if (dl_worker_failures || dlerror() != NULL) {
        puts("LINUX_DLOPEN_DEPS: CONCURRENT_FAIL");
        return 23;
    }
    static const int both_cycles[] = {
        10, 11, 20, 21, 24, 25, 14, 15,
        10, 11, 20, 21, 24, 25, 14, 15,
    };
    if (!expect_events(both_cycles, 16)) {
        puts("LINUX_DLOPEN_DEPS: CONCURRENT_ORDER");
        return 14;
    }
    puts("LINUX_DLOPEN_DEPS: PASS");
    return 0;
}
