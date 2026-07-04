#include "sys/wait.h"
#include "syscall.h"
#include "errno.h"

pid_t waitpid(pid_t pid, int *status, int options) {
    long ret = __syscall3(HBOS_SYS_WAITPID, pid, (long)status, options);
    return (pid_t)__syscall_errno(ret);
}
