# KWin/Plasma 落地可行性：依赖清单、后端选型与分阶段计划

> 状态：计划文档（不含生产代码）
> 证据基线（2026-11-14）：
> · KWin 上游 `master` = commit `c90230b`，PROJECT_VERSION 6.8.80；
> · 稳定参考版 `v5.27.11` / `v6.5.0`（Qt ≥ 6.9 / KF6 ≥ 6.18）/ `v6.6.0` / `v6.7.0`（Qt ≥ 6.10）已逐版核对；
> · 本仓库 `/media/data/hbosv2` @ 当前工作树（Chromium r85 状态，见 `docs/agents/PROGRESS.md:37`）；
> · 包体积取自宿主 `pacman -Si`（Arch 仓库，2026-11-14），**仅作量级参考**，发行版与版本会影响具体数字。
>
> 相关上游源码链接格式：`https://invent.kde.org/plasma/kwin/-/blob/v6.5.0/<path>#L<n>`。

---

## 0. 结论摘要

**一句话**：KWin/Plasma 在 HBOS 上可行，但**第一帧的瓶颈是 C++/Qt 运行时，不是 EGL，也不是 KMS**。
可控路径是「KWin ≤ 6.7 的 QPainter 软件合成 + 自定义输出后端 + `/dev/fb0`」，把 DRM/atomic 与
Mesa 全部推到后期；`--virtual`/`--headless` 出不了画面，legacy 最小 KMS 也喂不饱 KWin 的 DRM 后端。

### 0.1 决定性问题 A：KWin 没有 EGL 能不能起来？

**能，但必须锁 KWin ≤ 6.7，并且用 QPainter 合成路径。**

| KWin 版本 | 软件合成（QPainter） | 无 EGL 驱动可跑 | 证据 |
|---|---|---|---|
| 5.27 / 6.5.0 / 6.6.0 / 6.7.0 | 有（backends 里每后端一份 `*_qpainter_backend`） | **可以** | `v6.5.0` 树含 `src/qpainter/qpainterbackend.{h,cpp}`、`src/scene/itemrenderer_qpainter.cpp`、`src/backends/virtual/virtual_qpainter_backend.cpp`、`src/backends/drm/drm_qpainter_backend.cpp` |
| master 6.8.80 | **已删除** | 不可以 | `git ls-tree -r HEAD \| grep -i qpainter` 只剩 `src/qpainter/qpainterswapchain.{cpp,h}`；`OutputBackend` 只剩 `createOpenGLBackend(RenderDevice *)` 一条渲染路径（`src/core/outputbackend.h:44`），`createQPainterBackend()` 已不存在 |

三个必须写进计划的细节：

1. **`--virtual` 与 `--headless` 不是一回事，且 KWin 已没有 headless 后端**。
   6.5.0 与 master 的 `src/backends/` 只有 `drm / fakeinput / libinput / virtual / wayland / x11`；
   `main_wayland.cpp` 只有 `--virtual`（6.5.0:311，master:362），**没有 `--headless`**。
   `--virtual` 的 `VirtualOutput::present()` 只置 `m_frame` 并 arm vsync 监视器，
   **不把像素送到任何地方**（`src/backends/virtual/virtual_output.cpp:56-61`）——它是给 CI/截屏用的，
   想看见画面必须换成我们自己的 present。
2. **QPainter 路径不需要 EGL 驱动，但链接期仍需 `libEGL.so` + `libepoxy.so`**。
   KWin 6.5 的 `EGL`/`epoxy` 是 `TYPE REQUIRED`（`CMakeLists.txt:190-200`），
   `kwin` 目标无条件链 `EGL::EGL`、`epoxy::epoxy`（v6.5.0 `src/CMakeLists.txt:254-255`），
   `OutputBackend` 头文件直接 `#include <epoxy/egl.h>`（`src/core/outputbackend.h:13`）。
   所以：**运行时提供 libEGL.so（Mesa 或桩）即可，只要不初始化 EGL 就不会碰驱动**；
   这一条在 M1 用宿主原型一小时内可证伪/证实。
3. **要 GL 也不是 ANGLE swiftshader**。KWin 的 GL 路径是 EGL/GLES，
   最小可行是 **Mesa llvmpipe/softpipe（EGL_PLATFORM=surfaceless 或 gbm）**；
   ANGLE/SwiftShader 是 Chromium 的 GL-over-Vulkan/D3D 方案，多一层 Vulkan 依赖，
   KWin 也不会选它。第一帧完全不需要 GL。

### 0.2 决定性问题 B：一周内看到东西的 Plan B

**一周内能在 HBOS 屏幕上看见的，只有「自研 Wayland 服务端 + 手写极简客户端」这一条**；
KWin 在 HBOS 上出画面不在「一周」量级（见 §5 的代价列）。
Plan B 的完整定义见 §5.1，一句话版：

> D1–D3：HBOS 现有 Wayland 服务端（`src/gui/wayland/`）→ 一个约 300–500 行的 C 客户端
> （`wl_shm` + `xdg_wm_base` + `wl_seat`）在 HIVE 里出窗口、收键鼠；
> D4–D5：宿主 Linux 上用 KWin 6.5 `--virtual` + 自定义后端把帧落盘，跑通后端接口；
> D6–D7：把 sink 从「文件」换成「HBOS framebuffer 抽象」，定下 fbdev 契约。

**为什么不是 KWin 先**：HBOS 用户态没有任何 C++ 运行时（见 §1），
Qt6/KF6 的二进制与 ABI 才是主工程量；一周内碰 KWin 只能产出编译错误。

### 0.3 五条硬结论

