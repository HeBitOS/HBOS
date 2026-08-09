# HBOS Linux/KDE 兼容层

## 目标

最终目标是在 **不修改 KDE、Qt、KDE Frameworks 和 Plasma 上游源码** 的
前提下运行 KDE 桌面。兼容工作全部放在 HBOS 的 Linux ABI、libc、进程、
文件系统和图形协议层，不把 HBOS API 写进 KDE。

这不是在 HBOS 内嵌一个 Linux 虚拟机，也不是复制一套 Linux VFS。Linux
调用被薄层翻译为 HBOS 原生对象和内核服务，因此原生 HBOS 应用仍走原有
快路径。

## 轻量架构

```text
KDE / Qt / 普通 Linux 软件
        │  POSIX、Linux SYS_*、Wayland、D-Bus
        ▼
HBOS libc + Linux ABI 翻译层
        │  O(1) syscall 号码映射
        ▼
HBOS 原生 fd / task / VFS / socket / HIVE
```

设计约束：

- 不运行兼容守护进程，不经 IPC 转发普通系统调用。
- 不建立第二套 fd 表或 VFS。
- `eventfd` 和 `epoll` 使用固定容量内核表，不在等待热路径动态分配。
- `inotify` 使用 8 个实例、每实例 16 个 watch/32 条事件的固定表；没有实例时
  VFS 通知热路径仅做一次计数检查。
- 当前每进程 fd 上限为 128，`poll`/`epoll_wait` 的直接扫描比树或哈希表更便宜；
  fd 上限提高到 64 以上时再改为就绪队列。
- 无事件时调用调度器让出 CPU，不持续忙等。
- HBOS 私有 GUI、HAX 和网络 syscall 编号保持不变。

## 当前已经具备

