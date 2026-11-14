# 主进度台账

> 每次实质性改动都在"进度日志"末尾追加一行。这是给下一个 AI（和三个月
> 后的你）看的：**当前在做什么、什么已经能用、什么还差着**。
> 不要在这里复述代码——只记状态与结论。

## 当前状态（2026-11-08）

| 维度 | 状态 |
|---|---|
| 引导 | BIOS(GRUB2) + UEFI(Limine) 双通 |
| 内核 | ring0 完整；ring3 任务 + 抢占式调度在跑 |
| GUI | HIVE 桌面可用：WM、合成器、Wayland 服务端、内置应用 |
| 用户态 | libc + 动态链接器 + HAX ABI 可用；Linux ABI 兼容层分阶段 |
| 文件系统 | HBFS / ext2 / exFAT / FAT32 读写，VFS 统一 |
| 网络 | lwIP + mbedTLS，IPv4/IPv6，TCP 大响应已回归 |
| 包管理 | HPT（独立子模块），`.deb` → `.hax` 转换 |
| 浏览器 | Chromium 移植在 `browser/`，兼容基线见 `docs/wiki/` |

## 当前目标（2026-11-08 设定，取代旧的「出窗口」阶段目标）

**终态**：用户把 guest 镜像挂载进 HBOS/HIVE，在 GUI 桌面里启动 Chromium，
能像普通 Linux 桌面一样正常使用**全部功能**：打开网页并渲染显示、鼠标
点击/滚轮、键盘输入、窗口拖动/缩放/最大化、多窗口、复制粘贴、中文与
字体、书签历史、下载；bilibili Electron 客户端也能在同一镜像里跑起来。

**验收口径**（缺一不可）：

1. 客户端连上 `/tmp/wayland-0`，完成 registry 绑定，创建 `xdg_toplevel`，
   attach `wl_shm` buffer 并 commit；服务端日志出现 attach/commit 且合成出帧
2. GUI 里真实看到 Chromium 窗口与网页内容，不是黑屏 / 纯色
3. 键鼠事件由 HIVE 派发到 Chromium，输入有响应
4. browser / renderer / zygote 全部存活，不再 `exit 132` / `ud2`
5. bilibili `.deb` 解包进同一镜像并能启动到同一路径
6. `make test` 全绿；每轮本地 commit，**禁止 `git push`**

**当前卡点（r85）**：Chromium **首次真正跑起来** —— GPU 进程、NetworkService、
StorageService、两个 renderer 进程全部成功 EXEC 启动，crashpad `settings.dat`
三次 open 全成功，Wayland `xdg_toplevel` 已请求、attach/commit 已发出。

剩两处崩溃：

1. **renderer（pid=27/28）`base::File::Open` 校验失败**（`0x1109BF7005` int3）。
   已确认语义：Chromium 打开后会 `fcntl(fd, F_GETFL)` 把低 2 位（O_ACCMODE）
   跟它请求的访问模式比对，不一致就判定"打开失败"并 CHECK 崩溃。
   第一次撞上的原因是 HBOS 覆盖层一律记 `O_RDWR`（已修，见 commit `b0c412a`）；
   剩下的疑点是"从已有 fd 构造 File"这条路径上的 fd 身份，待 fd 表 dump 确认。
2. **browser 主进程（pid=2）`__builtin_trap()`**（`0x1103928A69`）。
   源自 `ZeroStateSuggestionsPageData` 的网络超时路径（errno 必须是
   EPIPE/ECONNRESET 才放行），属于网络未通的次要问题，不是主链路。

## 目录规范（2026-11-08 落地）

源码树按层分。构建产物严格镜像源码树：`src/gui/wm.c` → `build/gui/wm.o`。

```text
src/
├── boot/    启动期纯汇编：长模式入口、引导器 blob
├── core/    CPU/内存/调度/中断
├── app/     用户态应用框架
│   ├── api/    内核↔GUI 契约（gui_service.h、hal）
│   └── legacy/ 旧式内核内注册应用
├── gui/     桌面、WM、合成器、Wayland、图形栈
├── user/    syscall ABI、libc、动态链接器、HAX 装载
└── 其余      驱动、文件系统、网络、加密、工具命令（平铺在 src/ 根）
```

构建目录：`build/`（默认）、`build-nogui/`（无 GUI）、`build-core/`
（仅内核）。三者都在 `.gitignore` 里。

## 2026-11-08 改动

### 0. Chromium：访问模式记账修正（commit `b0c412a`）

**这一轮推翻了上一轮"stack canary 校验失败"的误判**，并找到真正的崩溃根因。