| # | 结论 | 依据 |
|---|---|---|
| C1 | 目标版本锁 **KWin 6.5.x（Qt 6.9 / KF6 6.18）**，不要跟 master | master 删了 QPainter；6.6/6.7 需 Qt 6.10，且 6.7 是最后一个带 QPainter 的版本线 |
| C2 | 后端走 **自定义 OutputBackend + QPainterBackend**，输出到 **`/dev/fb0`（fbdev 语义）** | QPainter 后端的渲染目标就是 `QImage`（`virtual_qpainter_backend.cpp:47`），天然可 blit |
| C3 | **不要**用 legacy 最小 KMS 去喂 KWin 的 DRM 后端 | KWin 6.5 的提交全是 `DRM_MODE_ATOMIC_*`（`src/backends/drm/drm_commit.cpp:107,116,121,131`） |
| C4 | 编译路径选 **「宿主原型 + 搬发行版二进制走 ABI 层」**，不交叉编译 Qt | HBOS 已能跑未修改的 glibc 2.43 动态程序（`docs/wiki/LINUX_KDE_COMPAT.md:46`）；Chromium 也是搬二进制（296 MB，`docs/jindu.md:1311`） |
| C5 | logind 不是阻塞项；DBus **是**（对 Plasma，对 KWin 单体不是） | `Session::create()` 自动降级到 `NoopSession`（`src/core/session.cpp:20-31`）；但 `plasma-workspace` 依赖 DBus |

### 0.4 需要 Lead 裁决的三件事

1. **gfx 的目标要不要改**：从「legacy 最小 KMS」改为「`/dev/fb0` fbdev shim（open/mmap/ioctl）先上，
   atomic KMS 推到 M4」？——这是本文档最关键的资源重排（理由见 §3.2）。
2. **版本锁定**：是否接受「KWin 6.5.x / Qt 6.9 / KF6 6.18」作为唯一支持版本？
   接受则放弃 6.8+ 的新特性；不接受则 EGL/Mesa llvmpipe 变成 M3 的前置，工期 ×3。
3. **Plan B 第 1 步是否立项**（极简 Wayland 客户端 + HIVE 出窗，1 人周）。

---

## 1. 现状基线（HBOS 侧，逐条带证据）

| 能力 | 现状 | 证据 |
|---|---|---|
| glibc 动态程序 | 可用基线：未修改 glibc 2.43 loader + 2.1 MiB libc、符号版本、malloc、pthread/静态 TLS 已在 QEMU 通过 | `docs/wiki/LINUX_KDE_COMPAT.md:46` |
| musl 动态程序 | 可用基线 | 同上文件 `:45` 附近表格 |
| C++ 运行时 | **完全没有**：`grep -rIl 'libstdc++\|_Unwind_Resume\|__cxa_throw' src/` = 0 命中；`src/user/libc/` 48 个文件里没有 libstdc++ / unwinder / exceptions 相关实现 | 本仓库实测（命令见 §7） |
| Wayland 服务端 | 自研，非 libwayland：`wl_compositor` v4 / `wl_shm` v1 / `wl_seat` v5 / `xdg_wm_base` v2 | `src/gui/wayland/wl_protocol.h:141-144` |
| xdg_shell | 已实现 `xdg_wm_base + xdg_surface + xdg_toplevel + xdg_popup`，`get_toplevel` 后主动发 configure | `src/gui/wayland/iface/wl_iface_xdg.c:3,8` |
| 合成/呈现 | HIVE 把画布整块 blit 到硬件帧缓冲，纯 CPU | `src/graphics/graphics.c:31-32,73` |
| DRM/KMS | **无**。`grep -rn '"/dev/' src/` 只出现 `/dev/{console,null,ptmx,pts,random,shm,tty,urandom,zero}` | 本仓库实测 |
| `/dev/fb0` | **无**。framebuffer 只在内核里被直接写，没有用户态 mmap 通道 | 同上 + `src/graphics/graphics.c` |
| EGL / GBM / GL | **无** | 本仓库无 `libEGL`/`gbm`/`egl` 实现；`docs/wiki/LINUX_KDE_COMPAT.md:304` |
| DBus daemon | **无**（只完成了 AF_UNIX + `SCM_RIGHTS` 传输底座） | `docs/wiki/LINUX_KDE_COMPAT.md:69,304` |
| logind / udev / libinput | **无** | `docs/wiki/LINUX_KDE_COMPAT.md:304` |
| evdev 设备节点 | **无**；输入是 PS/2 轮询后经 seat 分发 | `src/tools/gui.c:960,1200,1206`（`key_poll`/`mouse_poll`） |
| fd 上限 | 每进程 128 | `docs/wiki/LINUX_KDE_COMPAT.md:32` |
| SCM_RIGHTS 容量 | 每消息最多 4 个 fd、每 socket 最多排队 4 组 | `docs/wiki/LINUX_KDE_COMPAT.md:290` |
| 可写临时目录 | `/tmp` `/run` `/dev/shm` 已是真 tmpfs（Lead 2026-11-14 通报；`docs/jindu.md:1257-1259` 记的是历史状态） | Lead 通报 |
| 内核内合成 | HIVE 自家合成器可用（surface attach/commit/damage 已通） | `WAYLAND_PLAN.md` P0–P3 |

> 注：**「HBOS 有 Wayland 服务端」不等于「KWin 能复用」**。KWin 自己就是 Wayland 服务端，
> 它链接 `libwayland-server` + wayland-protocols 生成的代码，而不是本仓库的 `src/gui/wayland`。
> 因此落地 KWin 时要额外移植 **libwayland（C，约万行量级）**——这是 M2 的一部分，不是 M0。

---

## 2. 依赖清单表（问题 a）

### 2.1 KWin 上游版本与硬门槛

