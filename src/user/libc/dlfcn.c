#include "dlfcn.h"

static char dl_error_msg[] = "dynamic linking not supported";

void *dlopen(const char *filename, int flags) {
    (void)filename;
    (void)flags;
    return 0;
}

void *dlsym(void *handle, const char *symbol) {
    (void)handle;
    (void)symbol;
    return 0;
}

int dlclose(void *handle) {
    (void)handle;
    return -1;
}

char *dlerror(void) {
    return dl_error_msg;
}