反汇编结论（Chromium `chromium-d2ca.bin`，PIE base `0x1100001000`）：

| 地址 | 语义 |
|---|---|
| `0x1109bf6f50` | `base::File::Open` 的调用方，尾部 `cmpb $0x0,-0x78(%rbp); jne → int3` |
| `0x1109cd3810` | `base::File::Open` 包装：失败置 `byte0=1` + errno(1/2/4) |
| `0x1109cd4cd0` | `fcntl(fd, F_GETFL)` → `and $0x3`（O_ACCMODE）与期望模式比对 |
| `0x1109bf6ef0` | 从结构体 `+8` 取 fd |

`0x1109cd4cd0` 的返回值编码：**`0x100000001` = 模式不匹配（errno 1 EPERM）**，
**`0x100000000` = fcntl 自身返回 -1（errno 2 ENOENT）**。

根因：HBOS 的 `/dev/shm` 与 `/tmp` 覆盖层**无论调用方请求什么模式，一律按
`O_RDWR` 记账**。Chromium 以 `O_RDONLY(0)` 重开 `/dev/shm/.org.chromium.*`
时，`F_GETFL` 返回 `O_RDWR(2)`，比对失败 → 判定"打开失败" → CHECK 崩溃。

顺带修正的语义偏差：

- `F_GETFL` 不再返回 `O_CREAT/O_EXCL/O_TRUNC` 这些"创建期"标志
- `F_SETFL` 不再抹掉访问模式（原来整个 flags 被替换）

**验证（`build/tmp/r83.log`）**：Chromium 首次真正跑起来 —— GPU 进程、
NetworkService、StorageService、两个 renderer 全部成功 EXEC，
`settings.dat` 三次 open 全成功，Wayland `xdg_toplevel` 已请求。
此前所有轮次都停在 browser 主进程 ud2。

#### 反汇编踩过的坑（留给下一轮）

- **`int3` 的 saved rip = int3 地址 +1**，所以 trap 现场显示在后面的 `ud2`
  上。看"断点附近有 `call __stack_chk_fail`"就断言 canary 失败是错的 ——
  必须看**分支条件**。上一轮就因为只看邻近符号误判了一整轮。
- 变长指令不能从任意地址前向解码；x86-64 单指令最长 15 字节，
  真正的边界只能是 `rip-1 … rip-15` 之一（`dump_user_tail` 已有实现）。
- 296MB 的 stripped PIE 二进制，`readelf -rW` 会吐 109 万条重定位，
  直接 grep 太慢 —— 用 Python 扫字节找 `call rel32` / `lea rip-relative`，
  再用 `objdump -b binary --adjust-vma=` 只反汇编目标区间。

#### 新增诊断手段

| 手段 | 位置 | 用途 |
|---|---|---|
| `[OPEN-TRACE]` 16 项环形缓冲 | `src/syscall.c` | open/openat 的路径+结果+fd |
| `[OPEN-FAIL]` 逐条日志 | `src/syscall.c` | 失败 open 的路径+flags+errno |
| `[GETFL-TRACE]` 8 项环形缓冲 | `src/syscall.c` | F_GETFL 的 fd 与返回模式 |
| `[FD-TABLE]` fd 表 dump | `src/syscall.c` | 反查"手上那个 fd"是谁 |

`dlopen` 探测噪音会把 open 环形缓冲冲掉（Chromium 启动时成百上千次探测
不存在的库路径，全是 ENOENT），所以环形缓冲必须**同时记成功与失败**，
最后一条即肇事者。

### 1. 源码树分层

- `src/boot.asm` → `src/boot/boot.asm`，`src/boot_efi.asm` →
  `src/boot/boot_efi.asm`，`src/smp_trampoline.asm` → `src/core/`
- `src/apps/` + `src/api/` → 合并为 `src/app/{api,legacy}/`
- 产物路径随之变化：`build/boot.o` → `build/boot/boot.o`，
  `build/app/uwc.o` → `build/app/legacy/uwc.o`
- 旧产物留在 `build/` 里不会被 make 清理，首次构建前手动删掉
  `build/boot.o`、`build/smp_trampoline.o` 避免误链接

### 2. Makefile 引入源码目录变量

`SRC_DIR` / `SRC_BOOT_DIR` / `SRC_APP_DIR` / `SRC_GUI_DIR` / `SRC_USER_DIR`。
新增子目录时必须同时加变量并把文件归进 `C_SRCS` / `GUI_C_SRCS` /
`USER_C_SRCS` / `ASM_SRCS`，否则会静默漏编。