| 版本 | PROJECT_VERSION | Qt 最低 | KF6 最低 | 软件合成 |
|---|---|---|---|---|
| v5.27.11 | 5.27.11 | Qt5 | — | 有（25 个 qpainter 文件） |
| v6.5.0 | 6.5.0 | **6.9.0** | **6.18.0** | 有（16 个 qpainter 文件） |
| v6.6.0 | 6.6.0 | 6.10.0 | 6.22.0 | 有（16 个 qpainter 文件） |
| v6.7.0 | 6.7.0 | 6.10.0 | 6.26.0 | 有（20 个 qpainter 文件，最后一个带 QPainter 的版本线） |
| master | 6.8.80 | 6.11.0 | 6.30.0 | **无**（只剩 qpainterswapchain） |

证据：各 tag 的 `CMakeLists.txt:3,11,12`；`git ls-tree -r <tag> | grep -ci qpainter`。

### 2.2 Qt6 模块（KWin 6.5 硬依赖，`CMakeLists.txt:58-70`）

| 模块 | 必需性 | 体积（Arch 安装后） | 能否裁剪 | 备注 |
|---|---|---|---|---|
| Qt6 Core / Gui / Widgets | 必需 | 含在 qt6-base | **不可裁** | KWin 是 QWidget/QPainter 程序 |
| Qt6 DBus | 必需 | 含在 qt6-base | 不可裁（编译期） | 运行时无 daemon 时部分功能降级 |
| Qt6 Quick（QML） | 必需 | 含在 qt6-declarative **135.16 MiB** | **不可裁** | KWin 链接 Qt6::Quick；Plasma 更是 QML 重负载 |
| Qt6 WaylandClient | 必需（编译期） | qt6-wayland **5.37 MiB** | 条件可裁 | 嵌套 Wayland / KWayland 客户端侧 |
| Qt6 Svg | 必需 | 1.06 MiB | 不可裁 | 图标/装饰 |
| Qt6 Core5Compat | 必需 | 2.42 MiB | 不可裁 | KWin 6.5 仍链 |
| Qt6 UiTools / Sensors / Concurrent | 必需 | 含在 qt6-tools（22.37 MiB）/ base | 不可裁 | UiTools 拉起 qt6-tools |
| qt6-base 合计 | — | **67.56 MiB** | — | https://archlinux.org/packages/extra/x86_64/qt6-base/ |

### 2.3 KF6 / Plasma 模块（`CMakeLists.txt:94-110`）

| 模块 | 体积 | 说明 |
|---|---|---|
| KF6 Auth / ColorScheme / Config / CoreAddons / Crash / DBusAddons | 0.07–3.28 MiB 各 | 基础框架 |
| KF6 GlobalAccel | 0.30 MiB | 全局快捷键 |
| KF6 GuiAddons / I18n / IdleTime / Package / Service / Svg / WidgetsAddons / WindowSystem | **ki18n 18.25 MiB**，其余 0.2–0.9 MiB | I18n 体积主要来自翻译数据 |
| KF6 KIO（可选） | 19.84 MiB | 仅 kcms/文件对话框需要，KWin 单体可省 |
| KDecoration3 | 0.41 MiB | **必需**，窗口装饰 API |
| KWayland（server） | 1.25 MiB | **必需**，KWin 的 Wayland 协议层 |
| PlasmaActivities / libplasma | 0.44 / 8.27 MiB | Plasma 阶段才需要；KWin 单体可选 |
| kwin 本体 | **31.67 MiB** | 含 effect/QML 资源 |

### 2.4 系统库

| 库 | 必需性 | 体积（Arch） | 能否裁剪 | 备注 |
|---|---|---|---|---|
| libwayland-server/client ≥ 1.24 | **必需** | wayland 0.88 MiB | 不可裁 | HBOS 需移植；socket/eventfd/memfd/epoll 已有 |
| wayland-protocols ≥ 1.45 | 编译期必需 | 小 | 不可裁 | 生成协议代码 |
| libxkbcommon ≥ 0.7 | **必需** | 1.08 MiB + 数据 | 不可裁 | 键位映射；需要 xkb 数据文件 |
| libepoxy ≥ 1.3 | **必需（编译+链接）** | 2.70 MiB | 不可裁 | `OutputBackend` 头直接 include |
| EGL（libEGL.so） | **必需（链接）** | Mesa 的一部分 | 可先桩 | QPainter 路径不初始化 EGL，见 §0.1(2) |
| libdrm ≥ 2.4.118 | 编译期必需（qpainter 后端也用 `drm_fourcc.h`/`DrmDevice`） | 1.31 MiB | 不可裁 | 构建 drm 后端时必需；纯 virtual 型后端可绕开 |
| gbm | 仅 GL / DRM 层 | 小 | **可裁** | QPainter 路径不需要 |
| Mesa（llvmpipe） | 仅 GL 路径 | **53.90 MiB** + LLVM | **可裁（M3 前）** | 第一帧不需要 |
| Vulkan | master 必需，6.5 非必需 | 大 | 可裁 | 锁 6.5 即可避开 |
| DBus（libdbus + daemon） | 运行时必需（Plasma） | 1.03 MiB + daemon | 不可裁 | KWin 单体可在无 daemon 下降级 |
| logind / systemd-libs | **可规避** | systemd 40.57 MiB | **可裁** | `Session::create()` 降级到 Noop（`session.cpp:20-31`） |
| libinput ≥ 1.26 / udev | 真实输入需要 | 0.81 MiB + libudev | **可先绕开** | 可先用 `org_kde_kwin_fake_input` 协议注入（`fakeinputbackend.cpp:28`） |
| fontconfig / freetype2 | **必需** | 1.37 + 1.78 MiB | 不可裁 | 字体；中文字体另算（Noto CJK 60–200 MB） |
| ICU | **必需** | **43.23 MiB** | 不可裁（发行版 Qt 链接 ICU） | 若自编 Qt 可 `-no-icu`，但会破坏 locale 行为 |
| glibc | **必需** | 50.66 MiB | 不可裁 | HBOS 已有 2.43 兼容基线 |
| libcanberra | **编译期必需（上游 `REQUIRED`，6.5 `CMakeLists.txt:235`）** | 小 | 可桩 | 事件音；无音频时可先给桩 |
| lcms2 / hwdata | 可选 | 小 | 可裁 | 色彩管理 / 显示数据库 |

