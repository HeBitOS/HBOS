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
| 调度 | 基础可用 | 100 Hz 抢占、任务和信号 | 任务上限低，缺少 pthread 语义 |
| 进程 | 部分可用 | `fork/exec/wait/kill` | 无 COW，进程模型尚未做大型负载验证 |
| 虚拟内存 | 部分可用 | `mmap/munmap/mprotect/brk` | 大地址空间、文件映射和并发压力验证 |
| 共享内存 | 部分可用 | `shmget/shmat/shmdt/shmctl` | Chromium IPC 所需的句柄与权限模型 |
| 同步原语 | 缺失 | x86_64 原子操作基础 | pthread、mutex、condvar、TLS、等待/唤醒 |
| 内核动态内存 | 阻塞项 | `kmalloc/kcalloc/krealloc` | 仅 128 KiB 且不能释放 |
| 文件系统 | 部分可用 | VFS、ramfs、HBFS、ext2/fat32 读写路径 | 文件映射、锁、监听及长期一致性 |
| 网络 | 部分可用 | IPv4、TCP、DNS、HTTP、socket、select | 完整非阻塞语义、IPv6、代理及压力验证 |
| TLS | 部分可用 | 自有 TLS 1.3/HTTPS | 证书生态、算法覆盖和 Chromium 网络栈接口 |
| 窗口 | 基础可用 | WM、离屏表面、输入队列 | 多窗口句柄、resize、位图/脏矩形提交 |
| 控件 | 缺失 | 手写绘图和命中检测 | 公共控件树、布局、焦点和文本编辑 |
| 字体 | 部分可用 | CJK 位图和 GUI 字体图集 | 字体发现、fallback、整形和 Chromium 字体后端 |
| 剪贴板/IME | 缺失 | 基础键盘输入 | 剪贴板、组合输入和输入法协议 |
| Chromium 构建 | 缺失 | GCC/NASM/Make 工具链 | Clang、GN、Ninja、HBOS sysroot 和平台定义 |
| Chromium 源码 | 未引入 | 无 | 固定上游版本、补丁管理和许可证清单 |

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