### 3. 文档收敛

- `src/*.agent.md`（10 份中文名文档）→ `docs/agents/*.md`
- 长期参考文档 → `docs/wiki/`
- 一次性产物（证据、对比报告、截图）留在 `docs/` 根
- 头文件注释模板写入 `docs/wiki/CODE_STYLE.md` §3.1

### 4. `files/` 素材落地目录

用户放进 `files/` 的东西在构建 release 时被打到镜像根下（HBOS 里
`/files/`）。只打非说明文件，用内容 sha256 记账，内容变了才重打 ISO。
`.gitignore` 白名单只留 `README.md` 与 `.keep`。

### 5. 修掉 no-GUI 的两处断链（改动前就坏）

`make nogui` 在 HEAD 上直接失败，与本次重构无关，但顺手修了：

1. `build-nogui/tests/` 目录不存在 → `linux_glibc_libm.hax` 规则补
   `| $(BUILD_DIR)/tests` 与目录规则
2. `cmd_wayland` / `cmd_mouse` 在 no-GUI 里引用未链接的 GUI 符号
   （`wl_server_start`、`gui_pointer_*`）→ 函数、extern 声明、命令表项
   一起放进 `#if HBOS_ENABLE_GUI`

### 6. 索引头文件（§3.1.1）

模板里原来只有"数据模型 / 常量 / 接口"三个分区，缺了图 2 要求的**索引**：
打开文件先看到"它导出什么"，而不是滚到底部才知道。

- `docs/wiki/CODE_STYLE.md` 新增 §3.1.1（索引写法与六条规则）与 §3.1.2
  （自动检查说明）。族函数（`canvas_*` 7 个）允许按族合并成一行。
- 已在 `src/app/api/gui_service.h`、`src/core/pmm.h`、`src/core/vmm.h` 落地。
  `src/user/libc/dlfcn.h` 作为"兼容头也需要索引"的示例。
- 新增 `scripts/check_headers.sh` + `make check-headers`：查①缺模板头
  ②缺索引 ③索引点名的标识符在文件里不存在。**不**查"是否列全"（需 C
  语法分析，且短头文件允许并入 `@brief`，硬比对必误报）。
- 两级豁免：`third_party/` 完全不查；`src/user/libc/` 下的 Linux ABI
  兼容头（`ln.h` `wd.h` `init.h` `dlfcn.h` `link.h`）只查索引不查模板头。

### 7. 169 个头文件全量接入索引 + @brief 计数校验

用户要"逐个重构函数"，所以索引必须先变成可信清单——否则改完函数索引还是
旧的，清单就成了谎。

- `scripts/gen_header_index.py` 批量接入：`new 99` 个（原先没注释）、
  `upgraded 70` 个（把旧的 `@file` 块升级成完整模板）。索引名字从**声明
  本身**推导，职责优先级：尾注 `/**< */` → 上方注释块 → `LIBC_DESC` 标准
  库表 → 词表拆词 → 留「（待补）」。
- `make check-headers` 现在全绿（169 个全部通过），「（待补）」从 2355 行
  压到 620 行，剩下的大多是驱动寄存器宏（`SM_W`、`AHCI_PORT_*`），语义要
  等重构到那里才补得准。
- 新增第 5 项检查 **`@brief` 计数不符**：`导出 N 个接口（… C 函数）` 里的
  C 必须等于实际声明数。第一次跑就抓出 37 个头文件数错（`png.h` 写着 0
  实际 1，`ext2.h` 写着 23 实际 22，`socket.h` 写着 20 实际 24）。
- 计数逻辑单独抽成 `scripts/count_decls.py`：shell 正则数不准——跨行且以
  `{` 收尾的声明（`static inline int foo(\n …\n) {`）会被行尾分号规则整批
  漏掉，libc 头因此数成 0；而只做括号配平又会把结构体里的函数指针成员算
  进去（`xhci.h` 13→18）。两个坑都写在该脚本头注释里。

### 8. `syscall.o` 断链（改动前既有，已修）

`src/syscall.c` 引用两个 `linux_compat` 函数导致链接失败。排查结论：
**不是"从未存在"，是重命名没同步**。

- `linux_compat_overlay_is_path`（10 处）→ 实际名字是
  `linux_compat_shm_is_path`，早已实现。纯改名，`sed` 全量替换。
- `linux_compat_shm_stat`（3 处）→ 确实缺失，新写。语义：按名字查
  `shm_names[]` 表 → `memfd_slots[slot].size`，未登记返回 `-1` +
  `ENOENT`（等价 tmpfs 上 stat 一个不存在的文件）。`size` 允许为 NULL，
  `access`/`faccessat` 就是这么用的。