| 能力 | 状态 | 说明 |
|---|---|---|
| Linux `SYS_*` 源码兼容入口 | 初版 | 常用 x86-64 syscall 号码在 libc 内 O(1) 翻译 |
| 原生 x86-64 `syscall` 入口 | 可用初版 | 高地址 ET_EXEC 与静态 PIE 已在 QEMU 运行 |
| 静态 PIE / auxv / RELATIVE relocation | 可用初版 | 支持 ET_DYN、Linux auxv 和 `R_X86_64_RELATIVE` |
| `PT_INTERP` 启动 | 可用初版 | 从 VFS 流式映射 ET_DYN 解释器，传递 `AT_BASE/AT_ENTRY/AT_PHDR`；最小解释器已在 QEMU 运行 |
| 真实 musl 动态程序 | 可用基线 | 631 KiB 主 PIE 与 760 KiB loader/libc 均从零拷贝只读 VFS 流式启动，`DT_NEEDED=libc.so` hello 在 QEMU 通过 |
| 真实 glibc 动态程序 | 可用基线 | 未修改 glibc 2.43 loader 与 2.1 MiB libc 的符号版本、malloc、时钟及基础 pthread/静态 TLS 已在 QEMU 通过 |
| `dlopen`/`dlsym`/`dlclose` | 可用初版 | VFS 流式映射、递归 DT_NEEDED、引用计数、ring3 INIT/FINI、动态 TLS、GNU 符号版本、构造函数重入及四线程并发已由 QEMU 验证 |
| `mmap`/`munmap`/`mprotect` 与 W^X | 可用初版 | CPUID 门控 NXE；真实 R/W/X/none 权限、部分 VMA 拆分、匿名页与任务退出回收已在 QEMU 通过 |
| Linux `sysinfo` | 可用初版 | 返回 PMM 总量/空闲量、uptime、进程数和 `mem_unit=1`，使用精确 x86-64 112 字节 ABI |
| 进程/资源常用 ABI | 可用初版 | `prlimit64/getrusage/prctl/tgkill/madvise/clock_nanosleep` 已有原生 x86-64 边界转换和 QEMU 回归 |
| Linux 结构体 ABI | 可用初版 | 已转换 `stat`、`getdents64`、向量 I/O 与消息 socket ABI |
| 轻量只读 procfs | 可用初版 | `self`、`exe`、`status`、`maps`、`fd`、`cmdline`、`meminfo`、`cpuinfo`、`uptime` 由实时任务/VMA/PMM/fd 元数据生成 |
| 轻量只读 sysfs | 可用初版 | 提供 CPU online/present/possible、cpu0 topology，以及 `lo`/`eth0` 的地址、链路、MTU、索引和类型；CPU 数、MAC 与链路状态取自实时内核对象 |
| `openat` / `newfstatat` / `readlinkat` | 可用初版 | 支持 `AT_FDCWD`、绝对路径忽略 dirfd、真实目录 fd 下的相对路径、`AT_EMPTY_PATH` 与 `AT_SYMLINK_NOFOLLOW`；复用 fd 中已有的规范 VFS 路径，无第二套路径表 |
| `symlink` / `symlinkat` / `lstat` | 可用 ramfs/HBFS 基线 | 相对/绝对目标和中间路径链接可固定栈展开，最多八层；`open(..., O_NOFOLLOW)` 返回 `ELOOP`，`getdents` 返回 `DT_LNK`；无链接时保留单次查表快路径；ext2/FAT32 明确返回 `EOPNOTSUPP` |
| `rename` / `renameat` / `renameat2` | 可用 ramfs/HBFS 基线 | 支持普通文件和目录整树改名、空目录覆盖、`RENAME_NOREPLACE`，以及两端独立 dirfd；目录后代、打开 fd 和 cwd 同步迁移；ext2/FAT32 普通文件暂为 copy-delete，目录返回 `EOPNOTSUPP` |
| `poll` / `pipe2` | 可用初版 | 管道支持 `O_NONBLOCK` |
| `eventfd` | 可用初版 | 支持 semaphore、nonblock、cloexec 标志 |
| `epoll_create1/ctl/wait` | 可用初版 | 复用原生 fd，就绪扫描无额外复制 |
| `inotify_init1/add_watch/rm_watch` | 可用初版 | 原生 syscall 253/254/255/294；创建、修改、删除、自删除、`IN_IGNORED`、`IN_MOVED_FROM/TO` 同 cookie、`IN_MOVE_SELF`、文件及目录后代 watch 改名跟随、非阻塞读及 poll/epoll 就绪已由 QEMU 验证 |
| `futex` | 可用初版 | 支持 `WAIT`/`WAKE`、bitset、private 标志；真实 musl condvar/rwlock/once 与超时已通过 |
| `sched_yield` | 可用 | 直接进入 HBOS 调度器 |
| `getrandom` | 可用初版 | 使用 CPU RDRAND；不可用时明确返回 `ENOSYS` |
| `clone` / `clone3` / TLS / TID | 可用初版 | 共享 VM/fd/mm、`CLONE_SETTLS`、child TID 清理、futex wake、两种原生子返回上下文，以及真实 musl pthread 与 `dlopen` 库 General Dynamic TLS |
| Linux 信号 ABI | 可用初版 | `rt_sigaction`/mask、普通 ring3 handler、`SA_RESTORER`/`rt_sigreturn` 与同步 page-fault `SIGSEGV` 已通过 QEMU；handler 可 `mprotect` 后重试故障指令。`SA_SIGINFO` 三参 handler、`siginfo_t/ucontext_t` 用户帧、`sigaltstack`（131）与嵌套递送（上限 8 层）已由 `linux_signal_siginfo` QEMU 用例覆盖；ucontext 布局按 glibc 2.43 x86-64 头文件与宿主实测对齐（`uc_mcontext@40`、`uc_sigmask@296`、内核布局 432 字节）。FPU 状态不随 ucontext 保存（`fpregs=NULL`、`uc_flags=0`），作为已知限制 |
| Linux `fork` / COW | 可用初版 | 子进程从原生 syscall 现场以 `RAX=0` 恢复；私有 owned 4 KiB 页由 PMM 引用计数共享，写故障按页复制，单引用快路径不复制；`mprotect`/`MADV_DONTNEED` 会先私有化 |
| AF_UNIX / D-Bus 传输底座 | 可用初版 | stream、pathname/abstract address、peer credentials、`getsockname/getpeername`、`sendmsg/recvmsg`、`SCM_RIGHTS`；地址截断仍回报完整所需长度，accept 返回真实连接方地址 |
| Wayland 共享缓冲底座 | 可用初版 | `memfd_create`、非零 `ftruncate`、零复制 `MAP_SHARED` 与 fd 传递 |
| fork 后兼容 fd 引用 | 可用 | eventfd/epoll/socket/memfd 引用随 fd 表与映射生命周期回收 |

