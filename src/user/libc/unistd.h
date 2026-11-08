/**
 * @file    unistd.h
 * @brief   导出 40 个接口（0 类型 / 10 宏 / 30 函数）
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    F_OK           常量 / 特性宏
 *         R_OK           常量 / 特性宏
 *         W_OK           常量 / 特性宏
 *         X_OK           常量 / 特性宏
 *         STDIN_FILENO   常量 / 特性宏
 *         STDOUT_FILENO  常量 / 特性宏
 *         STDERR_FILENO  常量 / 特性宏
 *         SEEK_SET       SEEK设置
 *         SEEK_CUR       常量 / 特性宏
 *         SEEK_END       常量 / 特性宏
 *   函数  sleep           休眠整秒
 *         usleep          休眠微秒
 *         getpid          取当前进程号
 *         getcwd          取当前工作目录
 *         chdir           切换工作目录
 *         unlink          删除目录项
 *         symlink         （待补）
 *         readlink        （待补）
 *         read            从文件描述符读
 *         write           向文件描述符写
 *         lseek           移动 fd 读写位置
 *         ftruncate       按 fd 截断
 *         close           关闭 fd
 *         rmdir           删空目录
 *         pipe2           （待补）
 *         execve          替换当前进程映像
 *         execvp          按 PATH 查找并替换映像
 *         access          检查可达性
 *         isatty          fd 是否为终端
 *         rename          重命名路径
 *         getuid          HBOS-specific: HBOS's…
 *         geteuid         取有效用户号
 *         getgid          （待补）
 *         getegid         取有效组号
 *         setuid          设置用户号
 *         setgid          （待补）
 *         getgroups       （待补）
 *         setgroups       （待补）
 *         getpgid         （待补）
 *         hax_app_exists  （待补）
 */
#ifndef HBOS_USER_LIBC_UNISTD_H
#define HBOS_USER_LIBC_UNISTD_H

#include <stddef.h>
#include "syscall.h"

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

unsigned int sleep(unsigned int seconds);
int          usleep(unsigned int useconds);
pid_t        getpid(void);
char        *getcwd(char *buf, size_t size);
int          chdir(const char *path);
int          unlink(const char *path);
int          symlink(const char *target, const char *linkpath);
ssize_t      readlink(const char *path, char *buf, size_t bufsiz);

ssize_t      read(int fd, void *buf, size_t count);
ssize_t      write(int fd, const void *buf, size_t count);
off_t        lseek(int fd, off_t offset, int whence);
int          ftruncate(int fd, off_t length);
int          close(int fd);
int          rmdir(const char *path);
int          pipe2(int pipefd[2], int flags);

/* Real exec — see unistd.c: on success this never returns (the calling
 * task is replaced/terminated by the kernel), -1 on failure. */
int          execve(const char *path, char *const argv[], char *const envp[]);
int          execvp(const char *file, char *const argv[]);

int          access(const char *path, int mode);
int          isatty(int fd);

/* Same-filesystem file rename.  ramfs/HBFS relink metadata in place;
 * ext2/FAT32 currently use the filesystem copy/delete fallback. */
int          rename(const char *oldpath, const char *newpath);

/* HBOS-specific: HBOS's "programs" are .hax apps looked up by name in an
 * embedded kernel registry (src/user/hax.c's hax_app_find()), not files in
 * the ramfs. There is no /bin directory listing them, so PATH-searching
 * tools (which, executable_exists()) must ask the kernel directly whether
 * a name is a known app rather than stat()'ing a path. Returns 1 if found,
 * 0 otherwise. */
/* Multi-user identity.  HBOS now carries per-task uid/gid/euid/egid and
 * supplementary groups; getuid()/setuid() etc. round-trip through the
 * kernel's native syscall table. */
uid_t        getuid(void);
uid_t        geteuid(void);
gid_t        getgid(void);
gid_t        getegid(void);
int          setuid(uid_t uid);
int          setgid(gid_t gid);
int          getgroups(int size, gid_t list[]);
int          setgroups(int size, const gid_t list[]);
pid_t        getpgid(pid_t pid);

int          hax_app_exists(const char *name);

#endif