### 2.5 体积汇总（不含字体与图标主题）

| 组 | 估算 |
|---|---|
| Qt6（base + declarative + wayland + svg + 5compat） | ≈ 212 MiB |
| KF6 核心（不含 KIO/Plasma；含 KWayland 前约 31 MiB） | ≈ 31 MiB |
| KDecoration3 + KWayland + libplasma | ≈ 10 MiB |
| kwin 本体 | ≈ 32 MiB |
| 系统库（glibc + icu + wayland + xkb + freetype/fontconfig + drm + dbus + epoxy） | ≈ 102 MiB |
| **小计** | **≈ 390 MiB** |
| + 中文字体（Noto CJK）×1–2 + 图标主题 + QML 模块 + locale | 实际镜像 **600–900 MiB** |

结论：体积不是障碍（Chromium 单二进制就 296 MB，`docs/jindu.md:1311`），
**但「能不能裁」这条路上几乎没有余量**——KWin 6.5 的 Qt Quick / ICU / KF6 都是硬链。

### 2.6 「绕不过」与「可选」的分界

- **连 QPainter 路径都绕不过**：glibc+C++ 运行时、libwayland(-server)、wayland-protocols、
  xkbcommon、libepoxy+libEGL.so、libdrm 头、Qt Core/Gui/Widgets/DBus/Quick/WaylandClient/Svg/UiTools、
  KF6 核心、KDecoration3、KWayland、fontconfig/freetype、ICU。
- **只有 GL 路径要**：Mesa/llvmpipe、gbm、Vulkan。
- **可以晚点再要**：libinput/udev、DBus daemon、logind、PipeWire、lcms2（libcanberra 是编译期 REQUIRED，但可以桩）。
- **永不需要（HBOS 侧）**：X11/Xwayland（除非要跑 X 应用）、systemd 本体。

---

## 3. 后端选型（问题 b）

### 3.1 四条路评分

| 路 | 上游现状 | HBOS 现状（无 DRM / 无 EGL） | 可见输出 | 结论 |
|---|---|---|---|---|
| **drm** | `src/backends/drm/`，atomic 提交 + QPainter/EGL 两套 layer | 只有 legacy 最小 KMS；无 atomic 对象模型 | 有（真扫屏） | **后置到 M4**；legacy KMS 不够 |
| **headless** | **不存在**（6.5 与 master 的 `src/backends/` 都没有 headless） | — | 无 | 忽略；等价物是 `--virtual` |
| **virtual** | `src/backends/virtual/`，608 行；`present()` 丢弃帧 | 可直接编译 | **无** | 只作为**代码模板**，不能直接用 |
| **自定义 platform plugin** | 无此机制（KWin 后端是编译进 kwin 的 C++ 类，不是插件） | — | 取决于实现 | **推荐**：新增一个 virtual 型后端 |

> 「自定义 platform plugin」这个词在 KWin 语境里不成立：KWin 的后端是 `OutputBackend` 子类，
> 在 `main_wayland.cpp` 里 by enum 构造（6.5.0:459，master:517）。**要做的是加一个后端类并加一个枚举分支**，
> 不是写 Qt Platform Plugin。这一点影响工作量估算（见 3.5）。

### 3.2 drm 后端：为什么 gfx 的最小 KMS 不够

gfx-foundation 当前的 ioctl 清单是 `VERSION / GET_CAP / GET_RESOURCES / CREATE_DUMB / MAP_DUMB /
ADDFB2 / SET_CRTC / PAGE_FLIP`——这是 **legacy modeset** 接口集。KWin 6 的 DRM 后端要求的是
**atomic modeset**：

- `drm_commit.cpp` 全部走 `DRM_MODE_ATOMIC_*`：
  `DRM_MODE_ATOMIC_TEST_ONLY | NONBLOCK`（:107）、`TEST_ONLY | ALLOW_MODESET`（:116）、
  `NONBLOCK | PAGE_FLIP_EVENT`（:121）、`ALLOW_MODESET`（:131）
  （`https://invent.kde.org/plasma/kwin/-/blob/v6.5.0/src/backends/drm/drm_commit.cpp#L107`）。
- `DrmBackend` 构造需要 `Session*`（`drm_backend.cpp:71`），`supportInformation()` 打印
  `Atomic Mode Setting`（:346）——即运行时按 GPU 能力分支，legacy 卡直接降级失败。
- 还需要（上游代码里逐项用到）：connector/CRTC/plane 的 **property 枚举与 blob**（EDID、IN_FORMATS、
  CRTC_ID、FB_ID、SRC_W/H、CRTC_X/Y/W/H、ACTIVE、DPMS）、**page flip event**、**GEM mmap**、
  `DRM_CLIENT_CAP_ATOMIC`（GET_CAP 返回 true）、以及热插拔/uevent。
- 即使是 `drm_qpainter_backend.cpp`（6.5 有，66 行；dumb framebuffer 在
  `drm_qpainter_layer.cpp:65,79` 创建）也仍然通过同一套 atomic commit 提交（`drm_commit.cpp:107-131`），
  并不会退回 `SET_CRTC`。

**结论**：`SET_CRTC + PAGE_FLIP` 能让 KWin「认得出卡」的概率很低，即便认出来，第一次 modeset 就会失败。
把 KMS 目标从 legacy 改成 atomic 的代价 = 一个完整 DRM 对象模型（估 3–6 人月，且需要真机/多 GPU 验证）；
而 **`/dev/fb0` shim 的代价是 200–400 行内核 C**，且立刻能让 QPainter 后端出画面。