“原生 syscall 入口可用”表示专门构建的静态 Linux ELF 已经可以直接运行；
由于完整 TLS ABI、pthread 取消/异常恢复和复杂 glibc 依赖尚未完成，不能
把它表述为任意发行版 Linux 二进制都已兼容。

用户态翻译器采用按需链接，避免让原生 HAX 应用承担无用体积：

```sh
make linux-compat-lib
# 需要 Linux 源码兼容接口的程序再链接 build/user/libhboslinux.a
```

用户态翻译器按需链接，未使用它的 HBOS/HIVE/HPT 程序不会承担用户态兼容
代码体积。事件表、Unix socket 与 memfd 元数据均有固定上限；共享像素页仅
在 `ftruncate` 时按实际大小分配，映射时直接复用物理页而不复制整帧。

## KDE 不修改源码的路线

### 阶段 1：事件循环与同步（基础已完成）

完成 `poll`、`epoll`、`eventfd`、`futex`、非阻塞管道、单调时钟和随机源。
这是 Qt event dispatcher、D-Bus 和线程运行时的底座。

### 阶段 2：线程与进程 ABI（线程快路径已完成）

- 已完成 HBOS libc `clone` 共享地址空间线程与 TLS/FS base；原生 x86-64
  syscall 56 和 `clone3` 也支持 pthread 形态的共享 VM 线程，从同一
  `SYSCALL` 返回点以 `RAX=0`
  恢复子线程新栈、callee-saved 寄存器及 FS base，并实现 parent/child TID
  写入、退出清零和 futex 唤醒。可扩展 `clone_args` 尾部执行零值校验；
  syscall 56 还兼容 musl 保留的 `CLONE_DETACHED` no-op。未修改 musl 1.2.5
  已用四个 pthread 各执行 256 次 mutex 保护更新，并验证 create/join、调度
  让步、condvar 等待/广播、rwlock、`pthread_once`、realtime 绝对超时、
  FPU 舍入环境继承、静态 TLS 初值与线程隔离、返回值和 TID 清理。
  process-shared/优先级继承同步变体、pidfd、cgroup、指定
  TID、namespace 及非线程进程形态仍明确拒绝。
- `FUTEX_WAIT_BITSET/WAKE_BITSET` 已使用固定 waiter 表实现，支持按掩码
  唤醒和单调时钟绝对超时。
- `set_robust_list/get_robust_list` 与线程退出时的 `OWNER_DIED` 标记、定向
  唤醒已完成；链表遍历设 2048 项硬上限，退出路径不分配内存。
- 内核任务槽从 16 提升到 64、每进程 fd 从 32 提升到 128；两者仍为固定
  BSS 表，先覆盖 Chromium 单进程内容壳和 KDE 基础线程负载。
- 抢占、公平调度、进程组、session、作业控制和更完整信号帧。
- COW fork 已完成初版；继续补 `vfork`/`waitid`、跨核 TLB shootdown、OOM
  回滚与更完整资源限制。

### 阶段 3：Linux ELF 二进制 ABI（动态启动基线已打通）

- 已完成 x86-64 `syscall` 指令入口、静态 PIE、auxv 与相对重定位。
- GNU hash 的 bloom/bucket/chain 查找与符号计数已接入现有轻量动态库加载器。
- `PT_INTERP` 已支持绝对路径校验、VFS 解析、ET_DYN 程序头校验和按 PT_LOAD
  段流式映射；内核以解释器入口启动并向其传入主程序的 `AT_ENTRY/AT_PHDR`
  以及解释器 `AT_BASE`。`linux_dynamic` + `linux_interp` QEMU 用例已通过。
