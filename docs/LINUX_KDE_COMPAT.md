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
- 当前每进程 fd 上限为 128，`poll`/`epoll_wait` 的直接扫描比树或哈希表更便宜；
  fd 上限提高到 64 以上时再改为就绪队列。
- 无事件时调用调度器让出 CPU，不持续忙等。
- HBOS 私有 GUI、HAX 和网络 syscall 编号保持不变。

## 当前已经具备

| 能力 | 状态 | 说明 |
|---|---|---|
| Linux `SYS_*` 源码兼容入口 | 初版 | 常用 x86-64 syscall 号码在 libc 内 O(1) 翻译 |
| 原生 x86-64 `syscall` 入口 | 可用初版 | 高地址 ET_EXEC 与静态 PIE 已在 QEMU 运行 |
| 静态 PIE / auxv / RELATIVE relocation | 可用初版 | 支持 ET_DYN、Linux auxv 和 `R_X86_64_RELATIVE`；PT_INTERP 尚未支持 |
| Linux 结构体 ABI | 可用初版 | 已转换 `stat`、`getdents64`、向量 I/O 与消息 socket ABI |
| `openat` / `newfstatat` / `readlinkat` | 初版 | 当前支持 `AT_FDCWD`；其他 dirfd 明确返回 `ENOSYS` |
| `poll` / `pipe2` | 可用初版 | 管道支持 `O_NONBLOCK` |
| `eventfd` | 可用初版 | 支持 semaphore、nonblock、cloexec 标志 |
| `epoll_create1/ctl/wait` | 可用初版 | 复用原生 fd，就绪扫描无额外复制 |
| `futex` | 可用初版 | 支持 `WAIT`/`WAKE` 及 private 标志 |
| `sched_yield` | 可用 | 直接进入 HBOS 调度器 |
| `getrandom` | 可用初版 | 使用 CPU RDRAND；不可用时明确返回 `ENOSYS` |
| `clone` / FS TLS / TID | 可用初版 | 共享 VM/fd/mm、`CLONE_SETTLS`、child TID 清理与 futex wake |
| AF_UNIX / D-Bus 传输底座 | 可用初版 | stream、abstract address、peer credentials、`sendmsg/recvmsg`、`SCM_RIGHTS` |
| Wayland 共享缓冲底座 | 可用初版 | `memfd_create`、非零 `ftruncate`、零复制 `MAP_SHARED` 与 fd 传递 |
| fork 后兼容 fd 引用 | 可用 | eventfd/epoll/socket/memfd 引用随 fd 表与映射生命周期回收 |

“原生 syscall 入口可用”表示专门构建的静态 Linux ELF 已经可以直接运行；
由于 `PT_INTERP`、完整 TLS relocation 和 glibc/musl 动态加载尚未完成，不能
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

- 已完成 `clone` 共享地址空间线程与 TLS/FS base；后续补 `clone3`。
- `FUTEX_WAIT_BITSET/WAKE_BITSET` 已使用固定 waiter 表实现，支持按掩码
  唤醒和单调时钟绝对超时。
- `set_robust_list/get_robust_list` 与线程退出时的 `OWNER_DIED` 标记、定向
  唤醒已完成；链表遍历设 2048 项硬上限，退出路径不分配内存。
- 内核任务槽从 16 提升到 64、每进程 fd 从 32 提升到 128；两者仍为固定
  BSS 表，先覆盖 Chromium 单进程内容壳和 KDE 基础线程负载。
- 抢占、公平调度、进程组、session、作业控制和更完整信号帧。
- COW fork、可靠的 `execve`、`waitid` 和资源限制。

### 阶段 3：Linux ELF 二进制 ABI（静态 ELF 已打通）

- 已完成 x86-64 `syscall` 指令入口、静态 PIE、auxv 与相对重定位。
- GNU hash 的 bloom/bucket/chain 查找与符号计数已接入现有轻量动态库加载器；
  下一步是 `PT_INTERP`、TLS relocation 和 vDSO。
- ELF 启动器会在切换到新地址空间前对 `argv/envp` 做 64 KiB 有界快照，
  避免 `execve` 在新 CR3 下读取旧进程指针；加固后已通过 ring3 QEMU 用例。
- Linux `execve`/`dlopen` 的过渡缓冲上限从 64 KiB 提升到 512 KiB，可覆盖
  小型 musl/兼容测试；大型 Qt/Chromium 文件仍必须改为按 PT_LOAD 段流式读取，
  不能继续扩大内核 heap 缓冲伪装成完整支持。
- musl 动态加载器优先，随后验证 glibc ABI。
- Linux `stat`, `sigaction`, `ucontext`, socket 等结构体的边界转换。

完成这一阶段后，才把“现成 Linux 二进制直接运行”标为支持。

### 阶段 4：桌面基础服务（IPC 传输底座已完成）

- 已完成 Unix domain stream socket、`SCM_RIGHTS`、credentials 与 memfd。
- `/proc`、必要的 `/sys`、`tmpfs`、持久可写文件系统、inotify。
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

- 动态 ELF 的 `PT_INTERP`、动态 TLS、GNU symbol lookup 与 vDSO 尚未实现；
  当前直接二进制兼容限于专门构建的静态 ELF/PIE。
- `clone3` 与完整 Linux signal/ucontext ABI 尚未实现；robust futex 和 futex
  bitset 已完成基础验收。
- AF_INET socket 还没有精确的非消费式就绪查询；AF_UNIX 已有真实 readiness。
- `epoll` 已支持 level-triggered 与 edge-triggered（EPOLLET）、one-shot
  （EPOLLONESHOT）语义，由 `tests/linux_epoll_et.c` 覆盖（宿主 Linux
  参考实现 12/12 PASS；HBOS QEMU 验证见 `scripts/test_linux_compat_smoke.sh`）。
  edge 检测基于轮询扫描的就绪掩码对比，尚未使用 fd 后端状态版本号。
- `SCM_RIGHTS` 当前每条消息最多 4 个 fd、每个 socket 最多排队 4 组；足够
  当前 D-Bus/Wayland 基线，但还需压力测试和跨进程服务验证。
- D-Bus daemon、Wayland compositor/protocol、DRM/KMS、evdev/libinput、
  locale/fontconfig 和完整 `/proc`/`/sys` 仍需完成。

## 回归测试

兼容测试只在显式 smoke 构建中打包，不增加正式镜像的应用集合：

```sh
make bios-iso HBOS_COMPAT_SMOKE=1
# QEMU shell:
run linux_syscall
run linux_pie
run linux_abi
run linux_compat_thread
run linux_epoll_et
```

或一次性回归（自动进入命令行 Shell 并逐项检查 PASS 标记）：

```sh
scripts/qemu_linux_smoke.sh
```

当前五条路径验证了 Linux `syscall`、静态 PIE/relocation、结构体与向量 ABI、
线程/TLS、AF_UNIX、`SCM_RIGHTS`、memfd `MAP_SHARED` 生命周期以及 epoll
level/edge/one-shot 语义。

这些边界必须通过兼容性测试后逐项升级，不能用“函数存在”代替语义兼容。
