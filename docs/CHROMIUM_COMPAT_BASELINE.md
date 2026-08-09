# HBOS Chromium 兼容层阶段 1 基线

> 基线日期：2026-07-27
> 对应路线图：`CHROMIUM_COMPAT_ROADMAP.md` 阶段 1
> 基线用途：后续模块拆分、窗口 ABI 和 Chromium 兼容层工作的比较依据

## 1. 当前架构基线

HBOS 是 C99 编写的 x86_64 自研操作系统，使用 Make、GCC 和 NASM 构建，
支持 BIOS/UEFI 启动。应用可作为 ring3 ELF 或 `.hax` 程序运行，通过
`int 0x80` 进入内核系统调用层。

当前与路线图直接相关的规模如下：

| 项目 | 当前值 |
|---|---:|
| `src/tools/gui.c` | 9362 行 |
| `HBOS_SYS_*` 公开系统调用 | 96 个 |
| `GUI_APP_*` 应用 ID（含 `NONE`） | 15 个 |
| 已注册 `gui_app_module_t` 模块 | 8 个 |
| 桌面窗口上限 | 16 |
| 用户态并发窗口服务上限 | 4 |
| 每个并发窗口事件队列 | 32 项 |
| 内核任务上限 | 16 |
| 内核任务栈 | 8 KiB |
| 内核 bump heap | 128 KiB，`kfree()` 不释放 |
| 浏览器页面缓冲 | 128 KiB |
| 浏览器单次网络响应缓冲 | 512 KiB |

这些数字是阶段 1 的观测值，不代表后续设计上限。自动检查脚本会持续报告这些
指标；需要改变公开编号或 ABI 时，必须按本文的版本规则进行。

> 后续进展：内核 heap 已从上述 128 KiB、不可释放的阶段 1 实现升级为 2 MiB
> 可回收并合并空闲块的实现。基线检查现在防止它退回 no-op `kfree()`；表中的
> 原始观测值仍保留，便于比较阶段间变化。

> 2026-08-09 进展：完整 Linux 信号 ABI 已补齐：`SA_SIGINFO` 三参 handler、
> `siginfo_t` 与内核布局 `ucontext_t`（按 glibc 2.43 x86-64 头文件与宿主实测
> 对齐：`uc_mcontext@40`/`uc_sigmask@296`/432 字节内核布局）、`sigaltstack`
> （syscall 131，`SS_ONSTACK`/`SS_DISABLE`/`MINSIGSTKSZ` 语义）、嵌套递送
> （上限 8 层，legacy 单参数路径与 SA_SIGINFO 路径互斥）、SIGSEGV 的
> `si_addr=CR2`/`REG_ERR`/`REG_TRAPNO` 与 handler 修页重试。以上由
> `linux_signal_siginfo` QEMU 用例覆盖（A：kill 三参 handler 与寄存器恢复；
> B：PROT_NONE 页 SIGSEGV 修复；C：altstack 递送；D：SIGUSR2→SIGUSR1
> 嵌套与 sigmask 恢复）。ucontext 恢复严格校验用户范围，防止 ring3 伪造
> 寄存器；`CSGSFS` 段选择子被钳制为合法用户值。FPU 状态暂不随 ucontext
> 保存（`fpregs=NULL`），`si_pid` 只记录最近 kill 发送者——两项已知限制。