### 3.3 headless：不存在

- 6.5.0 `src/backends/`：`drm fakeinput libinput virtual wayland x11`；master 完全相同。
- `main_wayland.cpp` 里只有 `--virtual`（6.5.0:311；master:362），没有 `--headless`。
- 因此「用 headless 后端跑 KWin 再截帧」这条路在 6.5 上 = `--virtual` + screencast，**不产出可显示的帧**。

### 3.4 virtual：能编译，但 `present()` 把帧扔掉

```cpp
// v6.5.0 src/backends/virtual/virtual_output.cpp:56-61
bool VirtualOutput::present(const QList<OutputLayer *> &layersToUpdate, const std::shared_ptr<OutputFrame> &frame)
{
    m_frame = frame;
    m_vsyncMonitor->arm();
    return true;
}
```

`--virtual` 的用途是 CI：QPainter 已经把合成结果画进 `QImage`，但没人读这块内存。
**这正是我们的接入点**：把 `present()` 换成「读 layer 的 `QImage` → blit 到 `/dev/fb0`」。

### 3.5 自定义后端：接口清单与行数估算

以 v6.5.0 为基线（注意：6.5 的输出基类就叫 `Output`（`src/core/output.h`）；
6.8 才拆成 `BackendOutput` + `LogicalOutput`，接口与行号不能照搬 master）。

必须实现/覆写（`src/core/outputbackend.h`）：

| 接口 | 位置 | 说明 |
|---|---|---|
| `bool initialize() = 0` | `outputbackend.h:50` | 打开 `/dev/fb0`、读 `FBIOGET_VSCREENINFO`、mmap |
| `std::unique_ptr<QPainterBackend> createQPainterBackend()` | `:53` | 返回我们的 QPainter 后端 |
| `EglDisplay *sceneEglDisplayObject() const = 0` | `:55` | **返回 `nullptr`**（KWin 允许「不使用 EGL 的后端」） |
| `QList<CompositingType> supportedCompositors() const = 0` | `:75` | 返回 `{QPainterCompositing}` |
| `std::unique_ptr<InputBackend> createInputBackend()` | `:51` | 返回 nullptr 或自定义输入后端（先用 fakeinput 协议） |
| `QList<Output*> outputs() const` | 非纯虚 | 单输出返回自己 |

必须实现的输出类（`src/core/output.h`）：`renderLoop()`（:326）、
`testPresentation()`（:420）、`present()`（:421）三个纯虚 + `init()` / `applyChanges()`（照 `virtual_output.cpp`）。

必须实现的渲染类（`src/qpainter/qpainterbackend.h` + `src/core/renderbackend.h`）：
`QPainterBackend` 子类 + 一个 `OutputLayer` 子类（照 `VirtualQPainterLayer`），
在 `doBeginFrame()` 里用 `QPainterSwapchain(ShmGraphicsBufferAllocator, size, DRM_FORMAT_XRGB8888)`
拿到 `QImage`（`virtual_qpainter_backend.cpp:33-50`），在 `doEndFrame()` 拿到 `damagedRegion`（:52-57）。

**行数估算（有依据的估计，不是拍脑袋）**：

| 部分 | 依据 | 估算 |
|---|---|---|
| 后端 + 输出 + QPainter layer | v6.5.0 virtual 后端全部 6 个文件合计 **608 行**（含版权头） | 400–600 行 C++ |
| fbdev 打开/mmap/格式转换/damage 聚合/vsync 定时 | 新增（虚拟后端没有） | 200–300 行 C++ |
| 内核 `/dev/fb0` shim（open/mmap/ioctl） | 新增，内核 C | 200–400 行 C |
| 输入后端（可选，先用 fakeinput） | `org_kde_kwin_fake_input` 协议已在上游 | 0 行（首版）/ 150–250 行（自定义） |
| **合计（首版可显示）** | — | **≈ 800–1300 行（其中内核 200–400）** |

对照：`drm_egl_backend.cpp` 85 行只是薄封装，真正的重量在 `src/opengl/` 与 `src/backends/drm/` 的
layer/commit/pipeline 上；所以「不写 EGL 后端」省掉的不是 85 行，而是整个 `src/opengl` + Mesa。

### 3.6 后选定论

```
KWin 6.5.x
  ├─ CompositingType = QPainter（纯 CPU，Qt Gui raster）
  ├─ HbosBackend : OutputBackend      （new, ~100 行）
  ├─ HbosOutput  : Output             （new, ~250 行）
  ├─ HbosQPainterLayer : OutputLayer  （new, ~150 行，抄 VirtualQPainterLayer）
  └─ present(): QImage → /dev/fb0 mmap（damage 区域逐行 blit）
        └─ 内核 fbdev shim（new, 200–400 行，gfx）
```

---

## 4. 编译路径（问题 c）

### 4.1 三条路对比

| 路 | 做法 | 前置 | 代价 | 风险 | 评价 |
|---|---|---|---|---|---|
| **A. 交叉编译 Qt6/KF6 到 HBOS ABI 层** | 自建 sysroot + 交叉工具链，从源码编 Qt 6.9（或 6.12）+ KF6 + KWin | C++ 运行时、libstdc++、unwinder、完整 TLS、CMake/moc/rcc 交叉运行 | **6–12 人月**（Qt6 本体 + KF6 60+ 仓库） | 上游不认非 Linux 构建；`qmake`/`moc` 需宿主运行；每次升级重来 | 不推荐先做 |
| **B. glibc 静态链接** | 把 Qt/KWin 静态链进一个二进制 | 同 A（仍要编 Qt） | 同 A，且更难（Qt 插件/QML 模块不能静态化，KWin 依赖 QML 资源） | 静态化后 `dlopen` 类功能失效，Qt 插件体系崩 | **不解决问题** |
| **C. 宿主原型 + 搬发行版二进制走 ABI 层** | 宿主编/直接用发行版包；HBOS 侧只补 ABI 与设备节点 | 宿主有 Qt/KF6（可直接 `pacman -S kwin`）；HBOS 补 statx/ppoll/TLS/IFUNC 等 | **2–4 人月**（ABI 补洞是变量） | 发行版依赖树大（≈200 包）、需要 dbus/udev 兼容 | **推荐** |