### 9. 编辑器跳转：compile_commands.json

用户要"跳转到完整代码"。**做不出"一个 include 所有头文件"的汇总头**：
内核 `src/string.h` 与用户态 `src/user/libc/string.h` 的 `memcpy` 同名但
签名不同（`uint64_t n` vs `size_t n`），这是内核/用户态分离的设计。把 173
个头塞进同一翻译单元实测炸出 **182 个** redefinition / conflicting types。
16 组同名头（`string.h` `errno.h` `stat.h` `types.h` `wait.h` …）都属此类。

正解是编译数据库——编辑器要的是**每个文件各自的真实编译环境**：

- `scripts/gen_compile_db.py` + `make compile-db` → 根目录
  `compile_commands.json`（已进 .gitignore）。
- 参数从 `make` 自己展开，不手写 Makefile 解析。三类文件参数不同：
  `net6.c` 用 `LWIP_CFLAGS`，`secure_https.c` / `mbedtls_port.c` 用
  `MBEDTLS_APP_CFLAGS`，其余用 `CFLAGS`。
- 头文件也各挂一条（借用一个真实参与构建的 `.c` 的参数）。覆盖判定：
  `src` 下 **179 个头 100% 覆盖**，另含 91 个 third_party 头。
- 借用的 `.c` **必须真在 C_SRCS 里**——`src/libc/*.c`、`src/app/api/hal.c`
  是死代码，自己编不过，借它们的参数会让头文件满屏飘红。
- 全部 414 条**逐条实跑 gcc 验证通过**，不是"看起来对"。

踩过的三个坑（都写在脚本注释里）：
1. `@echo '$(VAR)'` 会被 shell 剥两层引号，`-DXXX='"net/x.h"'` 丢内层
   引号后 mbedtls 的 `#include XXX` 直接崩 → 改用 `$(info)` 在顶层展开。
2. `shlex.split` 再 `join` 回来同样丢引号 → 保留原始字符串，只按空白切分
   过滤 `-MMD/-MP`。
3. `.d` 里的依赖路径有 `src/core/../fd.h` 这种未归一化形态，会和
   `src/fd.h` 重复 → `os.path.normpath` 统一。

配套：`.clangd`（脚本白名单）、`.vscode/c_cpp_properties.json`
（`compileCommands` 指向生成物，C/C++ 插件与 clangd 都认）。

### 10. `src/hbos_all.h` —— 聚合索引头

用户要"一个可以索引全部的 h 文件"。第一版判断"做不到"是**错的**——
实测发现：**两侧各自内部几乎都干净**（内核侧 131 头只报 5 错，用户态
38 头 0 错），冲突只发生在把两侧混在一起时。所以按编译环境切三支即可。

- `scripts/gen_all_headers.py` + `make all-headers` → `src/hbos_all.h`。
- 三支互斥，用宏选：默认内核侧 134 头 / `HBOS_ALL_WITH_USER=1` 用户态 38
  头 / `HBOS_ALL_WITH_NET=1` 网络配置 7 头，**179 个头文件 100% 覆盖，
  三支各自 gcc 零错误**。
- `scripts/check_all_headers.sh` + `make check-all-headers` 守三件事：
  三支分别编得过、覆盖数 = `find src -name '*.h'` 数目、无遗漏。

分组踩的坑（值得记）：
1. **不能按 `src/user/` 前缀分组**。`src/user/syscall.h`、`app.h`、
   `ldso.h`、`hax_app.h` 引的是内核侧 `../sys/types.h`，属内核环境；
   只有 `src/user/libc/**` 才是独立 libc 环境。按前缀分会把这些也归到
   用户态，绕过分支隔离后撞出 `useconds_t` 重定义。
2. **用户态支绝不能带 `-Isrc`**。带上就会把内核 `sys/types.h` 拉进来，
   `useconds_t`（uint64_t vs unsigned int）、`socklen_t` 全线冲突。
3. **`fb_private.h` 的 `struct flanterm_fb_context` 按值内嵌
   `struct flanterm_context`**，前向声明救不了，必须让
   `flanterm_private.h` 先被包含（`PRIORITY` 表强制前置）。
4. **两个 `_private.h` 自带 `#error`**，标准用法是先定义
   `FLANTERM_IN_FLANTERM`；聚合头里临时定义、include 完再 undef。

