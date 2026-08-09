#include <dlfcn.h>
#include <stdio.h>

typedef int (*answer_fn_t)(void);

int main(void) {
    for (int round = 0; round < 2; round++) {
        void *handle = dlopen("/lib/liblinux_dlopen.so", RTLD_NOW);
        if (!handle) {
            puts("LINUX_DLOPEN: LOAD_FAIL");
            return 1;
        }
        answer_fn_t answer =
            (answer_fn_t)dlsym(handle, "hbos_large_answer");
        if (!answer || answer() != 42) {
            puts("LINUX_DLOPEN: SYMBOL_FAIL");
            (void)dlclose(handle);
            return 2;
        }
        if (dlclose(handle) != 0) {
            puts("LINUX_DLOPEN: CLOSE_FAIL");
            return 3;
        }
        if (dlsym(handle, "hbos_large_answer") != NULL) {
            puts("LINUX_DLOPEN: STALE_HANDLE");
            return 4;
        }
    }
    puts("LINUX_DLOPEN: PASS");
    return 0;
}