### 4.2 为什么 C 是唯一现实路径

- HBOS 的兼容层目标本来就是「现成 Linux 二进制直接运行」：
  glibc 2.43 loader/libc、符号版本、malloc、pthread/静态 TLS 已在 QEMU 通过
  （`docs/wiki/LINUX_KDE_COMPAT.md:46`），且 Chromium 就是搬 Debian 二进制
  （296,696,664 字节，`docs/jindu.md:1311`）。
- 交叉编译 Qt 等于在 HBOS 上重建一个 Linux 发行版的 C++ 生态，且上游不提供支持；
  而 ABI 补洞是**可增量验收**的：每补一个 syscall 就能点亮一批二进制。
- 版本锁定后，二进制集合可以冻结成一份「KDE runtime 镜像」，与内核解耦。

### 4.3 推荐路线（分三步，对应 M1–M3）

1. **宿主原型（M1）**：宿主 `pacman -S kwin` / 或从 v6.5.0 源码编（宿主有 Qt 6.12 / KF6，需注意
   6.12 > 6.9 的 API 差异；建议用发行版自带的 kwin 包先验证后端接口形态，再决定是否源码编 6.5）。
   产出：`HbosBackend` 三件套 + 帧落盘 + **一份真实用到的 API 清单**。
2. **ABI 层搬二进制（M2）**：把 Qt6/KF6/KDecoration3/KWayland/kwin 的 Debian 包解包进镜像
   （和 Chromium 同套路），先跑 `qmlscene`/`kwin_wayland --version` 这类小目标，
   用 `ldd` + 启动日志逐项补 ABI（重点：`statx`、`ppoll`、`clone3`、`rseq`、`membarrier`、
   `sched_setattr`、`getrandom`、TLS/IFUNC、`/proc/self/maps` 语义）。
3. **换 sink（M3）**：把 M1 的后端接上内核 `/dev/fb0` shim，KWin 直接接管屏幕。

### 4.4 如果坚持自编：最小可行顺序（仅作备选）

`libstdc++/libgcc_eh` → `libwayland` + `wayland-protocols` → `xkbcommon` →
`freetype/fontconfig` → `Qt6 Base`（`-no-icu -no-opengl -no-xcb`，保留 raster/wayland）→
`Qt6 Declarative` → `KF6 核心` → `KDecoration3/KWayland` → `KWin`。
每步都以「一个能跑的最小程序」验收，不要把 12 步串成一次构建。

---

## 5. 分阶段里程碑表（问题 d）

代价按「1 名熟悉本仓库的工程师」计，单位为**人周**；所有验收都以「可观察事实」为准。

| # | 里程碑 | 前置条件 | 交付/验收 | 代价 | 依赖 gfx KMS？ |
|---|---|---|---|---|---|
| **M0** | 一周可见：HBOS 上第一个 Wayland 客户端窗口 | 无（现有 `src/gui/wayland` + HIVE） | 手写客户端（300–500 行 C）连 `/tmp/wayland-0`，完成 registry→`xdg_toplevel`→`wl_shm` attach/commit；HIVE 里看到窗口、能拖动、键盘输入进窗口；`[WL]` 日志有 configure/ack 往返 | **1** | **否** |
| **M1** | 宿主原型：KWin 6.5 + HbosBackend 帧落盘 | 宿主 Linux + Qt/KF6；v6.5.0 源码 | 新增 `backends/hbos/` 三件套；`kwin_wayland --hbos --width 1280 --height 800` 起得来；每帧 PPM 可看；`--virtual` 对照不崩；输出「实际用到的 API 清单」 | **1–2** | **否** |
| **M2** | Qt6/KF6 二进制在 HBOS 跑起来（无 KWin） | M1 的 API 清单；tmpfs（已有）；ABI 补洞 | 解包 KDE runtime；`kwin_wayland --version`、一个 Qt 例子（如 `qmlscene`）在 HBOS 出窗口（接 M0 的 HIVE 服务端或 M1 的后端）；`ldd` 全解析、无 SIGILL | **4–6** | **否** |
| **M3** | KWin 在 HBOS 出第一帧（fbdev + QPainter） | M1 + M2 + `/dev/fb0` shim | KWin 接管屏幕（HIVE 退出）；出现 KWin 自己管理的窗口（debug console / Qt 客户端）；键鼠可用（fakeinput 协议注入）；连续 30 帧无撕裂崩溃 | **3–4** | **否** |
| **M4** | DRM/atomic 后端 + 多输出/热插拔 | M3；gfx 完成 atomic KMS（对象/property/blob/plane/page-flip/GEM mmap） | KWin `--drm` 起得来；`drm_qpainter_backend` 真扫屏；拔插 HDMI 不崩；DPMS 生效 | **6–10（gfx）+ 2（KWin）** | **是** |
| **M5** | Mesa llvmpipe GL 合成 | M4 或 M3 + EGL 桩替换 | `KWIN_COMPOSE=OpenGL` 起得来；1080p 合成 ≥30 fps（CPU 预算见 §6） | **4–8** | 否（M3 路线）/ 是（M4 真扫屏） |
| **M6** | Plasma 桌面本体 | M5 + DBus daemon + udev + Plasma 栈 | `plasmashell` 起得来：面板、启动器、窗口装饰、Dolphin、Konsole | **8–16** | 否 |
| **M7** | Debian Chromium/Electron 在 KWin 里跑 | M6 + wayland 客户端栈 + 现有 Chromium 移植 | Chromium 用 `--ozone-platform=wayland` 在 KWin 里出窗口；bilibili Electron 同 | **4–8** | 否 |