- 内嵌兼容资产可注册为只读静态 VFS 文件，读取直接引用内核 blob，不复制到
  ramfs、没有 128 KiB 文件限制，也不占用 2 MiB 内核 heap。基于这个路径，
  Ubuntu musl 1.2.5 的 760,368 字节 `libc.so` 已同时作为
  `/lib/ld-musl-x86_64.so.1` 与 `/lib/libc.so` 暴露，真实动态 hello 的
  `PT_INTERP`、`DT_NEEDED=libc.so`、自重定位和 libc `write()` 已在 QEMU 通过。
  下一步是更复杂的共享库图、TLS ABI 扩展和 vDSO。
- ELF 启动器会在切换到新地址空间前对 `argv/envp` 做 64 KiB 有界快照，
  避免 `execve` 在新 CR3 下读取旧进程指针；加固后已通过 ring3 QEMU 用例。
- Linux `execve` 不再把整个主 ELF 读入 512 KiB 内核缓冲：只保留最大 64 KiB
  的 ELF/program-header 快照，再按 PT_LOAD 以 64 KiB 块从 VFS 写入最终用户页。
  631,432 字节 musl PIE 已由小型 Linux 启动器调用 `execve("/linux_musl")`，
  经主程序流式加载、`PT_INTERP` 和 libc 自重定位后在 QEMU 输出 PASS。
  原生 HAX 内存加载仍保留零额外拷贝快路径。
- HBOS `dlopen` 已共用内存/VFS 数据源，只读取最大 64 KiB 的 ELF/program
  headers，再按 PT_LOAD 以 64 KiB 块写入用户映射；同时修复了旧路径对每个
  映射页重复分配一个未使用物理页的泄漏。628,000 字节 GNU-hash `.so` 已通过
  `dlopen → dlsym → 调用导出函数 → dlclose` QEMU 回归。
- `DT_NEEDED` 已支持最多 16 个直接依赖的递归装载，按父对象目录、`/lib`、根
  目录搜索；同一 `task_mm` 内按规范路径去重并引用计数，`dlclose` 可级联释放
  只由该对象持有的依赖。装载环当前明确失败，避免引用环泄漏。强符号解析失败
  会让 `dlopen` 失败，只有未解析弱符号保留零值。两层共享库的 JUMP_SLOT、
  重复打开、独立叶子句柄和级联卸载由 `linux_dlopen_deps` 在 QEMU 覆盖。
  `DT_INIT`、`.init_array`、`.fini_array`、`DT_FINI` 也已按依赖顺序完成：
  内核仅验证并枚举映射内入口，用户 libc 通过追加的
  `HBOS_SYS_DLINIT_NEXT/HBOS_SYS_DLFINI_NEXT` 在 ring3 执行，绝不让内核以
  ring0 调用用户代码。重复打开不重复构造，析构仅在最后引用关闭时执行；
  两轮 16 事件的精确顺序已由同一 QEMU 用例验证。用户 libc 现以 TID 为所有者
  的递归事务锁覆盖从内核装载、IFUNC 到构造/析构的完整窗口，因此构造函数可
  同线程再次 `dlopen`，其他线程以 `sched_yield` 等待；`dlerror` 也改为每线程
  状态。四线程各 64 轮共享句柄、查符号与引用增减压力已通过，且 INIT/FINI
  仍只执行一次。内核跨地址空间对象链表使用禁止抢占的短自旋临界区，装载地址
  通过原子 CAS 预留。进程异常退出时的析构、线程被取消/杀死时的锁恢复和
  `dlmopen` 加载命名空间仍未完成。
- 共享库 `PT_TLS` 已有 x86-64 General Dynamic 初版：loader 处理
  `R_X86_64_DTPMOD64/R_X86_64_DTPOFF64`，用户 libc 提供
  `__tls_get_addr`，每个线程第一次访问模块时才按 `p_align` 分配并复制
  `.tdata`、清零 `.tbss`，线程退出即回收对应页。每任务固定 64 个模块槽，
  未访问模块没有实例开销。`linux_dlopen_deps` 使用 64 字节对齐的真实
  `__thread` 变量，验证父子线程初值、写隔离、`.tbss` 清零和退出回收。
  Initial/Local Exec、TLSDESC、TLS destructor 与 loader generation 仍未实现，
  因此这是可验证基线而非完整 ELF TLS ABI。