顺带修的真 bug：`src/unistd.h` 的 `select`/`poll` 用裸 `void *`，与
`src/sys/select.h` 的 `fd_set`/`struct pollfd` 冲突，两者同时 include 必然
编译失败。改成 `#if !defined(HBOS_SYS_SELECT_H)` 条件声明。

`check_headers.sh` 新增两条豁免：`src/version.h`（Makefile 构建期生成）、
`src/hbos_all.h`（脚本生成的聚合头，`@owner` 写生成器而非目录）。

#### 10.1 从"只 include"升级为"原型 + 位置"（2026-11-08 二次迭代）

第一版只做 `#include`。用户指出这不够：

> 「你不应该在里面写如 `void xxx(int xxx);` 然后表明函数位置可以直接跳转吗」

对。`#include` 只是把符号塞进 TU，跳转完全依赖编辑器的索引服务；
索引没建好就什么都跳不了。现在每个函数原型都**直接印在文件里**，前面
一行是 `文件:行号`，形式：

```c
/* ── src/vfs.h ──────────────────────────────── */
/* 真实声明位于 src/vfs.h */
#include "vfs.h"

/* 初始化 VFS 层  ── 见 src/vfs.h:81 */
int vfs_init(void);
/* 查找节点并跟随符号链接  ── 见 src/vfs.h:83 */
vfs_node_t *vfs_lookup(const char *path);
```

规模：内核支 1427 条原型 / 用户态 211 / 网络 6，`src/hbos_all.h` 4350 行。
新增 `scripts/extract_decls.py` 负责解析（`from extract_decls import extract`）。

解析器踩的四个坑（都会让生成物直接编不过）：

1. **`static inline` 带函数体的定义不能抄成原型**。抄了会切断定义与体的
   联系，`static` 复制成普通原型还会被判"未定义的静态函数"。改为只印
   一行 `/* inline 定义 cli ── 见 src/core/cpu.h:404 */`。
2. **判定 `;` 还是 `{` 必须看第一个非空白字符**。`void cli(void){ f(); }`
   的 body 里也有 `;`，用 `";" in after` 会把内联定义误判为原型。
3. **参数表闭合位置不能用 `body.rfind(")")`**。`__asm__ volatile("cli")`
   里的 `)` 比参数表的更靠右，会取错。改成扫出配平点记 `close_at`。
4. **必须跳过整个函数体**。不跳的话，体内的 `__asm__ volatile("cli")`
   会被当成一条声明抄出来。

另加 `SKIP_DECLS` 登记 `src/unistd.h` 的 `select`/`poll`（裸 `void *`
兜底版本，真版本在 `src/sys/select.h`，两处都印会 `conflicting types`）。
以及：**没有函数声明的纯类型/宏头仍然要输出 `#include`**，否则覆盖率
检查会掉到 158/179（`types.h`、`net/lwipopts.h` 等）。

## 未完成 / 已知坑

- [~] **169 个头文件已全部套模板 + 索引**（`make check-headers` 全绿）。
      剩余 620 行「（待补）」需按重构进度补语义，不是机械活。
- [ ] `check-headers` 尚未接进 `make test`（全绿之后再挂更合适，现在可以挂了）。
- [ ] `browser/chromium/` 仍是 `src/` 的兄弟目录，未并入新树。理由：
      它是移植工作区不是内核源码，物理合并会污染 GN 构建。等移植稳定后
      单独决定（见 `docs/wiki/REPOSITORY_SPLIT_BOUNDARIES.md`）。
- [ ] `src/gui/` 与 `src/ui/` 职责边界仍然模糊——`ui/` 是控件层，现在
      被两边都 include。归到 `src/gui/ui/` 会更清楚，但改动面大。
- [ ] `src/app/api/hal.c` 未被任何 Makefile 规则引用（从未参与构建）。
      要么接进构建，要么删。别让它继续悬着。
- [ ] `src/user/Makefile` 是独立实验构建（`-I../libc`），不在主构建里。
      主构建用的是根 Makefile 的 `USER_LIBC_SRCS`。
- [ ] 子模块 `HIVE/` 里有完整的 `src/ app/ docs/ tools/` 目录树，与本仓
      同名但内容不同（HIVE 是独立仓库）。别把两边搞混。

## 进度日志

| 日期 | 改动 | 验证 |
|---|---|---|
| 2026-11-08 | 源码树分层 + 文档收敛 + 头文件注释模板 + files/ + 修 no-GUI 断链 | `make`（BIOS+UEFI ISO）、`make test`、`make nogui` 全通过 |
| 2026-11-08 | 补"索引头文件"§3.1.1 + `make check-headers` + 4 个示范围落地 | `make check-headers` 索引 0 错；`make` 构建通过 |
| 2026-11-08 | 169 个头文件全量套模板+索引；新增 `@brief` 计数校验（`scripts/count_decls.py`）；修 `syscall.o` 断链（`overlay_is_path` 改名 + 新写 `shm_stat`） | `make check-headers` 五项全绿；`make` 零 error，ISO 产出 |

