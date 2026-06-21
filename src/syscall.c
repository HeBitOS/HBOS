/**
 * @file    syscall.c
 * @brief   HBOS 系统调用分发器
 *
 * 接收来自 int 0x80 汇编 stub 的系统调用帧，
 * 根据系统调用号分发到对应的 POSIX 实现函数。
 *
 * 返回值约定:
 *   - 成功: 返回非负值（具体含义因调用而异）
 *   - 失败: 返回负的 errno 值（如 -ENOENT）
 */

#include <stdint.h>

#include "errno.h"
#include "fcntl.h"
#include "sys/stat.h"
#include "sys/wait.h"
#include "sys/dirent.h"
#include "syscall.h"
#include "unistd.h"
#include "core/task.h"
#include "core/vmm.h"
#include "fd.h"
#include "string.h"
#include "acpi.h"
#include "net.h"
#include "signal.h"
#include "fs.h"
#include "core/heap.h"
#include "elf.h"
#include "vfs.h"

#define SYSCALL_EXEC_MAX_SIZE 65536

/**
 * 将 POSIX 函数返回值转换为系统调用返回值
 * POSIX 函数通过 errno 全局变量报告错误，
 * 系统调用通过负返回值报告错误。
 */
static uint64_t finish_syscall(long ret) {
    if (ret < 0 && errno > 0)
        return (uint64_t)(-(int64_t)errno);
    return (uint64_t)ret;
}