- GNU symbol versioning 已解析并严格校验 `DT_VERSYM/DT_VERDEF/DT_VERNEED`
  及其计数、链表和字符串边界。普通 PLT/GOT 与 TLS 重定位都会按依赖方要求的
  `name@VERSION` 选择定义，普通 `dlsym` 只返回非 hidden 默认版本，GNU
  `dlvsym` 可显式选择版本且缺失版本失败。回归库由真实 linker version script
  同时生成 `@HBOS_1.0`、`@@HBOS_2.0`，并覆盖函数和
  `DTPMOD64/DTPOFF64` TLS 符号。symbol audit namespace 尚未完成。
- GNU IFUNC 与 `R_X86_64_IRELATIVE` 已采用延迟解析：内核登记 resolver 和
  重定位目标，用户 libc 在构造函数之前于 ring3 调用 resolver，再由追加式
  `DLIFUNC_NEXT/APPLY` 系统调用验证返回地址确实是用户态可执行映射并提交结果。
  resolver 记录与目标按实际数量动态分配，应用后立即释放目标节点。测试同时
  覆盖依赖库 `STT_GNU_IFUNC`、本地 `IRELATIVE` 和 `dlsym`，resolver 会读取
  `CS.RPL`，若被错误地在 ring0 调用就返回失败实现，因此 PASS 能证明权限边界。
- `dlopen` 句柄现绑定到所属 `task_mm`：其他地址空间不会参与 handle 校验或
  符号解析；最后一个共享该 mm 的线程退出时，loader 映射与元数据会一起清理。
  回归会连续加载/卸载两次，并确认关闭后的旧 handle 不能再用于 `dlsym`。
- 页表 NX 位和 EFER SCE/LME/LMA/NXE 位已按 x86-64 定义修正；内核仅在 CPUID
  报告 NX 后启用 EFER.NXE。`mmap` 和 `mprotect` 会更新真实页表 R/W/X 权限，
  `PROT_NONE` 会撤销用户访问，未映射页返回 `ENOMEM`。主程序、解释器和
  `dlopen` 对象先以临时可写、不可执行权限装载与重定位，再按各 `PT_LOAD`
  段收紧；共享边界页取段权限并集，避免相邻段互相撤销必要权限。
- `linux_mprotect` 以 RW 匿名页写入 `mov eax, 42; ret`，切换为 RX 后执行，再
  切回 RW，并验证未映射地址失败。`linux_mmap_reclaim` 通过 `sysinfo` 验证
  2 MiB 匿名映射和 1 MiB `brk` 收缩归还物理页，覆盖中间 `munmap` 的 VMA
  拆分、重复 `munmap`、`MAP_FIXED_NOREPLACE` 和 `EEXIST`。同一 QEMU 连续运行
  并退出 12 次后内存保持 93216 KiB，没有 ELF/栈/页表累计泄漏。
- 普通 VFS 文件现可由 Linux `mmap` 映射：`MAP_PRIVATE` 使用 owned page
  快照，允许写时私有修改且关闭原 fd 后映射继续有效；只读 `MAP_SHARED`
  提供装载段所需的低成本基线。可写 VFS `MAP_SHARED` 在页缓存一致性和回写
  落地前明确返回 `EOPNOTSUPP`。非零页偏移、文件原内容不被私有写污染、EOF
  尾页补零和完整回收均由 `linux_file_mmap` 在 ring3 QEMU 覆盖。整页超出 EOF
  的 fault-to-`SIGBUS` 尚未实现，因此当前在映射起始偏移达到 EOF 时返回
  `EINVAL`。
- `MADV_DONTNEED/FREE` 在当前无 demand paging 的实现中只清零匿名 owned page；
  对文件快照视为 advisory no-op，防止把已映射 ELF/共享库静默替换为零页。待
  文件页缓存和 page fault 重载完成后再实现真正的丢弃/重取。