## 本机构建注意

`/tmp` 只有约 10 MB 时 `grub-mkrescue` 会报"设备上没有空间"。构建前套
`TMPDIR=$(pwd)/build/tmp make ...` 即可，不要为此改 Makefile。

## 交接：Chromium 崩溃 1 的最后一步（2026-11-14 收口时未实施）

`[TRAP-CTX]` 还没做，做完就能定死 renderer `File::Open`（`0x1109BF7006`）的肇事 fd：

1. `src/syscall.c` 新增 `void linux_trap_fd_context(uint64_t rip, uint64_t fd_candidate);`
   —— fd 在 `[0,POSIX_MAX_FDS)` 且条目 used 时打印
   `[TRAP-CTX] rip=0x.. fd=N accmode=X type=Y via=<origin> path=".."`，否则 `fd=invalid`。
2. `src/core/gdt_idt.c` 在 #BP/#UD/#GP 分支、`linux_fd_table_dump()` 之前加
   `linux_trap_fd_context(regs->rip, regs->rdi);`
   依据：这类 CHECK 的 fd 留在 `rdi`（fcntl 包装只改 `esi=3`，r85 现场已证）。
   注意 `0x1103928A69` 那条的 rdi 不是 fd（是缓存的 errno），
   可按 rip 是否落在 `0x1109bf6xxx / 0x1109cd3xxx` 区间决定是否打印，避免误导。

已确认的语义（不用再查）：
- 崩溃 1 = `fcntl(fd,F_GETFL)&3` 与 Chromium 期望模式不等即 `int3;ud2`；open() 记账本身正确。
- 崩溃 2 = `0x1103928A69` 唯一入口 `0x110392892d jne`，判定式 `handle!=NULL && err∉{EPIPE,ECONNRESET}`，
  实测 err=22 EINVAL（缓存于对象 +0x68，经 getter `0x1103518850` 读出）。
- `prctl(PR_SET_VMA)` 已修，`nr=157` 清零；netlink(AF_NETLINK=16) 保持 EAFNOSUPPORT（有评估结论）。

## 崩溃 1 根因收口（2026-11-14 接手后第一轮）

### 结论：`fcntl(F_SETFD)` 是空实现，exec 把子进程要用的共享内存 fd 全关了

Chromium 用**命令行**把共享内存句柄的 fd 号传给子进程，实测 `[EXEC-ARGV]`：

```
--metrics-shmem-handle=4,i,<guid_hi>,<guid_lo>,262144
--field-trial-handle=3,i,<guid_hi>,<guid_lo>,262144
--pseudonymization-salt-handle=7,i,<guid_hi>,<guid_lo>,4
```

这些 region 用 `O_CLOEXEC` 创建，spawn 前必须 `fcntl(fd, F_SETFD, 0)` 清掉
FD_CLOEXEC 才能活过 exec。而 `src/syscall.c` 的 `linux_fcntl` 里：

```c
case 1:  /* F_GETFD */
case 2:  /* F_SETFD */
    return 0;   /* 未跟踪 FD_CLOEXEC */
```

`F_SETFD` 什么都没做，`flags` 里 `O_CLOEXEC` 还在，HBOS 的 exec 按它自己的
规则（`entry->flags & O_CLOEXEC` 就关）把这三个 fd 全关了。号码随即被复用：
实测子进程 **fd 3 = unix socket、fd 4 = icudtl.dat、fd 7 = /dev/null**，
而命令行里写的是 3/4/7 的共享内存 → 子进程拿 `/dev/null` 去做
`F_GETFL == O_RDWR` 校验 → `0x1109BF7006` 的 int3。

修法（最小且符合 Linux 语义，`O_CLOEXEC` 本来就折在同一个 `flags` 字段里）：
`src/fd.h` 新增 `fd_getfd_flags` / `fd_setfd_apply`，`linux_fcntl` 的
case 1/2 真实现；顺带 `F_DUPFD_CLOEXEC(1030)` 给新 fd 补上 CLOEXEC 位。

验证：同一 argv 跑两轮，修复前 `0x1109BF7006` 每次必崩，修复后**一次都没有**，
进程能活到崩溃 2（`0x1103928A69`，本轮落在浏览器主进程 pid=2）。
Wayland 请求数 51→51 基本不变（还没到合成阶段，正常）。

