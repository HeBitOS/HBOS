/**
 * @file    syscall.h
 * @brief   系统调用号表与内核侧分发
 *
 * @layer   user
 * @owner   src/user/libc/sys/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    SYS_read               常量 / 特性宏
 *         SYS_write              常量 / 特性宏
 *         SYS_open               常量 / 特性宏
 *         SYS_close              常量 / 特性宏
 *         SYS_stat               常量 / 特性宏
 *         SYS_fstat              常量 / 特性宏
 *         SYS_poll               常量 / 特性宏
 *         SYS_lseek              常量 / 特性宏
 *         SYS_mmap               常量 / 特性宏
 *         SYS_mprotect           常量 / 特性宏
 *         SYS_munmap             常量 / 特性宏
 *         SYS_brk                常量 / 特性宏
 *         SYS_rt_sigaction       常量 / 特性宏
 *         SYS_rt_sigprocmask     常量 / 特性宏
 *         SYS_ioctl              常量 / 特性宏
 *         SYS_readv              常量 / 特性宏
 *         SYS_writev             常量 / 特性宏
 *         SYS_access             常量 / 特性宏
 *         SYS_pipe               常量 / 特性宏
 *         SYS_sched_yield        常量 / 特性宏
 *         SYS_dup                常量 / 特性宏
 *         SYS_dup2               常量 / 特性宏
 *         SYS_nanosleep          常量 / 特性宏
 *         SYS_getpid             常量 / 特性宏
 *         SYS_socket             常量 / 特性宏
 *         SYS_connect            常量 / 特性宏
 *         SYS_accept             常量 / 特性宏
 *         SYS_sendto             常量 / 特性宏
 *         SYS_recvfrom           常量 / 特性宏
 *         SYS_sendmsg            常量 / 特性宏
 *         SYS_recvmsg            常量 / 特性宏
 *         SYS_bind               常量 / 特性宏
 *         SYS_listen             常量 / 特性宏
 *         SYS_getsockname        常量 / 特性宏
 *         SYS_getpeername        常量 / 特性宏
 *         SYS_socketpair         常量 / 特性宏
 *         SYS_setsockopt         常量 / 特性宏
 *         SYS_getsockopt         常量 / 特性宏
 *         SYS_clone              常量 / 特性宏
 *         SYS_fork               常量 / 特性宏
 *         SYS_execve             常量 / 特性宏
 *         SYS_exit               常量 / 特性宏
 *         SYS_wait4              常量 / 特性宏
 *         SYS_kill               常量 / 特性宏
 *         SYS_uname              常量 / 特性宏
 *         SYS_fcntl              常量 / 特性宏
 *         SYS_ftruncate          常量 / 特性宏
 *         SYS_getcwd             常量 / 特性宏
 *         SYS_statfs             常量 / 特性宏
 *         SYS_fstatfs            常量 / 特性宏
 *         SYS_sched_getaffinity  常量 / 特性宏
 *         SYS_signalfd           常量 / 特性宏
 *         SYS_signalfd4          常量 / 特性宏
 *         SYS_mremap             常量 / 特性宏
 *         SYS_chdir              常量 / 特性宏
 *         SYS_rename             常量 / 特性宏
 *         SYS_mkdir              常量 / 特性宏
 *         SYS_rmdir              常量 / 特性宏
 *         SYS_unlink             常量 / 特性宏
 *         SYS_symlink            常量 / 特性宏
 *         SYS_readlink           常量 / 特性宏
 *         SYS_chmod              常量 / 特性宏
 *         SYS_chown              常量 / 特性宏
 *         SYS_gettimeofday       常量 / 特性宏
 *         SYS_getuid             常量 / 特性宏
 *         SYS_getgid             常量 / 特性宏
 *         SYS_geteuid            常量 / 特性宏
 *         SYS_getegid            常量 / 特性宏
 *         SYS_getppid            常量 / 特性宏
 *         SYS_getgroups          常量 / 特性宏
 *         SYS_setgroups          常量 / 特性宏
 *         SYS_getpgid            常量 / 特性宏
 *         SYS_arch_prctl         常量 / 特性宏
 *         SYS_gettid             常量 / 特性宏
 *         SYS_futex              常量 / 特性宏
 *         SYS_getdents64         常量 / 特性宏
 *         SYS_set_tid_address    常量 / 特性宏
 *         SYS_clock_gettime      常量 / 特性宏
 *         SYS_exit_group         常量 / 特性宏
 *         SYS_epoll_wait         常量 / 特性宏
 *         SYS_epoll_ctl          常量 / 特性宏
 *         SYS_openat             常量 / 特性宏
 *         SYS_renameat           常量 / 特性宏
 *         SYS_symlinkat          常量 / 特性宏
 *         SYS_newfstatat         常量 / 特性宏
 *         SYS_readlinkat         常量 / 特性宏
 *         SYS_set_robust_list    常量 / 特性宏
 *         SYS_get_robust_list    常量 / 特性宏
 *         SYS_epoll_pwait        常量 / 特性宏
 *         SYS_accept4            常量 / 特性宏
 *         SYS_eventfd2           常量 / 特性宏
 *         SYS_epoll_create1      常量 / 特性宏
 *         SYS_dup3               常量 / 特性宏
 *         SYS_pipe2              常量 / 特性宏
 *         SYS_renameat2          常量 / 特性宏
 *         SYS_getrandom          常量 / 特性宏
 *         SYS_memfd_create       常量 / 特性宏
 *         SYS_shutdown           常量 / 特性宏
 *   函数  syscall     （待补）
 */
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
#define SYS_getsockname      51
#define SYS_getpeername      52
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
#define SYS_statfs           137
#define SYS_fstatfs          138
#define SYS_sched_getaffinity 204
#define SYS_signalfd          282
#define SYS_signalfd4         289
#define SYS_mremap            25
#define SYS_chdir            80
#define SYS_rename           82
#define SYS_mkdir            83
#define SYS_rmdir            84
#define SYS_unlink           87
#define SYS_symlink          88
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
#define SYS_renameat        264
#define SYS_symlinkat       266
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
#define SYS_renameat2       316
#define SYS_getrandom       318
#define SYS_memfd_create    319
#define SYS_shutdown         48

long syscall(long linux_number, ...);

#endif