- 地址空间不再浅复制下级页表：低半区页表独立，HBOS 分配的私有 PTE 以
  ownership 与 COW 软件位标记。`fork` 共享 owned 4 KiB 物理页并增加每页
  16 位引用计数；原可写映射在父子两端清除写位，首次写故障才复制单页，引用
  已降为一时只恢复写位。只读页保留只读语义，显式 `mprotect(PROT_WRITE)` 和
  内核侧 `MADV_DONTNEED` 清零都会先私有化；memfd `MAP_SHARED` 仍按共享语义
  借用同一物理页。用户态普通保护/缺页故障继续转换为同步 `SIGSEGV`。
- Chromium/Qt 常见探测已补 `prlimit64`、112 字节 `sysinfo`、144 字节
  `getrusage`、`PR_SET/GET_NAME`、dumpable、parent-death signal、
  `NO_NEW_PRIVS`、`tgkill(sig=0)`、`clock_nanosleep` 和常用 `madvise` hints。
  `MADV_DONTNEED/FREE` 会通过 owned PTE 清零匿名页；realtime 由启动 RTC epoch
  加 PIT 单调增量生成，替换了错误假设 1 GHz 的 RDTSC 时间。资源上限当前是
  固定内核容量，非等值修改返回 `EPERM`；`getrusage` 的逐任务 CPU/切换计数
  仍为零基线，seccomp 模式尚未实现。
- musl 与 glibc 动态加载器基线均已通过；继续扩展复杂 glibc 依赖和 NSS/locale ABI。
- Linux `stat`, `sigaction`, `ucontext`, socket 等结构体的边界转换。

完成这一阶段后，才把“现成 Linux 二进制直接运行”标为支持。

### 阶段 4：桌面基础服务（IPC 传输底座已完成）

- 已完成 Unix domain stream socket、`SCM_RIGHTS`、credentials 与 memfd；
  x86-64 syscall 51/52 和 HBOS libc 均可查询本地/对端地址，保留 abstract address
  的前导 NUL 和精确长度，短缓冲区仍返回完整所需长度。accept 现在返回客户端
  地址，accepted socket 则保留监听端本地地址，满足 D-Bus 端点发现语义。
- 轻量只读 `/proc`、必要的 CPU/网络 `/sys` 探测和固定容量 inotify 基线已完成；
  继续补设备热插拔语义、tmpfs 与持久可写文件系统。
- Qt/KDE 常用的临时文件替换与 ramfs/HBFS 目录整树 `rename*` 已有基线；
  `openat/newfstatat/readlinkat/symlinkat/renameat*` 可消费真实目录 fd，
  `AT_EMPTY_PATH`/`AT_SYMLINK_NOFOLLOW` 和 ramfs/HBFS 符号链接已通过原生 syscall
  与用户态翻译双路径回归；目录迁移时 cwd、打开的目录 fd 和后代 watch 会继续
  跟随。下一步是 ext2/FAT32 的真正原子 rename 与原生 symlink inode 操作。
- D-Bus、udev 兼容接口、locale、timezone、fontconfig 和权限模型。
- 音频先提供 PipeWire 所需的共享内存与事件接口。

### 阶段 5：标准图形与输入（共享缓冲底座已完成）

- DRM/KMS 兼容对象模型或等价的标准 Wayland backend。
- 已完成 memfd `mmap` 共享缓冲；后续补 Wayland 协议端、dmabuf、
  evdev/libinput 与 xkbcommon。
- 先运行独立 Wayland 客户端，再运行 Qt Wayland，最后运行 KWin。
- HIVE 可继续作为 HBOS 原生轻量桌面；KDE 走标准 Wayland，不依赖 HIVE
  私有 API。

### 阶段 6：原版 KDE

按 `musl → libstdc++ → D-Bus → Wayland → Qt 6 → KDE Frameworks 6 →
KWin → Plasma` 顺序建立持续集成。所有适配补丁只进入 HBOS、工具链或
通用上游可接受的平台层，不维护 KDE 私有分支。

## 当前明确缺口

- 动态 ELF 的 `PT_INTERP` 启动底座已通过最小解释器和真实 musl 1.2.5
  动态 hello 验收；General Dynamic TLS 已有按线程基线，完整 TLS 模型、
  GNU 符号版本、IFUNC/IRELATIVE 已有共享库回归基线；vDSO 仍未完成，
  尚不能把它表述为通用发行版 Linux 二进制兼容。
