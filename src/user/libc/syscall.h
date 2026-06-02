#ifndef HBOS_USER_LIBC_SYSCALL_H
#define HBOS_USER_LIBC_SYSCALL_H

#include <stdint.h>
#include <stddef.h>

typedef long ssize_t;
typedef long off_t;
typedef int pid_t;
typedef unsigned int mode_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int useconds_t;
typedef long clock_t;
typedef unsigned long dev_t;
typedef unsigned long ino_t;
typedef unsigned long nlink_t;

struct stat {
    dev_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    off_t st_size;
    long st_atime;
    long st_mtime;
    long st_ctime;
};

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

enum {
    HBOS_SYS_READ = 0,
    HBOS_SYS_WRITE,
    HBOS_SYS_OPEN,
    HBOS_SYS_CLOSE,
    HBOS_SYS_LSEEK,
    HBOS_SYS_FSTAT,
    HBOS_SYS_STAT,
    HBOS_SYS_UNLINK,
    HBOS_SYS_ISATTY,
    HBOS_SYS_GETPID,
    HBOS_SYS_SBRK,
    HBOS_SYS_EXIT,
    HBOS_SYS_GETPPID,
    HBOS_SYS_SLEEP,
    HBOS_SYS_USLEEP,
    HBOS_SYS_UNAME,
    HBOS_SYS_GETTOD,
    HBOS_SYS_ACCESS,
    HBOS_SYS_FTRUNCATE,
    HBOS_SYS_MKDIR,
    HBOS_SYS_RMDIR,
    HBOS_SYS_GETCWD,
    HBOS_SYS_CHDIR,
    HBOS_SYS_DUP,
    HBOS_SYS_GETEUID,
    HBOS_SYS_GETEGID,
    HBOS_SYS_GETTID,
    HBOS_SYS_DUP2,
    HBOS_SYS_PIPE,
    HBOS_SYS_FCNTL,
    HBOS_SYS_IOCTL,
    HBOS_SYS_READLINK,
    HBOS_SYS_FORK,
    HBOS_SYS_EXECVE,
    HBOS_SYS_WAITPID,
    HBOS_SYS_KILL,
    HBOS_SYS_GETUID,
    HBOS_SYS_GETGID,
    HBOS_SYS_SETUID,
    HBOS_SYS_SIGNAL,
    HBOS_SYS_SIGACTION,
    HBOS_SYS_SIGPROCMASK,
    HBOS_SYS_PAUSE,
    HBOS_SYS_MMAP,
    HBOS_SYS_MUNMAP,
    HBOS_SYS_MPROTECT,
    HBOS_SYS_BRK,
    HBOS_SYS_SETGID,
    HBOS_SYS_SYMLINK,
    HBOS_SYS_CHMOD,
    HBOS_SYS_CHOWN,
    HBOS_SYS_GETGROUPS,
    HBOS_SYS_SETGROUPS,
    HBOS_SYS_GETPGID,
    HBOS_SYS_NANOSLEEP,
    HBOS_SYS_CLOCK_GETTIME,
    HBOS_SYS_TIMES,
    HBOS_SYS_SOCKET,
    HBOS_SYS_BIND,
    HBOS_SYS_LISTEN,
    HBOS_SYS_ACCEPT,
    HBOS_SYS_CONNECT,
    HBOS_SYS_SEND,
    HBOS_SYS_RECV,
    HBOS_SYS_REBOOT,
    HBOS_SYS_SYNC,
    HBOS_SYS_MOUNT,
    HBOS_SYS_UMOUNT,
    HBOS_SYS_SELECT,
    HBOS_SYS_GETDENTS,
    HBOS_SYS_OPENDIR,
    HBOS_SYS_READDIR,
    HBOS_SYS_CLOSEDIR,
    HBOS_SYS_SHMGET,
    HBOS_SYS_SHMAT,
    HBOS_SYS_SHMDT,
    HBOS_SYS_SHMCTL,
};

long __syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);
long __syscall3(long nr, long a0, long a1, long a2);
long __syscall1(long nr, long a0);

#endif