**关键路径**：M0 → M1 → M2 → M3 →（M6 → M7）。
**KMS 不在关键路径上**：只有 M4（真扫屏/多屏）和 M5 的硬件扫描输出需要它。
若只追「在 HBOS 上看见 KWin 窗口」，M1/M2/M3 加起来 8–12 人周，**不需要** gfx 做 atomic KMS。

### 5.1 M0 一周拆解（Plan B 的可执行版）

| 日 | 任务 | 产出/验收 |
|---|---|---|
| D1 | 读 `src/gui/wayland/` 的 display/compositor/seat/xdg 接口，写最小客户端骨架（socket 连接 + registry） | 客户端能绑定 `wl_compositor/wl_shm/wl_seat/xdg_wm_base` |
| D2 | `wl_shm` pool + buffer + `xdg_surface.get_toplevel` + `set_title` + `commit`；处理 `configure` → `ack_configure` → attach → commit | HIVE 里出现 400×300 的纯色窗口 |
| D3 | `wl_seat` 指针/键盘事件；`xdg_toplevel` move/resize（用 `wl_pointer` + 服务端 move 请求） | 窗口能拖动、按键能改颜色（证明输入闭环） |
| D4 | 把 M1 的宿主原型搭起来（KWin 6.5 `--virtual` + `backends/hbos/` 空壳） | KWin 启动日志里出现 HbosBackend；`--virtual` 对照不崩 |
| D5 | `present()` 落 PPM；跑 `kwin_wayland --hbos` 用 `weston-simple-shm`/自写客户端连进去 | 每帧 PPM 可看，窗口内容正确 |
| D6 | 把 sink 抽象成 `hbos_fb_write(rect, pixels)`；宿主用文件、HBOS 用 `/dev/fb0` | 同一份后端代码两种 sink 编译通过 |
| D7 | 写 `docs/wiki/KWIN-PLAN.md` 增补「实测 API 清单」+ 给 gfx 的 fbdev 契约草案 | 后端接口清单冻结，下一步可派工 |

> M0 的 D1–D3 **不依赖 KWin/Qt**，是真正的「一周内看见东西」；
> D4–D7 依赖宿主 Qt/KF6，但只在宿主上跑，不占 QEMU。

### 5.2 Plan B 备选与取舍

| 方案 | 能否一周见画面 | 评价 |
|---|---|---|
| **HBOS 自研服务端 + 手写客户端** | **能**（唯一确定能） | 复用 `src/gui/wayland`，不引入任何新依赖；同时是 KWin 迁移的协议探针 |
| wlroots + tinywl | 不能 | wlroots 的 DRM 后端要 libdrm+gbm+EGL；headless 后端可走 pixman，但仍要移植 libwayland/wlroots 本体与整套协议栈；1 周只够编依赖 |
| weston（fbdev 后端） | 不确定 | 需核实所选 weston 版本是否仍有 fbdev 后端；且同样要 libinput/evdev 设备节点（HBOS 没有 `/dev/input/*`） |
| 直接上 KWin `--virtual` | 不能 | `present()` 丢帧（`virtual_output.cpp:56-61`），永远看不到 |
| 直接上 KWin `--drm` + gfx 最小 KMS | 不能 | 见 §3.2，atomic 缺失 |

### 5.3 各阶段需要 gfx/其他线提供什么

| 阶段 | 需要谁 | 具体物 |
|---|---|---|
| M0 | gui 线（HIVE/wayland 服务端） | 现有 `wl_server` 的 xdg_toplevel move/resize、`wl_shm` 格式确认（ARGB8888/XRGB8888） |
| M1 | 无 | 宿主 Linux 即可 |
| M2 | fs 线 | tmpfs 已就绪；`/proc` 语义补强（`/proc/self/maps`、`/proc/self/fd`） |
| M3 | **gfx** | **`/dev/fb0`**：open、`FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO`、`mmap`（write-combined 或普通内存）、可选 `FBIOPAN_DISPLAY`/page flip 通知 |
| M4 | **gfx** | atomic KMS（见 §3.2 清单）+ 热插拔 uevent + `DRM_IOCTL_MODE_GETPROPBLOB` |
| M5 | gfx | 只走 QPainter 时：链接期 EGL 桩即可（返回 `EGL_NO_DISPLAY`，GL 路径自动降级）；要走 GL 时：Mesa llvmpipe 移植 |
| M6 | 全栈 | dbus-daemon 跑起来、udev 兼容（`/run/udev`）、字体/图标主题、xkb 数据 |

---

## 6. 风险与不建议项

### 6.1 风险表

