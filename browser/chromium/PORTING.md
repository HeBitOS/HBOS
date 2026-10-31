# Chromium on HBOS —— 移植现状与落地方案

> **重要更正（2026-10-25）**：HBOS 上跑的 Chromium **不是**从源码编出来的，
> 而是**走 Debian `.deb` 二进制**，用 `scripts/install_app.py --deb` 解包写进
> 镜像，作为 Linux ELF 由 HBOS 的 Linux 兼容层（`src/syscall.c` 的
> `linux_syscall_dispatch_frame`）直接执行。本文后半部分的"四阶段源码构建
> 路线（S0–S3）"是**备选方案 / 长期方向**，不是当前主线，也不是"已完成"。
>
> 现网事实（见 `docs/jindu.md`）：
> - 已装包：Debian `chromium` 151.x，位于镜像 `/opt/chromium`，入口是
>   `chrome` → `chromium-browser` 的符号链接链；
> - 当前可跑通的是 `--ozone-platform=headless`（**不带** `--headless`），
>   浏览器长期存活不崩；
> - **做不到** `--dump-dom` / `--screenshot`：Debian 的 chromium 包里**没有**
>   编进 `headless::HeadlessBrowserImpl`（二进制里 0 个符号），命令处理器
>   存在但没有调用者 —— 这是打包差异，不是 HBOS 内核 bug；
> - 两个真正的硬前置：① 跨进程真共享内存（现在 `MAP_SHARED` 只做到进程内
>   私有快照）；② 最小 Wayland compositor（Ozone/Wayland 后端已编入，245
>   个符号可用）。

固定版本：`151.0.7922.71`（见 `VERSION`）。本文记录"现在到哪一步、缺什么、
下一步做什么"，作为后续工作的唯一依据。

## 零、为什么走 deb 而不是源码（结论先行）

| 维度 | deb 二进制 | 源码构建 |
|---|---|---|
| 一次性成本 | 解开 100+ MB 的包，几分钟 | ≥100 GB 工作树 + depot_tools + 数小时首次全量编 |
| 可迭代性 | 改内核兼容层即可，浏览器不动 | 每次改 HBOS 后端都要重编 chromium（数小时） |
| 调试信息 | 可用 Debian dbgsym 符号化栈 | 需自建 symbol 服务器 |
| 代价 | 被 Debian 的编译开关锁死（见下） | 自由裁剪（可编 headless shell、关掉不需要的 feature） |

被锁死的几个点（都验证过）：
- 无 `chrome-headless-shell`：`--headless` 只会"无窗口 → 干净退出"；
- Ozone 后端只有 `x11` / `wayland` / `headless`，没有自定义 HBOS 后端；
- 依赖 glibc 具体版本，`__libc_start_main` / TLS / `IFUNC` 都要兼容到位。

**所以当前策略是**：先让"Linux 二进制能在 HBOS 上活下来并被看见"（兼容层 +
Wayland compositor 路线），源码构建留作将来想要"原生 HBOS 窗口后端 / 裁剪
体积"时的选项。

## 一、现状盘点

| 层 | 现状 | 缺口 |
|---|---|---|
| 固定版本 / GN 参数 | 完成。`args.host.gn`（Linux 参考壳）与 `args.hbos.gn`（HBOS 目标）都在 | 无 |
| 源码工作树 | **不存在**。`scripts/chromium_source.sh` 只能引导，`/run/media/.../hbos-browser-work` 未建立 | 需 ≥100 GB 空间 + 一次性 bootstrap |
| Linux syscall 层 | 较强。`src/syscall.c` 212 个 dispatch 分支，覆盖 `clone`/`futex`/`epoll`/`inotify`/`signalfd`/`memfd`/`shm` | `fork`+`execve` 组合仅 13 处引用，需确认 Chromium 多进程模型可用 |
| 动态链接 | `src/user/ldso.c` 支持 `dlopen`/`dlsym`（含版本化符号），`dlfcn` 13 个入口 | 需验证 ELF TLS（`__tls_get_addr`）与 `DT_RELR` 压缩重定位 |
| libc | `src/user/libc/` 自研，按需增补 | Chromium 依赖 glibc 私有细节（`__libc_start_main`、`sysinfo`、`getauxval` 完整度） |
| 图形栈 | `src/gpu.c` 只有 `virtio_gpu_init` 占位；实际走 framebuffer 软件合成 | Ozone/HBOS 后端需要 surface 提交路径（`wl_compositor` 已有） |
| 音频 | AC'97 + WAV 播放可用 | Chromium 需要 ALSA/Pulse 抽象层或自建 audio sink |
| 网络 | lwIP + mbedTLS + IPv6/X.509（`src/net6.c`、`secure_https.c`） | Chromium 自带 BoringSSL + 自己的 socket 封装，需对接 lwIP 的 fd 语义 |

