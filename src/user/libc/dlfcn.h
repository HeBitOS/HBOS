#ifndef HBOS_USER_LIBC_DLFCN_H
#define HBOS_USER_LIBC_DLFCN_H

#define RTLD_LAZY  0x00001
#define RTLD_NOW   0x00002

void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
int   dlclose(void *handle);
char *dlerror(void);

#endif