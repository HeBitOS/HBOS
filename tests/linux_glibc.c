#include <stdint.h>
#include <pthread.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static int worker_value;
static __thread int worker_tls = 23;
static char file_buffer[4096];

static int file_contains(const char *path, const char *needle) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t length = read(fd, file_buffer, sizeof(file_buffer) - 1);
    close(fd);
    if (length <= 0) return 0;
    file_buffer[length] = '\0';
    return strstr(file_buffer, needle) != NULL;
}

static void *worker(void *argument) {
    if (worker_tls != 23) return (void *)(uintptr_t)1;
    worker_tls = 47;
    if (pthread_mutex_lock(&worker_lock) != 0)
        return (void *)(uintptr_t)2;
    worker_value = *(int *)argument;
    if (pthread_mutex_unlock(&worker_lock) != 0)
        return (void *)(uintptr_t)3;
    return worker_tls == 47 ? (void *)(uintptr_t)0x4850 :
                              (void *)(uintptr_t)4;
}

int main(void) {
    static const char pass[] = "LINUX_GLIBC: PASS\n";
    struct timespec now;
    char *memory = malloc(4096);
    if (!memory || getpid() <= 0 ||
        clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 1;
    memset(memory, 0x48, 4096);
    for (size_t i = 0; i < 4096; i++) {
        if ((unsigned char)memory[i] != 0x48) return 2;
    }
    free(memory);
    pthread_t thread;
    int requested = 0x1234;
    int create_result = pthread_create(&thread, NULL, worker, &requested);
    if (create_result != 0) return 20 + create_result;
    void *thread_result = NULL;
    if (pthread_join(thread, &thread_result) != 0 ||
        thread_result != (void *)(uintptr_t)0x4850 ||
        worker_value != requested || worker_tls != 23)
        return 4;
    char executable[128];
    ssize_t executable_length = readlink("/proc/self/exe", executable,
                                         sizeof(executable) - 1);
    if (executable_length <= 0) return 6;
    executable[executable_length] = '\0';
    if (strstr(executable, "linux_glibc") == NULL ||
        !file_contains("/proc/self/status", "Name:\tlinux_glibc") ||
        !file_contains("/proc/self/status", "Threads:\t1") ||
        !file_contains("/proc/self/maps", "[heap]") ||
        !file_contains("/proc/meminfo", "MemTotal:") ||
        !file_contains("/proc/cpuinfo", "processor"))
        return 7;
    char fd_target[64];
    if (readlink("/proc/self/fd/1", fd_target, sizeof(fd_target)) <= 0)
        return 8;
    DIR *fd_directory = opendir("/proc/self/fd");
    if (!fd_directory) return 9;
    int saw_stdout = 0;
    struct dirent *entry;
    while ((entry = readdir(fd_directory)) != NULL) {
        if (strcmp(entry->d_name, "1") == 0) saw_stdout = 1;
    }
    closedir(fd_directory);
    if (!saw_stdout) return 10;
    if (!file_contains("/sys/devices/system/cpu/online", "0") ||
        !file_contains("/sys/devices/system/cpu/cpu0/topology/core_id", "0") ||
        !file_contains("/sys/class/net/lo/address", "00:00:00:00:00:00") ||
        !file_contains("/sys/class/net/eth0/address", ":"))
        return 11;
    DIR *net_directory = opendir("/sys/class/net");
    if (!net_directory) return 12;
    int saw_loopback = 0;
    int saw_ethernet = 0;
    while ((entry = readdir(net_directory)) != NULL) {
        if (strcmp(entry->d_name, "lo") == 0) saw_loopback = 1;
        if (strcmp(entry->d_name, "eth0") == 0) saw_ethernet = 1;
    }
    closedir(net_directory);
    if (!saw_loopback || !saw_ethernet) return 13;
    return write(STDOUT_FILENO, pass, sizeof(pass) - 1) ==
           (long)(sizeof(pass) - 1) ? 0 : 5;
}
