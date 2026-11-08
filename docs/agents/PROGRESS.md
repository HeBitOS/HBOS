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

## 未完成 / 已知坑

- [ ] **169 个头文件尚未套模板**（`make check-headers` 当前全量报错）。
      建议按依赖顺序推进：`src/core/`（8）→ `src/types/`（2）→
      `src/lib/` + `src/libc/` → `src/sys/`（6）→ 驱动（`src/` 根 54）→
      `src/gui/`（20）→ `src/net/`（6）→ `src/user/libc/`（24）。
      全部补齐后再把 `check-headers` 接进 `make test`。
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

## 本机构建注意

`/tmp` 只有约 10 MB 时 `grub-mkrescue` 会报"设备上没有空间"。构建前套
`TMPDIR=$(pwd)/build/tmp make ...` 即可，不要为此改 Makefile。