### 顺带修的真 bug

1. **`[E]` 错误追踪条件优先级错**：原写法
   `A && B && C || D`，pid 命中时**绕过** `result<0` / 预算无条件打印，
   且 `task_current()` 为 NULL 时会解引用。拆成显式 `pid_match`。
2. **`[SHM-OPEN]` 环被冲垮两轮**：
   - 第一版按 `entry->type == FD_MEMFD` 判"值得记"，但 `/tmp` 覆盖层文件
     也是 `FD_MEMFD`，一条 Chromium 启动上百次开合 → 32 槽全是 close；
   - 第二版按路径过滤写在了 close 一侧，忘了 shm-open 一侧：`/tmp`、`/run`
     的覆盖层也走 `linux_compat_shm_open` → 96 槽全是 DawnGraphiteCache；
   - 现在：**open 侧只认 `/dev/shm/` 前缀**，环扩到 512 槽，`/dev/null`
     的开点、`dup2`（含"顶掉了谁"）、`SCM_RIGHTS` 收权都进同一条时间线。
3. `-Wcomment`（注释里的 `/dev/shm/*`）与 `rights_count` 未使用，一并清掉。

### 反汇编修正（推翻交接里的一处推断）

`0x1109BF7006` 是 `cmp BYTE [rbp-0x78],0; jne int3`，即
**"wrapper 返回错误码就 FATAL"**；wrapper `0x1109CD3810` 里
`fcntl64(fd, F_GETFL)`，把 `ret&3` 与 `setne sil; add esi,esi` 算出的
`(mode!=0)?O_RDWR:O_RDONLY` 比对。所以：

- `rdi` 确实是被校验的 fd（与 `[GETFL-TRACE]` 一致），这点交接是对的；
- 但 expected **不是调用点字面常量**，是运行期 mode 算出来的（本次 mode=2）。
  交接里"`bt[3]` 处 `mov $0x2,%ecx`"看到的是另一处指令。

另外两条与交接文档冲突的事实：

- **`memfd_create` 确实被调用**：`[MEMFD-CREATE] pid=2 path=shared-memory-region
  req=0x3`（CLOEXEC|ALLOW_SEALING）。"Chromium 不调 memfd_create"只适用于早期阶段。
- **一次 `SCM_RIGHTS` 传共享内存都没有发生**（`ev=scm` 全程 0 条），
  共享内存 fd 是靠 fork 继承 + 命令行号码传的，不走 unix socket。

### 下一步

1. 崩溃 2（`0x1103928A69`）现在落在浏览器主进程，err=22 EINVAL。修完 F_SETFD
   之后 fd 布局变了，这条要重新取一次上下文再判断。
2. `src/graphics/drm/`（`drm_card.c` 36K / `evdev.c` 21K / `EGL_DECISION.md`）
   是子代理产出，**已纳入构建但没有挂进启动流程**，未提交。要先定方向：
   它是 legacy KMS ioctl 集（20 个），而 `KWIN-PLAN.md` 结论是 KWin 6.5
   只走 atomic → 需要 `/dev/fb0` shim 或补 atomic。

## 崩溃 2 定位（2026-11-14 第二轮）

### 判定式核对（反汇编）

`0x110392892d` 那处不是"err∉{EPIPE,ECONNRESET}"这么含糊，实际是：

```asm
3928916: mov rdi,[rbx+0x20]     ; 通道对象
392891a: test rdi,rdi / je ok   ; 没有通道就直接过
392891f: mov eax,[rbp-0x94]     ; 缓存的错误码
3928925: cmp eax,0x20           ; EPIPE
392892a: cmp eax,0x68           ; ECONNRESET
392892d: jne 3928a68            ; 其它一律 FATAL
```

同一个函数里 `0x224cb50` 处的 rodata 是 **"XDG_CURRENT_DESKTOP"**（19 字节，
正好对上 `mov ecx,0x13` + 虚表调用），说明这条崩溃在 **Ozone/Wayland 初始化**
里：读桌面名 → 建通道 → 通道报错不是"对端走了"就 FATAL。

### memfd_create 的 flag 缺口（已修）

`[MEMFD-CREATE] pid=2 fd=-1 ev=memfd req=0x0xb path=shared-memory-region`