> 2026-08-04 进展：任务上限已从基线的 16 提升至 64，每任务文件描述符从
> 32 提升至 128；增加 futex wait/wake、bitset 变体、绝对超时与 robust-list
> `OWNER_DIED` 清理，并用 ring3 Linux ABI 线程程序验证。动态加载器已加入
> GNU hash 查找，以及最小 ET_DYN 解释器的 `PT_INTERP`/auxv 交接。以上是
> 2026-08-05 又以零拷贝只读 VFS 启动 musl 1.2.5 的 760 KiB loader/libc，
> 并将主程序 `execve` 改成按 PT_LOAD 流式读取；631 KiB 动态 PIE 的真实
> `DT_NEEDED=libc.so` hello 已在 QEMU 通过。以上仍只是 G1 子集，
> 不代表复杂 musl/glibc 依赖、完整 ELF TLS 模型、`clone3` 的进程/pidfd/
> cgroup 形态、完整
> `siginfo/ucontext` 或 Chromium 端口已经完成；普通 ring3 handler、
> `SA_RESTORER`、`rt_sigreturn` 与信号 mask 已有 QEMU 初版。HBOS 自有
> `dlopen` 也已取消 512 KiB 整体缓冲，628 KiB 共享对象的 VFS 流式映射、
> GNU-hash `dlsym`、函数调用和 `dlclose` 已通过 QEMU。页表 NX/EFER 位已修正，
> NXE 由 CPUID 门控；`mmap/mprotect` 现在真实执行 R/W/X/none 权限，主 ELF、
> 解释器与动态库在重定位后按 `PT_LOAD` 收紧权限。RW→RX→执行→RW 以及
> 未映射地址返回 `ENOMEM` 已由 QEMU 验证。部分 `munmap` VMA 拆分、匿名页和
> `brk` 回收、任务退出地址空间销毁也已加入；连续启动/退出 12 次内存用量不变，
> Chromium 常用的 `prlimit64/getrusage/prctl/tgkill/madvise/clock_nanosleep`
> 以及 RTC+PIT realtime/monotonic 时钟也已加入。普通 VFS 文件的私有快照和
> 只读共享 `mmap` 亦已通过 QEMU；`dlopen` 可递归解析 `DT_NEEDED`、去重并按引用
> 级联卸载，INIT/FINI 及其数组由用户 libc 在 ring3 按依赖顺序调用；
> `PT_TLS`、`DTPMOD64/DTPOFF64` 与 `__tls_get_addr` 已形成按需分配的每线程
> General Dynamic TLS 基线，并覆盖 `.tdata/.tbss`、64 字节对齐和线程写隔离；
> GNU `VERSYM/VERDEF/VERNEED`、默认版本选择与 `dlvsym` 也已覆盖函数和 TLS
> 重定位；GNU IFUNC/IRELATIVE resolver 由用户 libc 在 ring3 执行，内核只验证
> 并提交结果，CPL 检查回归可防止退化成 ring0 用户代码调用。loader 事务现
> 按 TID 递归串行化，允许构造函数再次 `dlopen`；四线程各 64 轮
> `dlopen/dlsym/dlclose`、线程局部 `dlerror`、一次性 INIT/FINI 已由 QEMU
> 验证，跨地址空间内核对象链表和装载地址预留也已并发保护。原生 syscall 56
> 已兼容 musl 的 `CLONE_DETACHED` no-op、寄存器返回、新栈、TLS 和 TID 参数；
> 未修改 musl 1.2.5 的四线程 create/join、mutex、condvar 广播、rwlock、once、
> realtime 超时、FPU 环境继承和静态 TLS 隔离通过。未修改 glibc 2.43 的动态
> loader/libc、符号版本、malloc 与 pthread/静态 TLS 也已通过。轻量只读
> procfs 以及 CPU/网络 sysfs 已由该真实 glibc 程序读取和枚举。普通文件
> 真实目录 fd 相对的 `openat/newfstatat/readlinkat/symlinkat/renameat*`、
> `AT_EMPTY_PATH`/nofollow、ramfs/HBFS 最终及中间路径符号链接与固定容量
> inotify 的原生 syscall、ramfs/HBFS 目录整树迁移、cwd/打开 fd/后代 watch
> 跟随、VFS 事件、
> 成对移动 cookie、watch 改名跟随、非阻塞读取及 poll/epoll 也已通过。完整十八项
> 门禁还覆盖 AF_UNIX `getsockname/getpeername` 的 pathname/abstract 地址、
> Linux 短缓冲区长度回报、未连接错误和 accept 真实对端地址；这为 D-Bus
> 命名端点发现补齐了必要 socket ABI。
> musl/glibc/Linux 门禁通过。用户态 page fault 已转换为同步 `SIGSEGV`，ring3
> handler 可 `mprotect` 修页并经 `rt_sigreturn` 恢复寄存器、重试原指令。原生
> `fork` 已从 syscall 现场恢复子进程，私有 owned 页使用引用计数 4 KiB COW，
> 并覆盖写 fault、`mprotect`、`MADV_DONTNEED` 与单引用快路径。可写共享文件
> 页缓存、跨核 TLB shootdown、空页表即时裁剪、`SIGBUS` 与完整 siginfo/ucontext
> 仍未完成。

## 2. 软件模块化现状

### 已注册为独立模块

- 计算器
- 时钟
- 设置
- 文件管理器
- 任务管理器
- 快捷方式
- 图片查看器
- 十六进制查看器

### 仍与桌面主实现耦合

- 记事本
- 文件统计 UWC
- 贪吃蛇
- 浏览器
- 代码工作台
- 控制台终端/诊断

