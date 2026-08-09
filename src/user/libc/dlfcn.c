#include "dlfcn.h"
#include "syscall.h"

/* HBOS: real dynamic-library loading, backed by src/user/ldso.c's ELF
 * ET_DYN loader (kernel side) via the HBOS_SYS_DLOPEN/DLSYM/DLCLOSE
 * syscalls -- see that file for what's actually supported: PT_LOAD
 * segment mapping, .dynamic parsing, R_X86_64_RELATIVE/GLOB_DAT/JUMP_SLOT
 * relocations, recursive DT_NEEDED dependencies, and ring3 execution of
 * DT_INIT/FINI plus init/fini arrays and General Dynamic PT_TLS. `flags` is accepted for API
 * compatibility but otherwise
 * ignored (HBOS's loader always resolves everything eagerly at load time;
 * there's no lazy-PLT-binding distinction to make between RTLD_LAZY and
 * RTLD_NOW). */

#define DL_ERROR_SLOTS 64U

typedef struct {
    volatile long tid;
    int had_error;
} dl_error_slot_t;

static dl_error_slot_t dl_error_slots[DL_ERROR_SLOTS];
static volatile long dl_lock_owner;
static unsigned int dl_lock_depth;

static long dl_tid(void) {
    long tid = __syscall1(HBOS_SYS_GETTID, 0);
    return tid > 0 ? tid : 1;
}

static dl_error_slot_t *dl_error_state(void) {
    long tid = dl_tid();
    unsigned long start = (unsigned long)tid % DL_ERROR_SLOTS;
    for (unsigned long offset = 0; offset < DL_ERROR_SLOTS; offset++) {
        dl_error_slot_t *slot =
            &dl_error_slots[(start + offset) % DL_ERROR_SLOTS];
        long owner = __atomic_load_n(&slot->tid, __ATOMIC_ACQUIRE);
        if (owner == tid) return slot;
        if (owner == 0) {
            long empty = 0;
            if (__atomic_compare_exchange_n(&slot->tid, &empty, tid, 0,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE))
                return slot;
            if (empty == tid) return slot;
        }
    }
    dl_error_slot_t *slot = &dl_error_slots[start];
    __atomic_store_n(&slot->tid, tid, __ATOMIC_RELEASE);
    slot->had_error = 0;
    return slot;
}

static void dl_set_error(int value) {
    dl_error_state()->had_error = value;
}

static void dl_lock(void) {
    long tid = dl_tid();
    if (__atomic_load_n(&dl_lock_owner, __ATOMIC_ACQUIRE) == tid) {
        dl_lock_depth++;
        return;
    }
    for (;;) {
        long empty = 0;
        if (__atomic_compare_exchange_n(&dl_lock_owner, &empty, tid, 0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            dl_lock_depth = 1;
            return;
        }
        (void)__syscall1(HBOS_SYS_SCHED_YIELD, 0);
    }
}

static void dl_unlock(void) {
    long tid = dl_tid();
    if (__atomic_load_n(&dl_lock_owner, __ATOMIC_ACQUIRE) != tid ||
        !dl_lock_depth)
        return;
    if (--dl_lock_depth == 0)
        __atomic_store_n(&dl_lock_owner, 0, __ATOMIC_RELEASE);
}

typedef struct {
    unsigned long module;
    unsigned long offset;
} hbos_tls_index_t;

/* SysV x86-64 General Dynamic TLS resolver.  The loader writes this pair
 * into the library GOT via DTPMOD64/DTPOFF64 relocations; the kernel lazily
 * creates one initialized block per (thread,module) on first access. */
void *__tls_get_addr(const hbos_tls_index_t *index) {
    if (!index || index->module == 0 || index->module > 0xffffffffUL)
        return 0;
    long address = __syscall3(HBOS_SYS_DLTLS_GET,
                              (long)index->module,
                              (long)index->offset, 0);
    return address > 0 ? (void *)(uintptr_t)address : 0;
}

#define DL_CALLBACK_LIMIT 4096UL

static int dl_run_ifuncs(void *handle) {
    for (unsigned long ordinal = 0; ordinal < DL_CALLBACK_LIMIT; ordinal++) {
        long resolver = __syscall3(HBOS_SYS_DLIFUNC_NEXT, (long)handle,
                                   (long)ordinal, 0);
        if (resolver == 0) return 0;
        if (resolver == -1) return -1;
        if (resolver == 1) continue;
        uintptr_t result =
            ((uintptr_t (*)(void))(uintptr_t)resolver)();
        if (__syscall3(HBOS_SYS_DLIFUNC_APPLY, (long)handle,
                       (long)ordinal, (long)result) != 0)
            return -1;
    }
    return -1;
}

static int dl_run_callbacks(long syscall_number, void *handle) {
    for (unsigned long ordinal = 0; ordinal < DL_CALLBACK_LIMIT; ordinal++) {
        long entry = __syscall3(syscall_number, (long)handle,
                                (long)ordinal, 0);
        if (entry == 0) return 0;
        if (entry == -1) return -1;
        ((void (*)(void))(uintptr_t)entry)();
    }
    return -1;
}

void *dlopen(const char *filename, int flags) {
    (void)flags;
    if (!filename) { dl_set_error(1); return 0; }
    dl_lock();
    long h = __syscall1(HBOS_SYS_DLOPEN, (long)filename);
    if (h == 0) {
        dl_set_error(1);
        dl_unlock();
        return 0;
    }
    if (dl_run_ifuncs((void *)h) != 0 ||
        dl_run_callbacks(HBOS_SYS_DLINIT_NEXT, (void *)h) != 0) {
        (void)__syscall1(HBOS_SYS_DLCLOSE, h);
        dl_set_error(1);
        dl_unlock();
        return 0;
    }
    dl_set_error(0);
    dl_unlock();
    return (void *)h;
}

void *dlsym(void *handle, const char *symbol) {
    if (!handle || !symbol) { dl_set_error(1); return 0; }
    dl_lock();
    long addr = __syscall3(HBOS_SYS_DLSYM, (long)handle, (long)symbol, 0);
    if (addr == 0) {
        dl_set_error(1);
        dl_unlock();
        return 0;
    }
    dl_set_error(0);
    dl_unlock();
    return (void *)addr;
}

void *dlvsym(void *handle, const char *symbol, const char *version) {
    if (!handle || !symbol || !version || !version[0]) {
        dl_set_error(1);
        return 0;
    }
    dl_lock();
    long address = __syscall3(HBOS_SYS_DLSYM, (long)handle,
                              (long)symbol, (long)version);
    if (address == 0) {
        dl_set_error(1);
        dl_unlock();
        return 0;
    }
    dl_set_error(0);
    dl_unlock();
    return (void *)address;
}

int dlclose(void *handle) {
    if (!handle) { dl_set_error(1); return -1; }
    dl_lock();
    if (dl_run_callbacks(HBOS_SYS_DLFINI_NEXT, handle) != 0) {
        dl_set_error(1);
        dl_unlock();
        return -1;
    }
    long ret = __syscall1(HBOS_SYS_DLCLOSE, (long)handle);
    if (ret != 0) {
        dl_set_error(1);
        dl_unlock();
        return -1;
    }
    dl_set_error(0);
    dl_unlock();
    return 0;
}

char *dlerror(void) {
    dl_error_slot_t *state = dl_error_state();
    if (!state->had_error) return 0;
    state->had_error = 0;
    return "dlopen/dlsym/dlclose failed (see kernel log for the load-time reason)";
}