- `clone3` 的 pthread 形态已经实现，进程/pidfd/cgroup/namespace 形态尚未实现。
  robust futex 和 futex bitset 已完成基础验收。`rt_sigprocmask` 的 Linux 位编号、
  阻塞/解除/查询、`rt_sigaction` 结构边界、普通一参数 ring3 handler、
  `SA_RESTORER` 和 `rt_sigreturn` 已接入并由 `linux_signal` QEMU 用例覆盖。
  同步 page fault 现在进入 ring3 `SIGSEGV` handler；回归把 `PROT_NONE` 页面改为
  可写后返回，验证故障写入被重试，并验证 `RCX/R11` 与其余 syscall-frame 寄存器
  恢复。无 handler 或无法构造安全用户栈帧时只终止故障任务，不再 panic 整个内核。
  `SA_SIGINFO` 完整用户帧（`siginfo_t`/内核布局 `ucontext_t`，按 glibc 2.43
  对齐）已由 `linux_signal_siginfo` QEMU 用例覆盖：三参 handler 的
  `si_signo/si_code/si_pid` 与 `REG_RIP/REG_RAX/REG_RCX` 恢复、SIGSEGV 的
  `si_code=MAPERR/ACCERR`、`si_addr=CR2`、`REG_ERR/TRAPNO` 与 handler 修页
  重试、`sigaltstack` 安装/查询与 `SA_ONSTACK` 递送（rsp 落在 altstack 内、
  `uc_stack.ss_flags` 反映被中断 rsp）、以及 SIGUSR2→SIGUSR1 嵌套递送与
  `uc_sigmask` 逐层恢复（嵌套上限 8）。FPU 状态不随 ucontext 保存
  （`fpregs=NULL`、`uc_flags=0`），`si_pid` 只记录最近一次 kill 发送者；
  这两项仍是已知限制。
- `mprotect` 已落实到页表并覆盖 W^X；部分 `munmap` 会拆分 VMA，匿名映射、
  `brk` 收缩及任务退出会按引用计数归还 owned 物理页和低半区页表。原生
  `fork` 现从 syscall 后现场恢复子进程，八轮回归分别覆盖自动写故障、
  `mprotect`、`MADV_DONTNEED`、父子隔离、子退出回收和单引用快路径。
  `mprotect` 尚未维护细粒度 VMA protection 元数据，空的中间页表只在进程退出
  时裁剪；大型 Qt/Chromium/KDE 负载前仍需跨核 TLB shootdown、可写共享文件
  页缓存和并发/OOM 压力验证。
- AF_INET socket 还没有精确的非消费式就绪查询；AF_UNIX 已有真实 readiness。
- `epoll` 已支持 level-triggered 与 edge-triggered（EPOLLET）、one-shot
  （EPOLLONESHOT）语义，由 `tests/linux_epoll_et.c` 覆盖（宿主 Linux
  参考实现 12/12 PASS；HBOS QEMU 验证见 `scripts/test_linux_compat_smoke.sh`）。
  edge 检测基于轮询扫描的就绪掩码对比，尚未使用 fd 后端状态版本号。
- `SCM_RIGHTS` 当前每条消息最多 4 个 fd、每个 socket 最多排队 4 组；足够
  当前 D-Bus/Wayland 基线，但还需压力测试和跨进程服务验证。
- `getsockname/getpeername` 当前只接入 AF_UNIX；未连接 socket 的
  `getpeername` 返回 `ENOTCONN`，AF_INET 的端点查询仍待网络栈补齐。
- inotify 当前覆盖经 VFS 创建、写入、截断、删除、目录增删、普通文件及目录树
  改名产生的核心事件；移动两端使用同一非零 cookie，文件和目录后代 watch
  会在改名后继续跟随。open/close/attrib、跨后端绕过 VFS 的直接写入、队列扩容
  和高并发压力验证仍未完成。覆盖一个仍被打开的目标文件当前返回 `EBUSY`；ext2/FAT32
  的 rename 仍是 copy-delete 回退，不能宣称断电原子性。