当前 `gui_app_module_t` 只有模块元数据和 `draw`、`on_key`、`on_tick`、
`on_click` 回调。应用状态仍集中在 `gui_state_t`；生命周期、实例私有状态、
resize、挂起和恢复没有统一接口。

## 3. 当前浏览器能力

现有浏览器是 HBOS 自研的轻量浏览器，不包含 Chromium/Blink/V8：

- 支持 `http://` 与 `https://` URL、DNS、重定向和前进/后退历史。
- HTTPS 使用 HBOS 自有 TLS 1.3 路径。
- 能提取 HTML 文本和部分结构，解析有限 CSS，并进行简单流式排版。
- 支持链接点击、滚动、部分图片加载、页面标题和保存纯文本页面。
- 网络加载使用 HBOS 任务异步执行，但解析、布局、绘制与桌面代码仍高度耦合。
- 没有 JavaScript、DOM 标准实现、完整 CSS 布局、Web API、多进程隔离或 GPU 合成。

阶段 6 前必须保留该浏览器作为可用后端和回归参照。

## 4. Chromium 平台能力矩阵

| 能力 | 当前状态 | 已有基础 | 主要缺口 |
|---|---|---|---|
| ring3 ELF | 基础可用 | ELF 加载、用户页表、动态库 | 装载兼容性和大程序压力验证 |
| 调度 | 基础可用 | 100 Hz 抢占、任务和信号；真实 musl 四线程及 glibc 基础 create/join 已通过 | 64 任务上限、公平性、优先级和取消语义 |
| 进程 | 部分可用 | 原生 `fork` syscall 现场恢复与 4 KiB COW、`exec/wait/kill`、`prlimit64/getrusage/prctl/tgkill` 子集 | seccomp、vfork/waitid、资源计数和大型负载验证不足 |
| 虚拟内存 | 部分可用 | 页表级 `mmap/munmap/mprotect/brk/madvise`、普通文件私有/只读共享映射、部分 VMA 拆分、owned-page 引用计数/COW/退出回收、NX/W^X、ELF `PT_LOAD` 权限、用户 page fault 到可恢复 ring3 `SIGSEGV` | 可写共享页缓存、跨核 TLB shootdown、`SIGBUS`/完整 fault metadata 和并发/OOM 压力验证 |
| 共享内存 | 部分可用 | `shmget/shmat/shmdt/shmctl` | Chromium IPC 所需的句柄与权限模型 |
| 同步原语 | 部分可用 | futex/bitset/robust-list、x86_64 原子操作、FS base、共享库 General Dynamic TLS、递归 loader 事务锁，以及真实 musl pthread mutex/condvar/rwlock/once/join 与超时回归 | process-shared/优先级继承变体、TLS descriptor/destructor、取消与异常退出恢复 |
| 内核动态内存 | 基础可用 | 2 MiB 可回收 heap、`kmalloc/kcalloc/krealloc/kfree` | 大型浏览器仍必须主要使用用户态虚拟内存，并补压力与 OOM 验证 |
| 文件系统 | 部分可用 | VFS、ramfs、HBFS、ext2/fat32 读写路径；轻量只读 procfs；CPU/网络只读 sysfs；真实目录 fd 的 `*at` 与 `AT_EMPTY_PATH`/nofollow；ramfs/HBFS 符号链接及文件/目录整树 rename/覆盖/NOREPLACE；固定容量 inotify 移动 cookie 与后代跟随 | ext2/FAT32 原子文件/目录 rename 与原生 symlink inode、完整 procfs/sysfs、inotify open/attrib、设备热插拔、文件锁及长期一致性 |
| 网络 | 部分可用 | IPv4、TCP、DNS、HTTP、socket、select | 完整非阻塞语义、IPv6、代理及压力验证 |
| TLS | 部分可用 | 自有 TLS 1.3/HTTPS | 证书生态、算法覆盖和 Chromium 网络栈接口 |
| 窗口 | 基础可用 | WM、离屏表面、输入队列 | 多窗口句柄、resize、位图/脏矩形提交 |
| 控件 | 缺失 | 手写绘图和命中检测 | 公共控件树、布局、焦点和文本编辑 |
| 字体 | 部分可用 | CJK 位图和 GUI 字体图集 | 字体发现、fallback、整形和 Chromium 字体后端 |
| 剪贴板/IME | 缺失 | 基础键盘输入 | 剪贴板、组合输入和输入法协议 |
| Chromium 构建 | 缺失 | GCC/NASM/Make 工具链 | Clang、GN、Ninja、HBOS sysroot 和平台定义 |
| Chromium 源码 | 外置固定 | Linux Stable 151.0.7922.71、精确 revision、外置同步脚本 | HBOS 平台补丁、补丁管理和许可证清单 |

