#include "dlfcn.h"
#include "syscall.h"

/* HBOS: real dynamic-library loading, backed by src/user/ldso.c's ELF
 * ET_DYN loader (kernel side) via the HBOS_SYS_DLOPEN/DLSYM/DLCLOSE
 * syscalls -- see that file for what's actually supported: PT_LOAD
 * segment mapping, .dynamic parsing, R_X86_64_RELATIVE/GLOB_DAT/JUMP_SLOT
 * relocations. `flags` is accepted for API compatibility but otherwise
 * ignored (HBOS's loader always resolves everything eagerly at load time;
 * there's no lazy-PLT-binding distinction to make between RTLD_LAZY and
 * RTLD_NOW). */

static int dl_had_error;

void *dlopen(const char *filename, int flags) {
    (void)flags;
    if (!filename) { dl_had_error = 1; return 0; }
    long h = __syscall1(HBOS_SYS_DLOPEN, (long)filename);
    if (h == 0) { dl_had_error = 1; return 0; }
    dl_had_error = 0;
    return (void *)h;
}

void *dlsym(void *handle, const char *symbol) {
    if (!handle || !symbol) { dl_had_error = 1; return 0; }
    long addr = __syscall3(HBOS_SYS_DLSYM, (long)handle, (long)symbol, 0);
    if (addr == 0) { dl_had_error = 1; return 0; }
    dl_had_error = 0;
    return (void *)addr;
}

int dlclose(void *handle) {
    if (!handle) { dl_had_error = 1; return -1; }
    long ret = __syscall1(HBOS_SYS_DLCLOSE, (long)handle);
    if (ret != 0) { dl_had_error = 1; return -1; }
    dl_had_error = 0;
    return 0;
}

char *dlerror(void) {
    if (!dl_had_error) return 0;
    dl_had_error = 0;
    return "dlopen/dlsym/dlclose failed (see kernel log for the load-time reason)";
}