# User — 用户空间 (`src/user/`)

> Ring3 用户程序运行时, 动态链接器, 系统调用, libc, 内建用户程序

## 文件清单

| 文件 | 职责 |
|------|------|
| `app.h` | 用户程序接口: app_run, app_kill |
| `app_runtime.c` | 用户程序运行时: 创建 ring3 任务, 加载 ELF, 初始化 |
| `crt0.c` | C 运行时入口 (_start → main), argc/argv 设置 |
| `ldso.c` + `ldso.h` | 动态链接器: 加载 .so, 符号解析, 重定位 |
| `syscall.c` + `syscall.h` | 系统调用: int 0x80 跳板函数 |
| `libc/` | 用户空间 libc (stdio, stdlib, string, socket, dlfcn) |
| `progs/` | 内建用户程序 (cat, cp, echo, hello, ls, ping, sh, wget) |

## 用户程序生命周期

```c
// app.h / app_runtime.c
int app_run(const char *path, int argc, char *argv[]);
// 1. vfs_open(path)
// 2. 读 ELF header, 验证 magic, x86-64
// 3. 分配用户态页表 (VMM user pages for code/data/bss/stack)
// 4. 加载 segments (PT_LOAD)
// 5. ldso_resolve (动态链接)
// 6. task_create(entry, RING3) → 创建 ring3 任务
// 7. 任务 yield → 切换到用户态执行

void app_kill(uint32_t task_id);

// 信号:
void app_signal(uint32_t task_id, int sig);
```

## 系统调用 (`syscall.c`)

```c
// int 0x80 系统调用号:
enum {
    SYS_read    = 0,
    SYS_write   = 1,
    SYS_open    = 2,
    SYS_close   = 3,
    SYS_stat    = 4,
    SYS_fstat   = 5,
    SYS_lseek   = 8,
    SYS_mmap    = 9,
    SYS_exit    = 60,
    SYS_getpid  = 39,
    SYS_socket  = 41,
    SYS_connect = 42,
    SYS_sendto  = 44,
    SYS_recvfrom= 45,
    SYS_bind    = 49,
    SYS_nanosleep= 35,
    SYS_getcwd  = 79,
    SYS_chdir   = 80,
    SYS_mkdir   = 83,
    SYS_rmdir   = 84,
    SYS_unlink  = 87,
    SYS_readdir = 89,
    SYS_ioctl   = 16,
    SYS_fcntl   = 72,
    // ... 约 30+ 系统调用
};

// 用户空间调用 (syscall.h):
static inline int64_t syscall6(uint64_t num, uint64_t a1, ...) {
    int64_t ret;
    register uint64_t r10 asm("r10") = a4;
    register uint64_t r8  asm("r8")  = a5;
    register uint64_t r9  asm("r9")  = a6;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9) : "memory");
    return ret;
}
```

## LDSO 动态链接器

```c
// ldso.c
int ldso_load(const char *path, uint64_t base);
int ldso_resolve(const char *name, uint64_t *addr);
// 支持: .so 格式, R_X86_64_RELATIVE, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT
```

## 内建用户程序 (`progs/`)

| 程序 | 源文件 | 功能 |
|------|--------|------|
| `cat` | `cat.c` | 读取并打印文件内容 |
| `cp` | `cp.c` | 复制文件 |
| `echo` | `echo.c` | 打印字符串 |
| `hello` | `hello.c` | "Hello, World!" |
| `ls` | `ls.c` | 列出目录 |
| `ping` | `ping.c` | ICMP echo 请求 |
| `sh` | `sh.c` | 简单 Shell（内建 fork/exec 风格） |
| `wget` | `wget.c` | HTTP GET 下载 |

## 用户空间 libc (`user/libc/`)

```c
// 提供 POSIX 子集:
stdio:  printf, sprintf, puts, putchar, getchar
stdlib:  malloc, free, atoi, strtol
string:  strlen, strcpy, strcmp, memcpy, memset
socket:  socket, connect, send, recv
dlfcn:   dlopen, dlsym
```

## 关键常量

```c
#define USER_STACK_SIZE   (64 * 1024)   // 64KB 用户态栈
#define MAX_USER_TASKS    16
#define ELF_MAGIC         0x464C457F    // "\x7fELF"
```

## 当前状态

| 项 | 状态 |
|----|------|
| ELF 加载 | ✅ x86-64 ELF |
| 动态链接 | ✅ LDSO |
| 系统调用 | ✅ ~30 个 |
| fork/exec | ❌ (协作式调度, 无 COW) |
| mmap | ⚠️ 基础 |
| 共享库 | ✅ .so 格式 |