static uint64_t align_page_up(uint64_t value) {
    return (value + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

static int map_user_heap_growth(uint64_t old_brk, uint64_t new_brk) {
    if (new_brk <= old_brk) return 0;
    if (new_brk > UINT64_MAX - (PAGE_SIZE - 1)) return -1;

    uint64_t va_start = old_brk & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t va_end = align_page_up(new_brk);
    for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE) {
        if (vmm_get_phys(va) != 0) continue;
        if (!vmm_alloc_page_at(va, VMM_P | VMM_W | VMM_U)) return -1;
    }
    return 0;
}

static uint64_t user_sbrk(intptr_t increment) {
    task_t *cur = task_current();
    if (!cur || !cur->user_heap_start ||
        cur->user_heap_limit <= cur->user_heap_start)
        return (uint64_t)(-ENOMEM);

    uint64_t old_brk = cur->user_brk ? cur->user_brk : cur->user_heap_start;
    uint64_t new_brk = old_brk;

    if (increment > 0) {
        uint64_t inc = (uint64_t)increment;
        if (inc > cur->user_heap_limit - old_brk) return (uint64_t)(-ENOMEM);
        new_brk = old_brk + inc;
    } else if (increment < 0) {
        uint64_t dec = (uint64_t)(-(increment + 1)) + 1;
        if (dec > old_brk - cur->user_heap_start) return (uint64_t)(-ENOMEM);
        new_brk = old_brk - dec;
    }

    if (map_user_heap_growth(old_brk, new_brk) != 0)
        return (uint64_t)(-ENOMEM);
    cur->user_brk = new_brk;
    return old_brk;
}

static uint64_t user_brk(uint64_t new_brk) {
    task_t *cur = task_current();
    if (!cur || !cur->user_heap_start ||
        cur->user_heap_limit <= cur->user_heap_start)
        return (uint64_t)(-ENOMEM);
    if (!new_brk) return cur->user_brk ? cur->user_brk : cur->user_heap_start;
    if (new_brk < cur->user_heap_start || new_brk > cur->user_heap_limit)
        return (uint64_t)(-ENOMEM);

    uint64_t old_brk = cur->user_brk ? cur->user_brk : cur->user_heap_start;
    if (map_user_heap_growth(old_brk, new_brk) != 0)
        return (uint64_t)(-ENOMEM);
    cur->user_brk = new_brk;
    return new_brk;
}

/**
 * 系统调用主分发函数
 * 由 interrupt_asm.asm 中的 syscall_int80_stub 调用
 *
 * @param f  系统调用帧（包含调用号和 6 个参数）
 * @return 系统调用返回值
 */
uint64_t syscall_dispatch_frame(hbos_syscall_frame_t *f) {
    if (!f) return (uint64_t)(-EFAULT);

    switch (f->nr) {
        // ============================================================
        // 文件 I/O (0-11)
        // ============================================================
        case HBOS_SYS_READ:
            return finish_syscall((long)read((int)f->a0, (void *)f->a1, (size_t)f->a2));

        case HBOS_SYS_WRITE:
            return finish_syscall((long)write((int)f->a0, (const void *)f->a1, (size_t)f->a2));

        case HBOS_SYS_OPEN:
            return finish_syscall((long)open((const char *)f->a0, (int)f->a1, (int)f->a2));

        case HBOS_SYS_CLOSE:
            return finish_syscall((long)close((int)f->a0));

        case HBOS_SYS_LSEEK:
            return finish_syscall((long)lseek((int)f->a0, (off_t)f->a1, (int)f->a2));

        case HBOS_SYS_FSTAT:
            return finish_syscall((long)fstat((int)f->a0, (struct stat *)f->a1));

        case HBOS_SYS_STAT:
            return finish_syscall((long)stat((const char *)f->a0, (struct stat *)f->a1));

        case HBOS_SYS_UNLINK:
            return finish_syscall((long)unlink((const char *)f->a0));

        case HBOS_SYS_ISATTY:
            return finish_syscall((long)isatty((int)f->a0));

        case HBOS_SYS_GETPID:
            return (uint64_t)getpid();

        case HBOS_SYS_SBRK:
            return user_sbrk((intptr_t)f->a0);

        case HBOS_SYS_EXIT: {
            // 实际终止当前任务
            int status = (int)f->a0;
            task_set_exit_status(status);
            task_exit();
            return 0;  // 不会到达这里
        }

        // ============================================================
        // 进程控制 (12-14)
        // ============================================================
        case HBOS_SYS_GETPPID:
            return (uint64_t)getppid();

        case HBOS_SYS_SLEEP:
            return finish_syscall((long)sleep((unsigned int)f->a0));

        case HBOS_SYS_USLEEP:
            return finish_syscall((long)usleep((useconds_t)f->a0));

        // ============================================================
        // 系统信息 (15-16)
        // ============================================================
        case HBOS_SYS_UNAME: {
            // uname: 返回系统信息到 utsname 结构
            // 结构定义在 sys/utsname.h 中
            struct utsname {
                char sysname[65];
                char nodename[65];
                char release[65];
                char version[65];
                char machine[65];
            };
            struct utsname *buf = (struct utsname *)f->a0;
            if (!buf) return (uint64_t)(-EFAULT);
            memset(buf, 0, sizeof(struct utsname));
            memcpy(buf->sysname, "HBOS", 5);
            memcpy(buf->nodename, "hbos", 5);
            memcpy(buf->release, "0.1-beta3-pre3", 15);
            memcpy(buf->version, "HBOS 0.1-beta3-pre3", 20);
            memcpy(buf->machine, "x86_64", 7);
            return 0;
        }

        case HBOS_SYS_GETTOD: {
            // gettimeofday: 返回当前时间（基于 RDTSC 的近似值）
            struct timeval {
                uint64_t tv_sec;
                uint64_t tv_usec;
            };
            struct timeval *tv = (struct timeval *)f->a0;
            if (!tv) return (uint64_t)(-EFAULT);
            // 使用 RDTSC 近似时间（从启动开始的秒数）
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            uint64_t tsc = ((uint64_t)hi << 32) | lo;
            // 假设 ~1GHz TSC
            tv->tv_sec = tsc / 1000000000ULL;
            tv->tv_usec = (tsc % 1000000000ULL) / 1000;
            return 0;
        }

        // ============================================================
        // 文件系统扩展 (17-22)
        // ============================================================
        case HBOS_SYS_ACCESS: {
            return finish_syscall((long)access((const char *)f->a0, (int)f->a1));
        }

        case HBOS_SYS_FTRUNCATE: {
            return finish_syscall((long)ftruncate((int)f->a0, (off_t)f->a1));
        }

        case HBOS_SYS_MKDIR: {
            return finish_syscall((long)mkdir((const char *)f->a0, (mode_t)f->a1));
        }

        case HBOS_SYS_RMDIR: {
            return finish_syscall((long)rmdir((const char *)f->a0));
        }

        case HBOS_SYS_GETCWD: {
            char *buf = (char *)f->a0;
            size_t size = (size_t)f->a1;
            char *ret = getcwd(buf, size);
            if (!ret) return (uint64_t)(-(int64_t)errno);
            return (uint64_t)strlen(ret);
        }

        case HBOS_SYS_CHDIR: {
            return finish_syscall((long)chdir((const char *)f->a0));
        }

        // ============================================================
        // 文件描述符操作和时间 (23-26)
        // ============================================================
        case HBOS_SYS_DUP: {
            int oldfd = (int)f->a0;
            if (oldfd < 0 || oldfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            task_t *cur = task_current();
            if (!cur || !cur->fds[oldfd].used)
                return (uint64_t)(-EBADF);
            int newfd = -1;
            for (int i = 0; i < POSIX_MAX_FDS; i++) {
                if (!cur->fds[i].used) { newfd = i; break; }
            }
            if (newfd < 0) return (uint64_t)(-EMFILE);
            cur->fds[newfd] = cur->fds[oldfd];
            return (uint64_t)newfd;
        }

        case HBOS_SYS_GETEUID:
            // geteuid: 返回有效用户 ID（当前始终为 root）
            return 0;

        case HBOS_SYS_GETEGID:
            // getegid: 返回有效组 ID（当前始终为 root）
            return 0;

        case HBOS_SYS_GETTID:
            // gettid: 返回线程 ID（当前 = 任务 ID）
            return (uint64_t)task_get_id();

        // ============================================================
        // 文件描述符操作扩展 (27-31)
        // ============================================================
        case HBOS_SYS_DUP2: {
            int oldfd = (int)f->a0;
            int newfd = (int)f->a1;
            if (oldfd < 0 || newfd < 0 || oldfd >= POSIX_MAX_FDS || newfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            task_t *cur = task_current();
            if (!cur || !cur->fds[oldfd].used)
                return (uint64_t)(-EBADF);
            if (oldfd == newfd) return (uint64_t)newfd;
            if (cur->fds[newfd].used) close(newfd);
            cur->fds[newfd] = cur->fds[oldfd];
            return (uint64_t)newfd;
        }

        case HBOS_SYS_PIPE: {
            int *pipefd = (int *)f->a0;
            if (!pipefd) return (uint64_t)(-EFAULT);
            extern int pipe(int pipefd[2]);
            int ret = pipe(pipefd);
            if (ret < 0) return (uint64_t)(-errno);
            return 0;
        }

        case HBOS_SYS_FCNTL: {
            int fd = (int)f->a0;
            int cmd = (int)f->a1;
            long arg = (long)f->a2;
            if (fd < 0 || fd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            task_t *cur = task_current();
            if (!cur || !cur->fds[fd].used)
                return (uint64_t)(-EBADF);
            switch (cmd) {
                case 0:   return cur->fds[fd].flags;
                case 1:   cur->fds[fd].flags = (int)arg; return 0;
                case 2:   cur->fds[fd].flags |= (int)arg; return 0;
                case 3:   cur->fds[fd].flags &= ~(int)arg; return 0;
                default:  return (uint64_t)(-EINVAL);
            }
        }

        case HBOS_SYS_IOCTL: {
            int fd = (int)f->a0;
            if (fd < 0 || fd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            task_t *cur = task_current();
            if (!cur || !cur->fds[fd].used)
                return (uint64_t)(-EBADF);
            return (uint64_t)(-ENOTTY);
        }

        case HBOS_SYS_READLINK: {
            const char *path = (const char *)f->a0;
            char *buf = (char *)f->a1;
            size_t bufsiz = (size_t)f->a2;
            if (!path) return (uint64_t)(-EFAULT);
            file_t *file = fs_find_file(path);
            if (!file) return (uint64_t)(-ENOENT);
            if (file->type != 2) return (uint64_t)(-EINVAL);
            uint32_t n = fs_read_file_data(file, 0, buf, (uint32_t)(bufsiz - 1));
            if (buf && bufsiz > 0) {
                if (n > bufsiz - 1) n = (uint32_t)(bufsiz - 1);
                ((uint8_t *)buf)[n] = '\0';
            }
            return (uint64_t)n;
        }

        // ============================================================
        // 进程管理扩展 (32-38)
        // ============================================================
        case HBOS_SYS_FORK: {
            int pid = task_fork();
            if (pid < 0) return (uint64_t)(-EAGAIN);
            return (uint64_t)pid;
        }

        case HBOS_SYS_EXECVE: {
            const char *path = (const char *)f->a0;
            char *const *argv = (char *const *)f->a1;
            char *const *envp = (char *const *)f->a2;
            if (!path) return (uint64_t)(-EFAULT);
            int fd = open(path, O_RDONLY);
            if (fd < 0) return (uint64_t)(-(int64_t)(errno ? errno : ENOENT));

            uint8_t *elf_buf = (uint8_t *)kmalloc(SYSCALL_EXEC_MAX_SIZE);
            if (!elf_buf) {
                close(fd);
                return (uint64_t)(-ENOMEM);
            }

            size_t size = 0;
            ssize_t n = 0;
            while ((n = read(fd, elf_buf + size, SYSCALL_EXEC_MAX_SIZE - size)) > 0) {
                size += (size_t)n;
                if (size >= SYSCALL_EXEC_MAX_SIZE) break;
            }
            int saved_errno = errno;
            close(fd);

            if (n < 0) {
                kfree(elf_buf);
                return (uint64_t)(-(int64_t)(saved_errno ? saved_errno : EIO));
            }
            if (size < sizeof(elf64_ehdr_t) || size >= SYSCALL_EXEC_MAX_SIZE) {
                kfree(elf_buf);
                return (uint64_t)(size >= SYSCALL_EXEC_MAX_SIZE ? -E2BIG : -ENOEXEC);
            }
            int ret = elf64_load_and_exec(elf_buf, size, argv, envp);
            kfree(elf_buf);
            if (ret < 0) return (uint64_t)(-ENOEXEC);
            return 0;
        }

        case HBOS_SYS_WAITPID: {
            pid_t pid = (pid_t)f->a0;
            int *status = (int *)f->a1;
            int options = (int)f->a2;
            if (pid <= 0) return (uint64_t)(-ECHILD);
            int st = 0;
            int ret = task_wait((uint32_t)pid, &st);
            if (ret < 0) return (uint64_t)(-ECHILD);
            if (options & WNOHANG) {
                const task_t *t = task_get_by_id((uint32_t)pid);
                if (t && t->state != TASK_TERMINATED) return 0;
            }
            if (status)
                *status = W_EXITCODE(st, 0);
            return (uint64_t)pid;
        }

        case HBOS_SYS_KILL: {
            pid_t pid = (pid_t)f->a0;
            int sig = (int)f->a1;
            if (pid <= 0) return (uint64_t)(-ESRCH);
            if (task_kill((uint32_t)pid, sig) < 0)
                return (uint64_t)(-ESRCH);
            return 0;
        }

        case HBOS_SYS_GETUID:
            return 0;

        case HBOS_SYS_GETGID:
            return 0;

        case HBOS_SYS_SETUID: {
            uid_t uid = (uid_t)f->a0;
            if (uid != 0) return (uint64_t)(-EPERM);
            return 0;
        }

        // ============================================================
        // 信号处理 (39-42)
        // ============================================================
        case HBOS_SYS_SIGNAL: {
            int sig = (int)f->a0;
            void (*handler)(int) = (void (*)(int))f->a1;
            if (sig <= 0 || sig >= _NSIG) {
                errno = EINVAL;
                return (uint64_t)(-EINVAL);
            }
            task_t *cur = task_current();
            if (!cur) return (uint64_t)(-ESRCH);
            void (*old)(int) = cur->sig_handler[sig];
            cur->sig_handler[sig] = handler;
            return (uint64_t)(uintptr_t)old;
        }

        case HBOS_SYS_SIGACTION: {
            int sig = (int)f->a0;
            void (*handler)(int) = (void (*)(int))f->a1;
            if (sig <= 0 || sig >= _NSIG) {
                errno = EINVAL;
                return (uint64_t)(-EINVAL);
            }
            task_t *cur = task_current();
            if (!cur) return (uint64_t)(-ESRCH);
            void (*old)(int) = cur->sig_handler[sig];
            cur->sig_handler[sig] = handler;
            void **oact = (void **)f->a2;
            if (oact) *oact = (void *)old;
            return 0;
        }

        case HBOS_SYS_SIGPROCMASK: {
            return 0;
        }

        case HBOS_SYS_PAUSE: {
            return (uint64_t)(-EINTR);
        }

        // ============================================================
        // 内存管理 (43-47)
        // ============================================================
        case HBOS_SYS_MMAP: {
            void *addr = (void *)f->a0;
            size_t len = (size_t)f->a1;
            int prot = (int)f->a2;
            int flags = (int)f->a3;
            int fd = (int)f->a4;
            off_t off = (off_t)f->a5;
            (void)prot; (void)flags; (void)fd; (void)off;
            if (len == 0) return (uint64_t)(-EINVAL);
            size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
            void *p = addr;
            if (!p) {
                for (uint64_t va = 0x0000100000000000ULL; ; va += PAGE_SIZE) {
                    int free = 1;
                    for (uint64_t va2 = va; va2 < va + pages * PAGE_SIZE; va2 += PAGE_SIZE) {
                        if (vmm_get_phys(va2) != 0) { free = 0; break; }
                    }
                    if (free) { p = (void *)va; break; }
                }
            }
            for (size_t i = 0; i < pages; i++)
                vmm_alloc_page_at((uint64_t)p + i * PAGE_SIZE, 0x07);
            task_t *cur = task_current();
            if (cur) {
                vm_area_t *vma = (vm_area_t *)kmalloc(sizeof(vm_area_t));
                if (vma) {
                    vma->start = (uint64_t)p;
                    vma->end   = (uint64_t)p + pages * PAGE_SIZE;
                    vma->next  = cur->vm_areas;
                    cur->vm_areas = vma;
                }
            }
            return (uint64_t)p;
        }

        case HBOS_SYS_MUNMAP: {
            void *addr = (void *)f->a0;
            size_t len = (size_t)f->a1;
            if (!addr || len == 0) return (uint64_t)(-EINVAL);
            size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
            task_t *cur = task_current();
            vm_area_t **pp = cur ? &cur->vm_areas : NULL;
            while (pp && *pp) {
                if ((*pp)->start == (uint64_t)addr) {
                    for (size_t i = 0; i < pages; i++)
                        vmm_unmap_page((uint64_t)addr + i * PAGE_SIZE);
                    vm_area_t *vma = *pp;
                    *pp = vma->next;
                    kfree(vma);
                    return 0;
                }
                pp = &(*pp)->next;
            }
            return (uint64_t)(-EINVAL);
        }

        case HBOS_SYS_MPROTECT: {
            return 0;
        }

        case HBOS_SYS_BRK: {
            return user_brk((uint64_t)f->a0);
        }

        case HBOS_SYS_SETGID: {
            gid_t gid = (gid_t)f->a0;
            if (gid != 0) return (uint64_t)(-EPERM);
            return 0;
        }

        // ============================================================
        // 文件系统扩展 II (48-49)
        // ============================================================
        case HBOS_SYS_SYMLINK: {
            const char *target = (const char *)f->a0;
            const char *linkpath = (const char *)f->a1;
            if (!target || !linkpath) return (uint64_t)(-EFAULT);
            if (fs_find_file(linkpath)) return (uint64_t)(-EEXIST);
            file_t *link = fs_create_file(linkpath);
            if (!link) return (uint64_t)(-ENOSPC);
            link->type = 2;
            size_t tlen = strlen(target);
            if (tlen > link->capacity) tlen = link->capacity;
            if (fs_write_file_data(link, 0, target, (uint32_t)tlen) < 0)
                return (uint64_t)(-EIO);
            link->size = (uint32_t)tlen;
            return 0;
        }

        case HBOS_SYS_CHMOD: {
            const char *path = (const char *)f->a0;
            if (!path) return (uint64_t)(-EFAULT);
            (void)f->a1;
            struct stat st;
            if (stat(path, &st) < 0)
                return (uint64_t)(-errno);
            return 0;
        }

        // ============================================================
        // 用户/组 ID (50-53)
        // ============================================================
        case HBOS_SYS_CHOWN: {
            const char *path = (const char *)f->a0;
            if (!path) return (uint64_t)(-EFAULT);
            struct stat st;
            if (stat(path, &st) < 0)
                return (uint64_t)(-errno);
            return 0;
        }

        case HBOS_SYS_GETGROUPS: {
            return 0;
        }

        case HBOS_SYS_SETGROUPS: {
            return (uint64_t)(-EPERM);
        }

        case HBOS_SYS_GETPGID: {
            pid_t pid = (pid_t)f->a0;
            if (pid == 0) pid = (pid_t)task_get_id();
            return (uint64_t)pid;
        }

        // ============================================================
        // 时间操作扩展 (54-56)
        // ============================================================
        case HBOS_SYS_NANOSLEEP: {
            const struct timespec_req {
                uint64_t tv_sec;
                uint64_t tv_nsec;
            } *req = (const struct timespec_req *)f->a0;
            if (!req) return (uint64_t)(-EFAULT);
            if (req->tv_sec > 0)
                sleep((unsigned int)req->tv_sec);
            else if (req->tv_nsec > 0)
                usleep((useconds_t)(req->tv_nsec / 1000));
            return 0;
        }

        case HBOS_SYS_CLOCK_GETTIME: {
            int clockid = (int)f->a0;
            struct timespec_out {
                uint64_t tv_sec;
                uint64_t tv_nsec;
            } *tp = (struct timespec_out *)f->a1;
            if (!tp) return (uint64_t)(-EFAULT);
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            uint64_t tsc = ((uint64_t)hi << 32) | lo;
            tp->tv_sec = tsc / 1000000000ULL;
            tp->tv_nsec = tsc % 1000000000ULL;
            (void)clockid;
            return 0;
        }

        case HBOS_SYS_TIMES: {
            void *buf = (void *)f->a0;
            (void)buf;
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            uint64_t tsc = ((uint64_t)hi << 32) | lo;
            return (uint64_t)(tsc / 10000);
        }

        // ============================================================
        // 网络套接字 (57-63)
        // ============================================================
        case HBOS_SYS_SOCKET: {
            int domain = (int)f->a0;
            int type = (int)f->a1;
            int protocol = (int)f->a2;
            (void)domain; (void)type; (void)protocol;
            task_t *cur = task_current();
            int fd = -1;
            for (int i = 0; i < POSIX_MAX_FDS; i++) {
                if (!cur->fds[i].used) { fd = i; break; }
            }
            if (fd < 0) return (uint64_t)(-EMFILE);
            cur->fds[fd].used = true;
            cur->fds[fd].node = NULL;
            cur->fds[fd].offset = 0;
            cur->fds[fd].flags = O_RDWR;
            cur->fds[fd].type = FD_SOCKET;
            cur->fds[fd].local_port = 0;
            return (uint64_t)fd;
        }

        case HBOS_SYS_BIND: {
            int sockfd = (int)f->a0;
            const void *addr = (const void *)f->a1;
            size_t addrlen = (size_t)f->a2;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            if (!addr || addrlen < 8) return (uint64_t)(-EINVAL);
            task_t *cur = task_current();
            if (!cur || !cur->fds[sockfd].used)
                return (uint64_t)(-EBADF);
            const uint8_t *addr_bytes = (const uint8_t *)addr;
            uint16_t port = ((uint16_t)addr_bytes[2] << 8) | addr_bytes[3];
            cur->fds[sockfd].local_port = port;
            return 0;
        }

        case HBOS_SYS_LISTEN: {
            int sockfd = (int)f->a0;
            int backlog = (int)f->a1;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            (void)backlog;
            task_t *cur = task_current();
            if (!cur || !cur->fds[sockfd].used)
                return (uint64_t)(-EBADF);
            uint16_t port = cur->fds[sockfd].local_port;
            net_tcp_listen(port);
            return 0;
        }

        case HBOS_SYS_ACCEPT: {
            int sockfd = (int)f->a0;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            task_t *cur = task_current();
            if (!cur || !cur->fds[sockfd].used)
                return (uint64_t)(-EBADF);
            net_tcp_conn_t *conn = (net_tcp_conn_t *)kmalloc(sizeof(net_tcp_conn_t));
            if (!conn) return (uint64_t)(-ENOMEM);
            int ret = net_tcp_accept(cur->fds[sockfd].local_port, conn, 3000);
            if (ret < 0) {
                kfree(conn);
                return (uint64_t)(-EAGAIN);
            }
            int newfd = -1;
            for (int i = 0; i < POSIX_MAX_FDS; i++) {
                if (!cur->fds[i].used) { newfd = i; break; }
            }
            if (newfd < 0) { kfree(conn); return (uint64_t)(-EMFILE); }
            cur->fds[newfd].used  = true;
            cur->fds[newfd].type  = FD_SOCKET;
            cur->fds[newfd].node  = (vfs_node_t *)conn;
            cur->fds[newfd].flags = 0;
            return (uint64_t)newfd;
        }

        case HBOS_SYS_CONNECT: {
            int sockfd = (int)f->a0;
            const void *addr = (const void *)f->a1;
            size_t addrlen = (size_t)f->a2;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            if (!addr || addrlen < 8) return (uint64_t)(-EINVAL);
            task_t *cur = task_current();
            if (!cur || !cur->fds[sockfd].used)
                return (uint64_t)(-EBADF);
            const uint8_t *addr_bytes = (const uint8_t *)addr;
            uint32_t ip = ((uint32_t)addr_bytes[4] << 24) |
                          ((uint32_t)addr_bytes[5] << 16) |
                          ((uint32_t)addr_bytes[6] << 8) |
                          (uint32_t)addr_bytes[7];
            uint16_t port = ((uint16_t)addr_bytes[2] << 8) | addr_bytes[3];
            net_tcp_conn_t *conn = (net_tcp_conn_t *)kmalloc(sizeof(net_tcp_conn_t));
            if (!conn) return (uint64_t)(-ENOMEM);
            int ret = net_tcp_connect(ip, port, conn);
            if (ret < 0) {
                kfree(conn);
                return (uint64_t)(-ECONNREFUSED);
            }
            cur->fds[sockfd].node = (vfs_node_t *)conn;
            return 0;
        }

        case HBOS_SYS_SEND: {
            int sockfd = (int)f->a0;
            const void *buf = (const void *)f->a1;
            size_t len = (size_t)f->a2;
            int flags = (int)f->a3;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            if (!buf) return (uint64_t)(-EFAULT);
            (void)flags;
            task_t *cur = task_current();
            if (!cur || !cur->fds[sockfd].used)
                return (uint64_t)(-EBADF);
            net_tcp_conn_t *conn = (net_tcp_conn_t *)cur->fds[sockfd].node;
            if (!conn) return (uint64_t)(-ENOTCONN);
            int ret = net_tcp_send(conn, (const uint8_t *)buf, (uint32_t)len);
            if (ret < 0) return (uint64_t)(-ECONNRESET);
            return (uint64_t)ret;
        }

        case HBOS_SYS_RECV: {
            int sockfd = (int)f->a0;
            void *buf = (void *)f->a1;
            size_t len = (size_t)f->a2;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            if (!buf) return (uint64_t)(-EFAULT);
            task_t *cur = task_current();
            if (!cur || !cur->fds[sockfd].used)
                return (uint64_t)(-EBADF);
            net_tcp_conn_t *conn = (net_tcp_conn_t *)cur->fds[sockfd].node;
            if (!conn) return (uint64_t)(-ENOTCONN);
            uint32_t recv_len = 0;
            int ret = net_tcp_recv(conn, (uint8_t *)buf, (uint32_t)len, &recv_len, 10);
            if (ret < 0) return (uint64_t)(-ECONNRESET);
            return (uint64_t)recv_len;
        }

        // ============================================================
        // 系统管理 (64-67)
        // ============================================================
        case HBOS_SYS_REBOOT: {
            int cmd = (int)f->a0;
            if (cmd != 0x1234567 && cmd != 0x01234567)
                return (uint64_t)(-EINVAL);
            acpi_poweroff();
            while (1) __asm__ volatile("hlt");
            return 0;
        }

        case HBOS_SYS_SYNC: {
            extern int fs_sync(void);
            fs_sync();
            return 0;
        }

        case HBOS_SYS_MOUNT: {
            const char *src = (const char *)f->a0;
            const char *tgt = (const char *)f->a1;
            if (!tgt) return (uint64_t)(-EFAULT);
            if (src && src[0]) {
                extern int fs_mount_disk(void);
                if (fs_mount_disk() < 0) return (uint64_t)(-ENODEV);
            }
            (void)f->a2; (void)f->a3; (void)f->a4;
            return 0;
        }

        case HBOS_SYS_UMOUNT: {
            const char *tgt = (const char *)f->a0;
            if (!tgt) return (uint64_t)(-EFAULT);
            struct stat st;
            if (stat(tgt, &st) < 0) return (uint64_t)(-ENOENT);
            return 0;
        }

        // ============================================================
        // I/O 多路复用 & 目录遍历 (68-69)
        // ============================================================
        case HBOS_SYS_SELECT: {
            int nfds = (int)f->a0;
            uint64_t *readfds = (uint64_t *)f->a1;
            uint64_t *writefds = (uint64_t *)f->a2;
            int count = 0;
            if (nfds > POSIX_MAX_FDS) nfds = POSIX_MAX_FDS;
            task_t *cur = task_current();
            if (!cur) return (uint64_t)(-ESRCH);
            uint64_t rfds = readfds ? *readfds : 0;
            uint64_t wfds = writefds ? *writefds : 0;
            for (int fd = 0; fd < nfds; fd++) {
                if ((rfds & (1ULL << fd)) && cur->fds[fd].used) {
                    if (readfds) rfds |= (1ULL << fd);
                    else rfds &= ~(1ULL << fd);
                    count++;
                }
                if ((wfds & (1ULL << fd)) && cur->fds[fd].used) {
                    if (writefds) wfds |= (1ULL << fd);
                    else wfds &= ~(1ULL << fd);
                    count++;
                }
            }
            if (readfds) *readfds = rfds;
            if (writefds) *writefds = wfds;
            return (uint64_t)count;
        }

        case HBOS_SYS_GETDENTS: {
            int fd = (int)f->a0;
            struct dirent *dirp = (struct dirent *)f->a1;
            unsigned int count = (unsigned int)f->a2;
            if (fd < 0 || fd >= POSIX_MAX_FDS || !dirp)
                return (uint64_t)(-EINVAL);
            task_t *cur = task_current();
            if (!cur || !cur->fds[fd].used)
                return (uint64_t)(-EBADF);
            vfs_node_t *dir = cur->fds[fd].node;
            if (!dir || dir->type != VFS_NODE_DIR)
                return (uint64_t)(-ENOTDIR);
            extern uint32_t fs_get_count(void);
            extern file_t *fs_get_file(uint32_t index);
            uint32_t total = fs_get_count();
            unsigned int bytes_written = 0;
            for (uint32_t i = 0; i < total; i++) {
                file_t *file = fs_get_file(i);
                if (!file || !file->used) continue;
                struct dirent dent;
                dent.d_ino = (uint64_t)i;
                dent.d_off = (int64_t)i;
                dent.d_type = (file->type == 1) ? DT_DIR : DT_REG;
                size_t name_len = strlen(file->name);
                if (name_len > NAME_MAX) name_len = NAME_MAX;
                memcpy(dent.d_name, file->name, name_len);
                dent.d_name[name_len] = '\0';
                dent.d_reclen = sizeof(struct dirent) - NAME_MAX - 1 + name_len + 1;
                unsigned int reclen = sizeof(dent);
                if (bytes_written + reclen > count) break;
                memcpy((uint8_t *)dirp + bytes_written, &dent, reclen);
                bytes_written += reclen;
            }
            return (uint64_t)bytes_written;
        }

        case HBOS_SYS_OPENDIR: {
            const char *path = (const char *)f->a0;
            if (!path) return (uint64_t)(-EFAULT);
            int ret = vfs_opendir(path);
            if (ret < 0) return (uint64_t)(-ENOENT);
            return 0;
        }

        case HBOS_SYS_READDIR: {
            const char *path = (const char *)f->a0;
            char *out_name = (char *)f->a1;
            uint32_t *out_type = (uint32_t *)f->a2;
            if (!path || !out_name || !out_type) return (uint64_t)(-EFAULT);
            int ret = vfs_readdir(path, out_name, out_type);
            if (ret < 0) return (uint64_t)(-ENOENT);
            return 0;
        }

        case HBOS_SYS_CLOSEDIR: {
            const char *path = (const char *)f->a0;
            vfs_closedir(path);
            return 0;
        }

        case HBOS_SYS_SHMGET: {
            extern int shmget(int, size_t, int);
            int key = (int)f->a0;
            size_t size = (size_t)f->a1;
            int flags = (int)f->a2;
            int ret = shmget(key, size, flags);
            if (ret < 0) return (uint64_t)(-ENOSPC);
            return (uint64_t)ret;
        }

        case HBOS_SYS_SHMAT: {
            extern void *shmat(int, const void *, int);
            int shmid = (int)f->a0;
            const void *shmaddr = (const void *)f->a1;
            int flags = (int)f->a2;
            void *ret = shmat(shmid, shmaddr, flags);
            if (ret == (void *)-1) return (uint64_t)(-EINVAL);
            return (uint64_t)(uintptr_t)ret;
        }

        case HBOS_SYS_SHMDT: {
            extern int shmdt(const void *);
            const void *shmaddr = (const void *)f->a0;
            int ret = shmdt(shmaddr);
            if (ret < 0) return (uint64_t)(-EINVAL);
            return 0;
        }

        case HBOS_SYS_SHMCTL: {
            extern int shmctl(int, int, void *);
            int shmid = (int)f->a0;
            int cmd = (int)f->a1;
            void *buf = (void *)f->a2;
            int ret = shmctl(shmid, cmd, buf);
            if (ret < 0) return (uint64_t)(-EINVAL);
            return 0;
        }

        default:
            return (uint64_t)(-ENOSYS);
    }
}