**一句话结论**：引擎本身能编（Linux 参考壳路径完整），真正的成本全在
"HBOS 平台层"——Ozone 后端、多进程、音频、以及 glibc 兼容细节。

## 二、四阶段落地路线（**备选方案**，非当前主线）

> 下面整节假设"从源码构建"。只有在下述任一条件成立时才值得切过去：
> - 需要一个真正的 `ui/ozone/platform/hbos/` 后端（而不是借 Wayland）；
> - 需要裁掉一大半用不上的组件，把体积从 300 MB 压到几十 MB；
> - 需要 `chrome-headless-shell` 这类 Debian 包里没有的产物。
>
> 在那之前，主线是：**内核 Linux 兼容层 + 最小 Wayland compositor + deb 二进制**。
> 相关进度见 `docs/jindu.md`（R0–R18c）。

### S0：Linux 参考壳（不依赖 HBOS）

目的：证明固定版本、GN/Ninja、Blink/V8 本身可用，把"平台问题"和"引擎
问题"彻底分开。

```sh
export HBOS_CHROMIUM_WORKDIR=/path/to/hbos-browser-work   # 需 ≥100 GB
scripts/chromium_source.sh bootstrap     # depot_tools + fetch --nohooks
scripts/chromium_source.sh pin           # 切到 VERSION 的精确提交
scripts/chromium_source.sh hooks
scripts/chromium_source.sh host-gen
HBOS_CHROMIUM_JOBS=4 scripts/chromium_source.sh host-build   # → content_shell
```

产出物：`out/hbos-host/content_shell`。用它跑 `docs/` 里的 Vue 验收页，
确认 G2 级别（ES modules / fetch / Vue 3 hydration）通过。

### S1：Ozone 后端（第一个真正的 HBOS 改动）

Chromium 的窗口系统入口是 Ozone。需要新增 `ui/ozone/platform/hbos/`：

- `OzonePlatformHbos`：实现 `CreateSurfaceFactory` / `CreateWindowManager`
- `SurfaceOzoneCanvas`：把 Skia 的 raster 结果 memcpy 到 HBOS 的窗口
  共享表面（`hax_win_open` 同一套 `wl_surface` 机制，已有实现可参考
  `src/gui/wl/wl_surface.c`）
- 输入：把 `wl_seat` 的 pointer/keyboard 事件翻译成 `ui::Event`

这一步之前必须先在 HBOS 侧补一个能力：**共享内存窗口表面**。当前内置
应用是直接画进合成缓冲，外部进程需要 `mmap` 一块可被合成器读到的区域
（`memfd_create` + `mmap` 已覆盖，需串联成 API）。

### S2：多进程与 glibc 兼容

Chromium 默认多进程（browser / gpu / renderer）。两条路：

- **路线 A（推荐先做）**：`--single-process` + `--no-sandbox` 单进程跑通
  渲染，先拿到"能显示网页"的里程碑。代价是不稳定、不隔离，但验证价值高。
- **路线 B**：补齐 `fork`/`execve` 语义与 zygote 支持。HBOS 当前 `clone`
  完整度不错，但 `fork` 的 COW 语义和 `execve` 后的 `ldso` 重入需要验证。

glibc 兼容优先补这几个（Chromium 启动即用）：`getauxval`、`sched_getaffinity`、
`prctl(PR_SET_NAME)`、`/proc/self/maps` 伪文件、`sysinfo`。

### S3：媒体与 GPU

音频：先接一个"静音 sink"让 `<audio>`/MSE 初始化不失败，再逐步接 AC'97。
视频：`args.hbos.gn` 里 `proprietary_codecs = false`，Bilibili 的 H.264
主码流会失败 —— 需要打开 `proprietary_codecs` + `ffmpeg_branding = "Chrome"`，
与 `args.host.gn` 保持一致。

GPU：短期保持软件合成（`--disable-gpu`），中期等 `virtio_gpu` 真正实现。

## 三、五项验收（沿用原文）

- G0：Lite 能力诊断及离线回归。
- G1：动态 ELF、pthread/futex、IPC、IPv6/X.509、窗口与音频基础。
- G2：Blink/V8 执行 HTML、完整 CSS、ES modules、fetch、Vue 3 hydration。
- G3：Bilibili 首页完成 SSR hydration、导航、图片和交互。
- G4：Bilibili 视频页完成 MSE、解码、音频输出与 720p 连续播放。

验收必须由自动验收页或启动日志证明，不能以"能够编译"代替。

## 四、当前建议的下一步

1. 先做 S0。它不碰 HBOS 内核，风险最低，且能立刻暴露"固定版本是否真的
   可编"这个问题（Chromium 上游经常有版本相关的 GN 失效）。
2. S0 通过后立刻做 S1 的"共享内存窗口表面"这个内核侧 API —— 它是后面
   一切的前提，且工作量可控。
3. 单进程跑通网页渲染（S2 路线 A）作为第一个对外可见的里程碑。