`0xB = MFD_NOEXEC_SEAL(0x8) | MFD_ALLOW_SEALING(0x2) | MFD_CLOEXEC(0x1)`。
Linux 6.3 起的新 flag，HBOS 不认就 EINVAL。Chromium 自带 fallback（退回 0x3
重试，日志里紧接着就有成功的 `req=0x3`），所以不致命，但错误码留在 errno 里
污染后续判断。已在 `linux_compat.c` 接受 `MFD_NOEXEC_SEAL` / `MFD_EXEC` 并对
两者同时出现返回 EINVAL；**"不可 exec"的语义没有强制**（需要 mmap 的
PROT_EXEC 检查配合），属已知缺口。

### `[ERRNO]` 环的噪声过滤（新增）

24 槽的环被 futex 的 ETIMEDOUT、recvmsg 的 EAGAIN、ioctl 的 ENOTTY、
readlink 的 EINVAL 刷满，真正想看的那条 EINVAL 反而被挤掉（崩溃 2 就是这么丢的）。
新增 `linux_errno_is_noise()`：只滤已确认语义正确的 (nr, errno) 组合，
没查清的一条都不滤。

### dup2 源 fd 可见后的事实

每个 Chromium 子进程启动时固定 6 步 remap，**目标号固定、源号随进程变**：

```
[DUP2] pid=28 fd=0  src=37 /dev/null      stdin
[DUP2] pid=28 fd=5  src=36 (socket)        mojo 通道；fd5 原本是 shm，被顶掉
[DUP2] pid=28 fd=7  src=0  /dev/null   ★  就是这一条
[DUP2] pid=28 fd=6  src=1  /dev/shm/...
[DUP2] pid=28 fd=10 src=2  /dev/shm/...
```

命令行里三个句柄的号码是**写死的 3 / 4 / 7**（`--field-trial-handle=3`、
`--metrics-shmem-handle=4`、`--pseudonymization-salt-handle=7`），实际搬运靠
dup2。而 `fd=7` 的源是 `0` —— 即上一步刚装成 /dev/null 的 stdin。

结论：**父进程把 pseudonymization salt 用 /dev/null 占了位**（salt region
没建出来，或者建出来又丢了），子进程按命令行取 fd 7 去做
`PlatformSharedMemoryRegion::Take()`，`F_GETFL&3 != O_RDWR` → 崩溃 1。
这也解释了为什么崩溃 1 变成偶发：只有走到 `/dev/null → 7` 这一步的子进程才炸
（p4 只剩崩溃 2，崩溃 1 一次没出现）。

### 下一步

父进程为什么拿 /dev/null 当 salt。salt 只有 4 字节，走
`UnsafeSharedMemoryRegion::Create(4)` → `memfd_create("shared-memory-region")`，
实测这条**能成功**，所以丢的可能在"取只读句柄 / DuplicateHandle"那一段。
需要观察父进程 fd 3 / 4 / 7 何时被别的东西抢占（现在只有 crash dump 才 print
fd 表，运行期看不到）。

### spawn 现场（`[EXEC-FD]`，新增，12 次预算）

在 `elf64_load_vfs_and_exec` 里、`[EXEC-ARGV]` 之后打父进程 fd 3..12。
实测同一轮里前后两种形态：

```
前三次 exec: 3:UNIX/2 4:- 5:UNIX/2 6:MEMFD/0=/dev/shm/... 7:FILE/0=/dev/null
后三次 exec: 3:UNIX/2 4:UNIX/2 5:UNIX/2 6:MEMFD/0=/dev/shm/... 7:MEMFD/2
```

即：**早启动的那几个子进程 spawn 时，7 号位上根本没有可写共享内存**，
晚启动的才有（`MEMFD/2`，没有路径 = 裸 memfd_create，正是 salt 的样子）。
六个子进程都带着 `--pseudonymization-salt-handle=7`，所以早期那几个
必然在 `Take()` 里炸 —— 这就是崩溃 1 "偶发"的全部来源。

顺带确认：命令行里的 `3 / 4 / 7` 在父进程里也都**不是**共享内存
（3 是 socket、4 有时是空的、7 如上），说明这三个号码是"子进程目标号"，
实际搬运靠 remap，而 remap 出现在早期进程里的是 `dup2(0 → 7)`
（源是刚装好的 /dev/null）。

下一步只剩一个问题：**salt region 什么时候建、它的真实 fd 是多少**。
建议直接盯 `memfd_create("shared-memory-region")` 之后的那次
`GetReadOnlyHandle`/`dup`——如果 HBOS 的 `dup()` 返回了 0（它从 0 号开始找空位，
而 Linux 的 dup 也允许低号，但 Chromium 侧显然按"≥3"在记账），
就会出现"Chromium 认为 salt 在 0 号"这种局面。
