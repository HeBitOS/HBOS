#include "unistd.h"
#include "syscall.h"

unsigned int sleep(unsigned int seconds) {
    long ret = __syscall1(HBOS_SYS_SLEEP, (long)seconds);
    return ret < 0 ? (unsigned int)seconds : 0;
}

int usleep(unsigned int useconds) {
    long ret = __syscall1(HBOS_SYS_USLEEP, (long)useconds);
    return ret < 0 ? -1 : 0;
}

pid_t getpid(void) {
    return (pid_t)__syscall1(HBOS_SYS_GETPID, 0);
}

char *getcwd(char *buf, size_t size) {
    long ret = __syscall3(HBOS_SYS_GETCWD, (long)buf, (long)size, 0);
    if (ret < 0) return 0;
    return buf;
}