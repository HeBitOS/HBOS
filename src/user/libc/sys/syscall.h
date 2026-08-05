#ifndef HBOS_USER_LIBC_SYS_SYSCALL_H
#define HBOS_USER_LIBC_SYS_SYSCALL_H

/*
 * Linux x86-64 syscall numbers used by common libc, Qt and KDE runtime
 * code.  syscall() translates these to the compact HBOS ABI; applications
 * do not need to include HBOS syscall numbers.
 */
#define SYS_read              0
#define SYS_write             1
#define SYS_open              2
#define SYS_close             3
#define SYS_stat              4
#define SYS_fstat             5
#define SYS_poll              7
#define SYS_lseek             8
#define SYS_mmap              9
#define SYS_mprotect         10
#define SYS_munmap           11
#define SYS_brk              12
#define SYS_rt_sigaction     13
#define SYS_rt_sigprocmask   14
#define SYS_ioctl            16
#define SYS_readv            19
#define SYS_writev           20
#define SYS_access           21
#define SYS_pipe             22
#define SYS_sched_yield      24
#define SYS_dup              32
#define SYS_dup2             33
#define SYS_nanosleep        35
#define SYS_getpid           39
#define SYS_socket           41
#define SYS_connect          42
#define SYS_accept           43
#define SYS_sendto           44
#define SYS_recvfrom         45
#define SYS_sendmsg          46
#define SYS_recvmsg          47
#define SYS_bind             49
#define SYS_listen           50
#define SYS_socketpair       53
#define SYS_setsockopt       54
#define SYS_getsockopt       55
#define SYS_clone            56
#define SYS_fork             57
#define SYS_execve           59
#define SYS_exit             60
#define SYS_wait4            61
#define SYS_kill             62
#define SYS_uname            63
#define SYS_fcntl            72
#define SYS_ftruncate        77
#define SYS_getcwd           79
#define SYS_chdir            80
#define SYS_mkdir            83
#define SYS_rmdir            84
#define SYS_unlink           87
#define SYS_readlink         89
#define SYS_chmod            90
#define SYS_chown            92
#define SYS_gettimeofday     96
#define SYS_getuid          102
#define SYS_getgid          104
#define SYS_geteuid         107
#define SYS_getegid         108
#define SYS_getppid         110
#define SYS_getgroups       115
#define SYS_setgroups       116
#define SYS_getpgid         121
#define SYS_arch_prctl      158
#define SYS_gettid          186
#define SYS_futex           202
#define SYS_getdents64      217
#define SYS_set_tid_address 218
#define SYS_clock_gettime   228
#define SYS_exit_group      231
#define SYS_epoll_wait      232
#define SYS_epoll_ctl       233
#define SYS_openat          257
#define SYS_newfstatat      262
#define SYS_readlinkat      267
#define SYS_set_robust_list 273
#define SYS_get_robust_list 274
#define SYS_epoll_pwait     281
#define SYS_accept4         288
#define SYS_eventfd2        290
#define SYS_epoll_create1   291
#define SYS_dup3            292
#define SYS_pipe2           293
#define SYS_getrandom       318
#define SYS_memfd_create    319
#define SYS_shutdown         48

long syscall(long linux_number, ...);

#endif