## 5. ABI 与版本约定

### 5.1 版本命名

从阶段 1 开始分别维护三个版本，互不混用：

- **App Module ABI**：内置应用模块生命周期接口。
- **Window ABI**：用户态窗口、表面、事件和控件接口。
- **Chromium Platform ABI**：Chromium 平台适配层内部接口。

版本使用 `major.minor`：

- `major`：存在二进制或调用语义不兼容。
- `minor`：只追加兼容字段、能力或函数。

当前既有 `gui_app_module_t` 和 `HBOS_SYS_WIN_*` 记为 **v1.0**。阶段 2 与
阶段 4 的新接口分别从 **v2.0** 开始，不在原结构体中原地插入字段。

### 5.2 系统调用稳定性

- 已发布的 `HBOS_SYS_*` 编号禁止重排或复用。
- 新系统调用只允许追加在 `HBOS_SYS_MAX` 之前。
- 内核头、用户 libc 头和 HAX SDK 的枚举顺序必须同步。
- 删除能力时保留编号并返回明确错误，不把编号分配给新用途。
- `scripts/check_chromium_baseline.sh` 负责检查内核与用户 libc 的编号序列。

### 5.3 可扩展结构体

跨 ABI 边界的新结构体统一以以下字段开头：

```c
typedef struct {
    uint32_t struct_size;
    uint16_t abi_major;
    uint16_t abi_minor;
    /* 只允许在末尾追加字段 */
} hbos_abi_header_t;
```

接收方只读取 `struct_size` 覆盖且自身版本认识的字段。新增可选能力通过
capability 位图查询，不通过“调用空实现并假设成功”判断。

### 5.4 资源所有权

- 创建函数成功后必须返回显式句柄。
- 句柄的拥有者负责关闭；任务异常退出时由内核执行最终回收。
- 事件中不得暴露可长期保存的内核裸指针。
- 缓冲区的拥有者、生命周期、像素格式和线程访问规则必须写入公开头文件。
- 兼容层无法实现的操作必须返回稳定错误码并记录能力缺失。

## 6. 目标架构边界

```text
桌面 / Shell
    |
    +-- 应用模块注册与生命周期（阶段 2–3）
    |       |
    |       +-- 内置软件模块
    |       +-- 轻量浏览器外壳
    |
    +-- 窗口 ABI v2 + 控件库（阶段 4–5）
            |
            +-- WM / 合成器 / 输入路由
            +-- 用户态窗口表面

轻量浏览器外壳
    |
    +-- HBOS 轻量渲染后端（现有能力，长期保留）
    +-- Chromium 内容后端（阶段 8–9）
            |
            +-- chromium_compat
                    |
                    +-- 用户态 libc / POSIX 子集
                    +-- 进程、内存、IPC、网络
                    +-- 字体、输入、窗口表面
```

Chromium 专用适配逻辑应位于用户态 `chromium_compat`，不能直接散落在内核、
桌面或各应用模块中。内核只增加可复用的通用系统能力。

## 7. 阶段 1 自动验证

快速静态基线：

```sh
make chromium-baseline
```

同时构建 BIOS 和 UEFI：

```sh
scripts/check_chromium_baseline.sh --build
```

执行完整现有启动矩阵：

```sh
scripts/check_chromium_baseline.sh --boot
```

`--boot` 会调用现有 `scripts/smoke.sh`，需要 QEMU、qemu-img 和 OVMF。
静态基线必须在每个后续阶段持续通过；构建和启动测试按变更风险执行。

## 8. 阶段 1 完成标准

- 本文能力表和缺口已与当前代码核对。
- 系统调用序列、公开常量及模块注册表可以自动检查。
- BIOS/UEFI 构建和现有 smoke 测试存在统一入口。
- ABI 版本、追加规则、结构体扩展规则和资源所有权已经固定。
- 路线图进度表记录阶段 1 的验证结果。

## 9. 首次验证记录

2026-07-27 执行结果：

- `make chromium-baseline`：通过。
- `scripts/check_chromium_baseline.sh --build`：通过。
- `scripts/check_chromium_baseline.sh --boot`：通过。
- BIOS/UEFI ISO、BIOS HDD、BIOS VMDK、BIOS VDI、UEFI HDD 和 UEFI VMDK
  均到达 `[KERN] Shell ready`，POSIX/ramfs 自测通过。