| 风险 | 触发点 | 影响 | 缓解 |
|---|---|---|---|
| **上游删掉 QPainter** | 升级到 6.8+ | 必须上 EGL/Mesa，工期 ×3 | 锁 6.5.x；升级前 `git ls-tree <tag> \| grep -i qpainter` 卡口 |
| **C++ 运行时缺失** | M2 首次启动 Qt 程序 | `SIGILL`/未定义符号 | 先跑最小 C++ 程序验证 `libstdc++`/unwinder/TLS；把 `__cxa_*`/`_Unwind_*` 列入 ABI 清单 |
| **SCM_RIGHTS 容量 4 fd/消息、4 组排队** | Wayland 每帧 fd 传递 + KWin 大量 client | 客户端卡死/丢事件 | M2 前扩容到 ≥64 组（`docs/wiki/LINUX_KDE_COMPAT.md:290`） |
| **fd 上限 128/进程** | KWin + 多客户端 + dmabuf | `EMFILE` | M2 前提到 1024 并改就绪队列（`LINUX_KDE_COMPAT.md:32`） |
| **无 vDSO** | `clock_gettime` 高频 | Qt 定时器开销大 | 可接受；M5 再评估 |
| **CPU 合成带宽** | 1080p@60 XRGB8888 = 1920×1080×4×60 ≈ **475 MiB/s** 读 + 写 | 卡顿 | 依赖 QPainter 的 `damagedRegion`（`virtual_qpainter_backend.cpp:52`）做局部 blit；必要时降到 720p/30 |
| **没有 `/dev/fb0`** | M3 开始 | 后端无 sink | gfx 提供 fbdev shim（200–400 行） |
| **DBus daemon 缺失** | M6 | plasmashell 起不来 | M6 前置任务；可先用 `dbus-broker`/`dbus-daemon` 二进制 |
| **libinput/udev 缺失** | 真实输入 | 只能靠 fakeinput 协议 | 先用 `org_kde_kwin_fake_input` 把 HIVE 输入注入；M4 后再接 evdev |
| **发行版 KWin 版本漂移** | M2 用 Arch/Debian 包 | 可能拿到 6.8（无 QPainter） | 冻结包版本，或自建最小 6.5 包；镜像里记录版本清单 |
| **QML/Plasma 资源路径** | M6 | 图标/主题缺失 | `/usr/share/{plasma,qml,icons,locale}` 一并打包 |

### 6.2 不建议项（明确不做）

1. **不建议跟 KWin master/6.8+**：QPainter 已删，等于强制 Mesa + EGL 驱动（§0.1）。
2. **不建议从天价交叉编译 Qt6 起步**：6–12 人月、无上游支持、每次升级重来（§4.1-A）。
3. **不建议用 glibc 静态链接绕过**：Qt 插件/QML 模块无法静态化，KWin 依赖 QML 资源（§4.1-B）。
4. **不建议用 legacy 最小 KMS 冒充 KWin 的 DRM 后端**：KWin 只走 atomic（§3.2）。
5. **不建议 ANGLE/SwiftShader**：绑 Vulkan/D3D，KWin 不认；要 GL 就 llvmpipe（§0.1）。
6. **不建议先移植 wlroots 当跳板**：它同样要 DRM+EGL（或 pixman）+ libinput，代价高于 M0 手写客户端，
   且对 KWin 迁移没有复用价值（§5.2）。
7. **不建议在 M0 之前动内核图形栈**：`/dev/fb0` 只服务 M3，提前做会阻塞 gui 线的 M0。

### 6.3 决策点（必须在对应阶段前定）

| 决策 | 截止 | 选项 |
|---|---|---|
| 目标 KWin 版本 | M1 前 | 6.5.x（推荐）/ 6.7（Qt 6.10）/ 跟 master（需 GL） |
| 二进制来源 | M2 前 | Debian 包（与 Chromium 同源；须选 KWin 6.x 的版本）/ Arch 包 / 自编最小集 |
| 输入首版 | M3 前 | fakeinput 协议（0 行，推荐）/ 自定义 InputBackend（150–250 行） |
| KMS 归属 | M4 前 | gfx 做 atomic / 永久用 fbdev（放弃多屏与硬件合成） |

---

## 7. 证据与复现命令

```sh
# 1) KWin 版本与依赖门槛
git clone --depth 1 --filter=blob:none --no-checkout https://invent.kde.org/plasma/kwin.git /tmp/kwin-src
cd /tmp/kwin-src && git sparse-checkout init --cone \
  && git sparse-checkout set src/backends src/core src/qpainter src/scene src/CMakeLists.txt CMakeLists.txt src/main_wayland.cpp
git show v6.5.0:CMakeLists.txt | grep -E 'PROJECT_VERSION|QT_MIN_VERSION|KF6_MIN_VERSION'
git ls-tree -r v6.5.0 | grep -i qpainter     # 6.7.0 也有；master 只剩 qpainterswapchain
# 2) QPainter 后端确实是内存 QImage
git show v6.5.0:src/backends/virtual/virtual_qpainter_backend.cpp | sed -n '30,60p'
# 3) DRM 后端只走 atomic
git show v6.5.0:src/backends/drm/drm_commit.cpp | grep -n DRM_MODE_ATOMIC
# 4) virtual 的 present() 丢弃帧
git show v6.5.0:src/backends/virtual/virtual_output.cpp | sed -n '56,61p'
# 5) HBOS 侧：没有 C++ 运行时 / 没有 /dev/fb0 / 没有 /dev/dri
grep -rIl 'libstdc++\|_Unwind_Resume\|__cxa_throw' src/ ; ls src/user/libc | wc -l
grep -rn '"/dev/' src/*.c src/*.h src/user/*.c | grep -oE '"/dev/[a-z0-9_/]*"' | sort -u
# 6) HBOS Wayland 服务端协议版本
sed -n '141,144p' src/gui/wayland/wl_protocol.h
# 7) 包体积（Arch 2026-11-14）
pacman -Si qt6-base qt6-declarative qt6-wayland kwin kdecoration kwayland icu mesa | grep -E '名字|安装后大小'
```

---

## 8. 一页纸行动清单

- [ ] Lead 裁决 §0.4 三件事（KMS 重排 / 版本锁 6.5.x / M0 立项）
- [ ] M0 派工：手写 Wayland 客户端（300–500 行 C）+ HIVE 验收（1 人周）
- [ ] M1 派工：`backends/hbos/` 三件套 + 帧落盘（宿主，1–2 人周）
- [ ] gfx 出 `/dev/fb0` 契约草案（ioctl 子集、mmap 语义、格式、vsync）
- [ ] fs/abi 线出「Qt6 二进制启动所需 syscall 清单」缺口表（M2 前置）
- [ ] 版本卡口脚本：`git ls-tree <tag> | grep -i qpainter` 进 CI，防止误升 6.8
