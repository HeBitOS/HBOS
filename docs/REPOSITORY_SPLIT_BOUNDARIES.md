# HBOS 内核、GUI 与应用拆分边界

> 状态：HIVE 仓库已建立，物理拆分继续进行
> 日期：2026-07-28
> HIVE 仓库：<https://github.com/HeBitOS/HIVE>

当前完成度：no-GUI 的链接边界、内核到 GUI 的服务 ABI、HAX/注册式应用的
可选打包和外部应用目录已经可用。`src/tools/gui.c` 内仍有遗留桌面应用代码，
因此当前是“可拆仓基础”，不是已经完成的物理拆仓；这部分必须按第 6 节继续迁移。

## 1. 目标形态

首轮采用两个物理仓库、三个逻辑组件：

```text
<KERNEL_REPO_NAME_TBD>
├── kernel / drivers / fs / net / process
├── user libc 与系统调用 ABI
├── gui_service 公共契约
├── no-GUI stub
└── BIOS / UEFI no-GUI 构建

hbos-hive
├── desktop/       桌面、WM、合成器、主题
├── gui-runtime/   gui_service 完整实现
├── sdk/           HAX 窗口 ABI 与 HIVE 控件公共头
└── apps/          独立应用源码包
```

`apps/` 在 GUI 仓库中是独立构建单元，不允许包含桌面私有头。若应用数量或维护
团队扩大，可在以后不改 ABI 的情况下把 `apps/` 再迁为第三个物理仓库。

## 2. 强制依赖方向

```text
应用 → HAX SDK / Window ABI
GUI  → Kernel UAPI / gui_service ABI
内核 → 不依赖 GUI 或应用
```

- 内核不得包含 `wm.h`、`winsrv.h`、`gui_state.h` 或应用模块头。
- 系统调用层只包含 `src/api/gui_service.h`。
- GUI 完整构建提供 `src/gui/service.c`；no-GUI 构建提供
  `src/gui/nogui.c`，两者实现相同服务契约。
- 应用只包含公开 SDK（首选 `hive.h`，兼容 `hax.h`/`hax_widgets.h`）和用户态 libc，不得调用
  `src/tools/gui.c`、合成器、WM 或内核帮助函数。
- ABI 中已发布的系统调用编号只追加、不重排；no-GUI 版本保留编号并明确返回
  “不可用”，避免同一个应用在不同变体中调用到错误功能。

## 3. 当前可独立构建入口

| 构建 | 命令 | GUI | 内嵌 HAX 应用 | 输出 |
|---|---|---:|---:|---|
| 完整系统 | `make` | 是 | 是 | `build/` |
| no-GUI | `make nogui` | 否 | 否 | `build-nogui/` |
| no-GUI BIOS | `make nogui-bios` | 否 | 否 | `build-nogui/hbos-bios.iso` |
| no-GUI UEFI | `make nogui-uefi` | 否 | 否 | `build-nogui/hbos-uefi.iso` |
| 运行 no-GUI BIOS | `make run-nogui` | 否 | 否 | QEMU |
| 运行 no-GUI UEFI | `make run-nogui-uefi` | 否 | 否 | QEMU + OVMF |
| GUI 核心、无应用 | `make core-only` | 是 | 否 | `build-core/` |
| 外部应用目录 | `make APP_DIR=/path/to/apps hax-apps` | 不适用 | 单独产物 | `build/app/` |

构建开关：

- `HBOS_ENABLE_GUI=0|1`：选择 GUI 完整实现或 no-GUI 服务实现。
- `HBOS_BUNDLE_APPS=0|1`：选择空注册表/HAX 清单或把应用打包进系统镜像。
- `APP_DIR=/path`：允许应用源码位于主仓库之外。
- `BUILD_DIR=/path`：每个变体使用独立对象和资源目录，禁止复用不同编译参数的
  `.o` 文件。

## 4. no-GUI 版本定义

no-GUI 表示“不包含桌面、WM、合成器、GUI 字体/壁纸/图标和 GUI 应用”，仍保留：

- BIOS/UEFI 启动。
- framebuffer/VGA 文本终端。
- Shell、文件系统、网络、驱动、ring3、ELF 和系统调用。
- 空的注册式应用表与 HAX manifest；应用加载 ABI 本身仍保留。
- GUI/窗口系统调用的稳定编号，但查询返回不可用、创建窗口返回失败。

no-GUI 启动时跳过 GUI/Shell 选择器，直接进入 Shell；已有磁盘中保存的
`startup hive`/`startup gui` 配置不会阻止启动。

`make nogui-smoke` 必须验证：

- BIOS 和 UEFI 均到达 `[KERN] Shell ready`。
- POSIX/ramfs 自测通过。
- 输出包含 no-GUI 构建提示。
- 内核符号中不存在 WM、winsrv、内置 GUI 应用或 GUI 资源 blob。
- 注册式应用表和 HAX manifest 中应用数量均为 0。

## 5. 应用独立性规则

- 一个应用必须能仅凭 `APP_DIR/include`、用户态 libc 和链接脚本生成 `.hax`。
- 应用状态必须由应用进程自己持有；SDK 控件状态保存在 `hax_ui_t`，不写桌面
  全局状态。
- 应用通过窗口事件获得输入，通过窗口 API 提交绘制，不直接访问 framebuffer。
- SDK 不要求链接桌面对象；`hive.h`/`hax_widgets.h` 当前为纯用户态头文件实现。
- GUI 应用在 no-GUI 系统上必须以“窗口不可用”正常失败，不得导致内核崩溃。
- 应用包和内核版本通过 ABI major/minor 与 capability 协商，不通过仓库同步
  提交号建立隐式耦合。

## 6. 真正拆仓前必须完成

当前已完成的是“可选链接和服务边界”，还不是最终的用户态桌面。实际拆成两个
Git 仓库前还需要：

1. 将桌面主循环从 `src/tools/gui.c` 移出内核工具层。
2. 把 WM、合成器和 GUI runtime 改为 ring3 服务，通过公开系统调用与内核通信。
3. 将 GUI 字体、壁纸、图标改为 GUI 包资源，不再由内核 `incbin`。
4. 将 GUI 内置软件迁移为独立 `.hax` 应用，桌面只读取应用元数据。
5. 生成可版本化的 Kernel SDK 包，GUI 仓库只依赖该包。
6. 在 CI 中分别构建内核、GUI runtime 和应用，再组合成发布镜像。

HIVE 仓库首轮以源码镜像方式接收 runtime、SDK 和已独立的 HAX 应用；HBOS
仓库暂时保留集成副本，直到 runtime 完成 ring3 服务化并建立可版本化的
Kernel SDK。迁移期间不建立反向依赖，也不重写 HBOS 现有 Git 历史。