- `*at` 已支持有效目录 fd、`AT_FDCWD`、忽略 dirfd 的绝对路径、
  `AT_EMPTY_PATH` 与 `AT_SYMLINK_NOFOLLOW`，并验证普通文件 fd 用作相对目录时
  返回 `ENOTDIR`。符号链接目前只由 ramfs/HBFS 保存 type 与目标数据，使用固定
  栈缓冲、最多八层展开；ext2/FAT32 尚无原生 symlink inode，硬链接、链接权限/
  时间戳和完整循环诊断仍需补齐。
- D-Bus daemon、Wayland compositor/protocol、DRM/KMS、evdev/libinput、
  locale/fontconfig、完整 procfs/sysfs 语义、设备树和热插拔事件仍需完成。

## 回归测试

兼容测试只在显式 smoke 构建中打包，不增加正式镜像的应用集合：

```sh
make bios-iso HBOS_COMPAT_SMOKE=1
# QEMU shell:
run linux_syscall
run linux_pie
run linux_dynamic
run linux_abi
run linux_signal
run linux_mprotect
run linux_mmap_reclaim
run linux_file_mmap
run linux_process_abi
run linux_dlopen
run linux_dlopen_deps
run linux_compat_thread
run linux_epoll_et
run linux_inotify
```

或一次性回归（自动进入命令行 Shell 并逐项检查 PASS 标记）：

```sh
scripts/test_linux_compat_smoke.sh
```

可选的真实 musl/glibc 门禁不把第三方二进制提交进仓库，显式传入已解包
musl sysroot 与 glibc 库目录：

```sh
make bios-iso HBOS_COMPAT_SMOKE=1 \
  HBOS_MUSL_SYSROOT=/path/to/musl-sysroot \
  HBOS_GLIBC_LIBDIR=/path/to/glibc/libdir
HBOS_MUSL_SMOKE=1 HBOS_GLIBC_SMOKE=1 \
  scripts/test_linux_compat_smoke.sh build/hbos-bios.iso
```

当前基础十五条加两条可选 musl、一条可选 glibc，共十八条路径验证了 Linux `syscall`、静态 PIE/relocation、`PT_INTERP` 与
auxv 交接、结构体与向量 ABI、线程/TLS、AF_UNIX、原生 syscall 51/52、
`getsockname/getpeername` 的 pathname/abstract、短缓冲区与 accept 对端语义、
`SCM_RIGHTS`、memfd
`MAP_SHARED` 生命周期、普通 VFS 文件私有/只读共享映射、匿名页/`brk`/任务退出回收、原生 fork syscall 现场恢复与 4 KiB COW、进程/资源/时钟 ABI、普通 ring3
signal/`rt_sigreturn`、真实页表 W^X、epoll
level/edge/one-shot、目录 fd 相对的 `openat/newfstatat/readlinkat/symlinkat/renameat*`、
`AT_EMPTY_PATH` fd 查询、nofollow 链接元数据、最终/中间路径符号链接解析、
三种 rename 入口的文件与目录整树覆盖/不覆盖语义、cwd/打开 fd/后代 watch
迁移、固定容量 inotify 的
创建/修改/删除、成对移动 cookie、文件 watch 改名跟随及 poll/epoll 就绪、超过 512 KiB 的 VFS 流式 `dlopen`、递归 `DT_NEEDED`、按线程 General Dynamic TLS，以及真实 musl
loader/libc 的动态 hello与真实 pthread。musl 门禁运行 `linux_musl_stream`，
保证 hello PASS 来自超过 512 KiB 的 VFS-backed `execve`，而不是内嵌 HAX
内存快路径；`linux_musl_pthread` 则覆盖原生 syscall 56、四线程 mutex/join
和 musl 静态 TLS；`linux_glibc` 覆盖 glibc loader/libc、版本符号、malloc、
`pread64`、32 位零扩展 `AT_FDCWD`、稳定 VFS inode、基础 pthread/静态 TLS，
以及动态读取 `self/exe`、status、maps、fd、meminfo 与 cpuinfo；同一真实 glibc
程序还会枚举 `/sys/class/net`，读取 CPU online/topology、loopback 和 E1000
地址，防止只有路径占位而没有用户态可消费语义。

这些边界必须通过兼容性测试后逐项升级，不能用“函数存在”代替语义兼容。
