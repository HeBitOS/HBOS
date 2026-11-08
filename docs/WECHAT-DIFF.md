# 微信 Linux 版（wechat 4.1.13）对标分析

> 分析日期：2026-10-31
> HBOS 基线：`v0.2.1-pre1`（`version.mk`：MAJOR=0 / MINOR=2 / BETA=1 / PRE=1，`HIVE_GUI_REV=1`）
> 分析对象：腾讯微信 Linux 桌面客户端 `wechat 4.1.13.26`（amd64 deb），及其内置运行时 `WeChatAppEx`
> 分析方法：下载官方 deb 后 `dpkg-deb -x` 解包 + `readelf -d` / `objdump -p` / `strings` 静态分析（【实测】），辅以公开报道（【报道】）

## 阅读约定

本文对每条事实标注证据强度，请按标注决定是否作为决策依据：

- **【实测】**：我在本机对官方 deb 解包后的真实文件做的静态分析，可复现，附命令或文件名。这是本文最强的一级证据。
- **【报道】**：来自公开网页/媒体报道，未被我独立验证，可能与实测冲突；**冲突时以【实测】为准**。
- **【推断】**：我基于实测事实做的推理，未见直接证据，可能错。

关于 §0 中「对方做法」一列的来源，全部为【实测】，除单独标注者外。

---

## 结论摘要

1. **微信 Linux 版已经是「Qt 静态链接 + 内嵌整套 Chromium」的双运行时架构**，而不是传统的 Qt 单进程应用。【实测】主程序 `wechat` 无任何 `libQt*.so` 依赖但内部含 Qt 5.15.14 全部符号与 `qt.qpa`；`/opt/wechat/RadiumWMPF/runtime/WeChatAppEx`（243,674,232 B）是一个完整 Chromium 144 分发体。**判定：不构成对 HBOS 的威胁（微信是负载不是竞品），但它把 HBOS 的验收目标难度从"跑一个 App"提高到"同时跑 Qt 客户端 + Chromium 客户端"。**

2. **HBOS 合成器当前 7 个全局与微信两套运行时的最小期望集之间，缺口集中在 4 个扩展上**：`zwp_text_input_manager_v3`（输入法）、`wp_viewporter`（缩放）、`zxdg_decoration_manager_v1`（服务端装饰）、`wp_fractional_scale_manager_v1`（分数缩放）。【实测】依据是 `WeChatAppEx` 内 `Failed to bind ... global` 字符串全清单。**判定：值得抄——这 4 个扩展对 Debian Chromium 同样必需，做一次两个负载都受益。**

3. **HBOS 的 wl_seat 已经声明 KEYBOARD 能力，但从不发送 `wl_keyboard.keymap` 事件**。【实测】`grep -rn keymap src/gui/` 零命中，`wl_protocol.h` 无 `WL_KEYBOARD_E_KEYMAP` 常量。客户端（Qt/QtWayland 与 Chromium）在拿到 keymap 之前不会建立键位映射表，键盘输入会整体失效。**判定：必须补，这是"窗口出来了但打不了字"的直接原因。**

4. **微信主程序本身是 A 战略的极佳验证负载**：它只 `Depends: fonts-noto-cjk | google-noto-cjk-fonts`，`NEEDED` 里只有 glibc/glib/gio/gobject/fontconfig/dbus/libudev 这类基础库，GLIBC 符号版本最高 **2.26**，无 GLIBCXX/CXXABI。【实测】这意味着只要 HBOS 补齐 `/dev/dri`、`/dev/snd`、D-Bus socket、fontconfig 缓存，**微信主窗口是当前 HBOS 最有可能先跑起来的真实大型 GUI 应用**（比 Chromium 容易得多）。

5. **微信的 AI 化对 HBOS 是负面信号**：【报道】官方口径明确"在微信内打造专属 AI 智能体"，但交付形态是**聊天入口 + 小程序 + 云端 Agent**（元宝 2025-04 上线为微信好友形态，2026 Q3 计划全量），而不是新的桌面客户端。【报道】结论：**微信生态的 AI 能力不会给 HBOS 带来新的桌面负载，HBOS 不应把资源押在"微信 AI 客户端"上**。

---

## 0. 抓取方法与元信息

| 项目 | 内容 |
|---|---|
| 官方站点 | `https://linux.weixin.qq.com/`（【实测】访问成功） |
| 当前版本 | **4.1.13**（官网页面标注），deb 内 `Version: 4.1.13.26` |
| 架构支持 | X86 / ARM 提供 deb/rpm/AppImage；LoongArch 仅 deb |
| 下载 URL | `https://dldir1v6.qq.com/weixin/Universal/Linux/WeChatLinux_x86_64.deb` |
| 包体积 | 231,861,976 B（约 221 MiB），`Last-Modified: 2026-10-12` |
| 安装体积 | `Installed-Size: 771664`（约 754 MiB），解包后实测 754 MB |
| 包名 | `Package: wechat`，`Source: wechat-stable` |
| 唯一强依赖 | `Depends: fonts-noto-cjk \| google-noto-cjk-fonts`；`Pre-Depends: dpkg (>= 1.14.0)` |
| 许可证 | 腾讯专有（非开源），无源码 |
| 分析工具 | `dpkg-deb -I/-x`、`readelf -d/-V`、`objdump -p`、`strings`、`grep -c` |

### 抓取过程中的关键发现

1. **官网说的 4.1.13 与包内 4.1.13.26 是同一个东西**（第四段为构建号），不用纠结版本号差异。
2. **deb 的 `Depends` 只有一条字体依赖**。这是极端反常的——同类 Electron 应用通常依赖 `libnss3/libgbm/libxkbcommon0` 等十几条。原因是微信把 Qt 与大部分第三方库**静态链接**进了主程序，动态依赖只剩系统必备项。这对 HBOS 是重大利好：**要满足的动态链接面非常窄**。
3. **`/opt/wechat/RadiumWMPF/runtime/` 是一整个 Chromium 目录树**，包含 `icudtl.dat`、`snapshot_blob.bin`、`v8_context_snapshot.bin`、`libEGL.so`、`libGLESv2.so`、`libvk_swiftshader.so`、`vk_swiftshader_icd.json`、`libvulkan.so.1`、`libffmpeg.so`、`libtxffmpeg.so`、`wmpf_*.pak`、`locales/`、`crashpad_handler`。`WeChatAppEx` 内嵌字符串为 **`Chrome/144.0.7559.236`**。
4. **`WeChatAppEx` 同时是 Electron 发行体**：内部含 `___electron_webpack_init__`、`electron_common_native_image`、`ipcRenderer`、`contextBridge`、`sharedTexture` 等 Electron JS bundle 标记。【推断】微信小程序运行时（RadiumWMPF）在 Linux 上是基于 Electron + 定制 Chromium 构建的。
5. **两套 Wayland 客户端并存**：主程序是 Qt Wayland 客户端（含 `QWaylandWindow`/`QWaylandDisplay`，引用 `zwp_text_input_v3`、`zxdg_decoration_manager_v1`、`wl_shell`、`zwp_primary_selection`，**不引用** `zwp_linux_dmabuf_v1`）；`WeChatAppEx` 是 Chromium Ozone/Wayland 客户端（含完整 `ui/ozone/platform/wayland/host/` 源路径字符串）。两者能用同一套扩展协议落地，但 dmabuf 需求不同。

### 目录体积（【实测】`du -sh`）

| 目录 | 体积 |
|---|---|
| `/opt/wechat/RadiumWMPF` | 345M |
| `/opt/wechat/XFile` | 32M |
| `/opt/wechat/XEditor` | 28M |
| `/opt/wechat/ocr_model` | 27M |
| `/opt/wechat/vlc_plugins` | 11M |
| 顶层 `.so`（19 个） | 合计约 200M |

---

## 1. 对方定位与架构摘要

### 1.1 定位

微信 Linux 版是腾讯官方维护的**桌面聊天客户端**，定位与 Windows/macOS 版对齐，非社区移植。它不对外提供 SDK，也不开放协议——对 HBOS 而言它既不是竞品也不是合作方，而是一个**必须能跑起来的真实负载**，且是当前 Linux 桌面市场占有率最高的中文 GUI 应用之一。

### 1.2 架构（【实测】，三层）

```
┌─────────────────────────────────────────────────────────┐
│ /usr/bin/wechat  (181 MB, PIE, Qt 5.15.14 静态链接)      │
│   NEEDED: glib/gio/gobject/fontconfig/dbus-1/libudev     │
│           + libandromeda/libmmcronet/libvoip*/libowl/... │
│   自研 so: andromeda(网络) mmcronet(HTTP) confService    │
│            voipChannel/voipCodec/voipComm(音视频)        │
│            roam_migration ilink_wrapper owl mmmojo       │
└───────────────┬─────────────────────────────────────────┘
                │ fork/exec + IPC (mmmojo)
┌───────────────▼─────────────────────────────────────────┐
│ /opt/wechat/RadiumWMPF/runtime/WeChatAppEx (243 MB)      │
│   Chromium 144.0.7559.236 + Electron JS bundle           │
│   多进程：browser/gpu/renderer/utility                   │
│   Ozone/Wayland 或 Ozone/X11（运行时选择）               │
│   SwiftShader 软件 Vulkan 兜底                           │
└─────────────────────────────────────────────────────────┘
```

`.desktop` 文件中 `com.tencent.wechatappex.desktop` 的 `Exec=/opt/wechat/RadiumWMPF/runtime/WeChatAppEx`，`Comment` 直接写「微信浏览器」——**这坐实了 `WeChatAppEx` 就是微信内置浏览器/小程序运行时，且可作为独立入口启动**。【实测】

### 1.3 UI 工具包判定（含一处与公开说法矛盾）

- 主程序 `.comment` 段：`GCC 7.5.0 (Debian)`、`clang 18.1.8`、`clang 12.0.0`、`Linker: LLD 18.1.8`。【实测】
- 主程序 Qt 证据：`QStandardPaths`×6、`QApplication`×22、`QGuiApplication`×21、`qt.qpa`×22、`QWaylandWindow`/`QWaylandDisplay` 存在、版本串 `5.15.14` 存在、xcb 相关符号×108；**无 `libQt*.so` NEEDED**。【实测】
- **矛盾点**：【报道】有文章称微信 4.0 起"界面层采用原生 QWidget 渲染"，但**没有任何实测证据能把 GTK 拉进来**——主程序与 `WeChatAppEx` 的 `NEEDED` 里都**没有 `libgtk-3.so.0`**。`WeChatAppEx` 虽然 `NEEDED` 了 `libatk`/`libatk-bridge`/`libatspi`/`libcairo`/`libpango`，但这是 Chromium 在 Linux 上的固有依赖（无障碍/字体），不代表用 GTK 画界面。【实测】
- **结论**：微信 Linux 版 **不是 GTK 应用**，主 UI 是 Qt 5.15.14（静态）+ QWidget，内嵌浏览器是 Chromium。任何"微信依赖 GTK 所以 HBOS 要移植 GTK"的说法都不成立。

### 1.4 运行后端（关键，存在两种证据冲突）

【实测】`WeChatAppEx` 内含完整 Ozone/Wayland 后端代码路径字符串：`../../ui/ozone/platform/wayland/host/wayland_zwp_linux_dmabuf.cc`、`Failed to map zwp_linux_dmabuf_feedback_v1 format table`、`Failed to bind zxdg_decoration_manager_v1`、`wp_fractional_scale_manager_v1`。这证明**代码里有原生 Wayland 路径**。

但【报道】另外两条说法与之张力明显：

- `tsight.io`：在 Wayland 环境下微信"通过 XWayland 运行"。
- GitHub `duanluan/shell-scripts` 的 `activate-wechat.sh` 注释："无论是 X11 还是 Wayland，微信通常通过 XWayland 运行"，并给出 `export QT_QPA_PLATFORM=xcb` 来禁用原生 Wayland "以避免闪烁"。

【推断】三者可以共存：**代码支持 Wayland，但默认/推荐运行在 XWayland 下**，或者原生路径未完全稳定（闪烁、输入法挂接）。对 HBOS 的含义是——**HBOS 没有 XWayland（`src/` 内无 X11 服务端实现），所以只能走原生 Wayland 路径，而这条路对方自己都不建议用户走**。这是本文最重要的一条风险判断。

---

## 2. 逐项对标表

图例：`无差距` = HBOS 已满足或对方无此需求；`可忽略` = 有缺口但对 HBOS 当前阶段无影响；`值得抄` = 对方做法可直接借鉴且能降低 HBOS 工作量；`必须补` = 不补则目标负载跑不起来。

| # | 能力维度 | 对方做法【实测】 | HBOS 现状（文件:行号） | 差距等级 | 对 A 战略的价值 |
|---|---|---|---|---|---|
| 1 | UI 工具包依赖 | Qt 5.15.14 静态链接，无 `libQt*.so` 动态依赖；仅需 glib/gio/gobject/fontconfig/dbus | 无 Qt 支持（不需要），`elf.c` 只解析 PT_INTERP 与重定位 | 无差距 | Qt 静态链接意味着 HBOS 只需支持 glib 家族与 fontconfig，不需移植 Qt——**A 战略的兼容面比预期窄** |
| 2 | 浏览器运行时 | 内嵌 Chromium 144 + Electron（`WeChatAppEx` 243 MB）多进程 | Chromium 测试已在跑，但卡在 `CHECK(!channel_)`（`docs/jindu.md` R54 后 3 条阻塞） | 必须补（进度项） | 与现有 Chromium 验收目标**同一个坑**，做一次通吃 |
| 3 | Wayland 全局：compositor | 必需 | `src/gui/wayland/core/wl_server.c:654` | 无差距 | — |
| 4 | Wayland 全局：wl_shm | 必需 | `wl_server.c:655`，格式仅 ARGB8888/XRGB8888（`iface/wl_iface_display.c:116-121`） | 可忽略 | Chromium/Qt 都用 ARGB8888，够用；但若对方请求 `wl_shm.format` 之外的 XRGB2101010 会退化 |
| 5 | Wayland 全局：wl_seat | 必需，POINTER+KEYBOARD | `wl_server.c:656`，`wl_iface_display.c:124-126` 发 CAPABILITIES，`:127-129` 发 NAME "seat0" | 无差距（能力声明面） | — |
| 6 | **wl_keyboard.keymap 事件** | 必需（Qt 与 Chromium 均依赖 xkb keymap 建映射） | **完全缺失**：`grep -rn keymap src/gui/` 零命中；`wl_protocol.h` 无 `WL_KEYBOARD_E_KEYMAP` | **必须补** | **最高优先级**。没有 keymap，键盘输入整体失效，"窗口出来但打不了字" |
| 7 | wl_keyboard.repeat_info | 长按重复 | 缺失（`wl_backend_hive.c` 只发 key/modifiers） | 可忽略 | 只影响长按，不影响首次可交互 |
| 8 | Wayland 全局：xdg_wm_base | 必需 | `wl_server.c:657`，version 2 | 无差距 | — |
| 9 | Wayland 全局：wl_output | 必需（`Failed to bind wl_output` 在清单内） | `wl_server.c:661`，`wl_iface_display.c:136` 发信息 | 无差距 | — |
| 10 | Wayland 全局：wl_data_device_manager | 必需（剪贴板/拖放） | `wl_server.c:665`（仅注册，未实现内容） | 值得抄 | 微信大量用剪贴板（粘贴图片/文件）。**注册不实现即可让客户端不 fail，但要能复制粘贴需补 `data_source`/`data_offer` 实现** |
| 11 | Wayland 全局：wl_subcompositor | 必需 | `wl_server.c:667` | 无差距 | — |
| 12 | `zwp_text_input_manager_v3` | Qt Wayland 输入法必需（主程序引用 `zwp_text_input_v3`） | 缺失（`grep -rn text_input src/gui/` 零命中） | **必须补**（但可后置） | 中文聊天场景的核心；不过 HBOS 连 keymap 都没有，输入法属第二阶段 |
| 13 | `wp_viewporter` | `WeChatAppEx` 期望全局 | 缺失 | 值得抄 | 高分屏缩放的现代替代方案，Chromium/Qt 都需要 |
| 14 | `zxdg_decoration_manager_v1` | Qt 主程序引用（客户端装饰协商） | 缺失 | 值得抄 | 无它时 Qt 退回客户端自绘标题栏，能用但样式怪；Chromium 同样会 `Failed to bind` |
| 15 | `wp_fractional_scale_manager_v1` | `WeChatAppEx` 期望 | 缺失 | 可忽略 | 只影响非整数缩放清晰度 |
| 16 | `wp_presentation` | `WeChatAppEx` 期望（帧计时反馈） | 缺失 | 可忽略 | 影响帧调度精度，不影响出图 |
| 17 | `zwp_linux_dmabuf_v1` | `WeChatAppEx` 引用源文件，但主程序 Qt **不引用** | 缺失 | 可忽略（当前） | 有 SwiftShader/libEGL 软件路径兜底；等 `/dev/dri` 就位再谈 |
| 18 | XWayland | 微信**默认推荐**跑在 XWayland 下【报道】 | 无 X11 服务端 | 必须补（战略级缺口） | **HBOS 没有 XWayland，等于失去了微信官方推荐的运行路径**，只能靠原生 Wayland，风险显著 |
| 19 | `/dev/dri`（DRM render node） | Chromium GPU 进程与 `libgbm` 依赖 | `src/devfs.c:561-639` 只注册 null/zero/tty/ptmx/pts/console/random/urandom，**无 dri** | 必须补（进度项） | 与 Chromium 验收同一缺口 |
| 20 | `/dev/snd`（音视频） | `WeChatAppEx` NEEDED `libasound`；`src/ac97.c` 存在 | `devfs.c` 无 `/dev/snd` 节点；`ac97.c` 未导出节点 | 必须补（后期） | 语音消息/通话是微信核心功能，但非首屏 |
| 21 | D-Bus | 主程序 NEEDED `libdbus-1.so.3`；`WeChatAppEx` 同 | 仅 `src/selftest.c` 有字符串，**无 D-Bus 实现** | 值得抄 | Qt 用 D-Bus 做单实例、通知、托盘。无 D-Bus 能用但会打一堆错 |
| 22 | fontconfig 缓存 | 主程序 NEEDED `libfontconfig.so.1` | 无 fontconfig 移植，无缓存目录 | 必须补（中文显示） | **不补则中文全变豆腐块**，微信作为中文应用等于不可用 |
| 23 | 共享内存 POSIX shm | Chromium `platform_shared_memory_region_posix.cc` 依赖 `/dev/shm` | `/dev/shm` 内存覆盖层已落地（`docs/jindu.md` R54），`linux_compat.c:2373-2402` memfd slot 后备 | 无差距 | 微信内嵌 Chromium 会复用这条路径，已验证 |
| 24 | 原子操作 | `libatomic.so.1` | 内核无 `__atomic` 需求（用户态库） | 无差距 | — |
| 25 | GLIBC 版本上限 | 主程序最高 `GLIBC_2.26`；`WeChatAppEx` 最高 `GLIBC_2.25` | `elf.c` 支持符号版本解析 | **无差距（利好）** | **两个负载的 glibc 版本要求都低于 Debian 12 的 2.36**，HBOS 的 glibc 兼容层压力小于预期 |
| 26 | 多进程模型 | 主进程 + GPU + renderer 多进程 | R36 结论"单进程路线走不通，必须多进程"（`docs/jindu.md`） | 必须补（进度项） | 微信内嵌 Chromium 强制多进程，HBOS 的多进程能力是硬门槛 |
| 27 | 进程间 mojo IPC | `libmmmojo.so` + Chromium mojo | 无 mojo，依赖 AF_UNIX（`linux_compat.c:986/1025`）+ memfd | 必须补（进度项） | Chromium 已证明 AF_UNIX + memfd 可以承载 mojo，微信同构 |
| 28 | 无障碍 | `libatk-bridge`/`libatspi`；【报道】主窗口遍历 1007 节点 | 无 AT-SPI2 | 可忽略 | 只影响读屏，非 A 战略目标 |
| 29 | 沙箱 | Chromium 多进程沙箱 | 无 namespace/seccomp 完整实现 | 可忽略 | 微信测试可用 `--no-sandbox` 绕过（Chromium 已有此路径） |
| 30 | 桌面集成 | `.desktop` + `%U` URL scheme | 无桌面环境/无 MIME 关联 | 可忽略 | 不影响 A 战略验收 |
| 31 | AI 能力交付形态 | 云端 Agent + 聊天入口（元宝）+ 小程序【报道】 | — | 无差距 | **微信 AI 不产生新的桌面客户端需求**，HBOS 不应为此调整路线 |

### 2.1 补充观察：为什么"微信比 Chromium 更容易先跑起来"

【实测】微信主程序 `NEEDED` 只有 24 条，最高 glibc 2.26，不需要 GLIBCXX/CXXABI（`libstdc++` 静态链接），不需要 `libX11`（Qt QPA 静态内建 xcb 与 wayland 两套）。相比之下 `WeChatAppEx` 的 `NEEDED` 含 `libX11/libXcomposite/libXdamage/libXext/libXfixes/libXrandr/libgbm/libxcb/libxkbcommon/libcairo/libpango/libatk/libcups/libasound/libnss3/libnspr4/libexpat/libudev` 等近 25 条。

【推断】如果目标只是"在 HIVE 里看到微信主窗口并能打字"，**裁剪路径是：先不启动 `WeChatAppEx`**（小程序/内置浏览器入口），只跑主框架。这条路径的依赖面比 Chromium 验收小一个量级，是 HBOS 在 Chromium 卡住时**可以立刻转向的备选验收目标**。

---

## 3. 可执行建议

每条标注「现在做」或「将来做」，代价为人工日估算（1 人日 ≈ 6 小时有效工作）。

### S1【现在做】补齐 `wl_keyboard.keymap` 事件 —— 代价 0.5 人日

- **做什么**：在 `wl_iface_seat.c:22` 的 `WL_SEAT_GET_KEYBOARD` 分支里，拿到 `new_id` 后立即向该 wl_keyboard 对象发送 `keymap(format, fd, size)`，format 用 `WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1`（1），fd 指向一段以 `\0` 结尾的 xkb 文本（可直接内嵌一份最小 xkb 描述），随后发 `repeat_info(25, 400)`。
- **为什么**：Qt 的 `QWaylandInputDevice` 与 Chromium 的 `WaylandKeyboard` 都阻塞在 keymap 上；没有它键码无法翻译成 keysym。【实测】当前 `grep -rn keymap src/gui/` 零命中。
- **代价**：0.5 人日（发送 SCM_RIGHTS fd + 内嵌 xkb 文本）。
- **风险**：低。需要确认 `wl_server.h:WL_MAX_PENDING_FDS 16` 够用；xkb 文本需保证结尾 `\0` 且 size 含结尾 NUL，否则客户端会拒绝。

### S2【现在做】把 `wl_backend_pointer_*` / `wl_backend_keyboard_*` 接进 HIVE 主循环 —— 代价 1 人日

- **做什么**：`src/gui/wayland/backend/wl_backend_hive.c:227/296/318/338` 已实现指针与键盘事件发送，但 `grep -rn "wl_backend_pointer\|wl_backend_keyboard" src/tools/gui.c src/gui/*.c` **零命中**，即这些函数从未被 HIVE 的事件循环调用，只有 `wl_input_probe` 的自检注入（`wl_backend_hive.c:148-159`）在跑。需要把真实输入设备（PS/2 或 USB HID）事件路由到这些函数。
- **为什么**：当前"输入能用"是假的——只有自检注入的合成事件。真实用户按键不会到达客户端。
- **代价**：1 人日（含键盘扫描码 → evdev keycode 映射表）。
- **风险**：中。`wl_backend_keyboard_key` 注释明写"key 是 evdev 键码"，扫描码转换表容易错一位。

### S3【将来做】补 `wp_viewporter` + `zxdg_decoration_manager_v1` —— 代价 2 人日

- **做什么**：在 `wl_server.c:654-667` 注册区新增两个全局；`wp_viewporter` 只需实现 `get_viewport` 与 `set_source/set_destination`（合成时做区域裁剪+缩放）；`zxdg_decoration_manager_v1` 实现 `get_toplevel_decoration` 并回 `configure(server_side=1)`。
- **为什么**：【实测】`WeChatAppEx` 里有 `Failed to bind zxdg_decoration_manager_v1`、`Failed to bind wp_viewporter` 字符串，Debian Chromium 同样会 bind 这两个。
- **代价**：2 人日。
- **风险**：低。两个都是纯服务端状态机，不涉及新内核能力。注意 `wp_viewporter` 的 source/destination 是双精度浮点参数，协议解析器要支持 f64。

### S4【将来做】`zwp_text_input_manager_v3` —— 代价 3 人日

- **做什么**：注册全局并提供 `get_text_input`，实现 `enable/disable/set_cursor_rectangle/commit` 客户端→服务端消息，以及 `enter/leave/preedit_string/commit_string` 服务端→客户端消息；输入法引擎可先用 HIVE 内建的简易拼音表。
- **为什么**：【实测】微信主程序引用 `zwp_text_input_v3`；【实测】`WeChatAppEx` bind 清单含 `zwp_text_input_manager_v3` 与 `zwp_text_input_manager_v1`。**中文聊天没有输入法等于不可用。**
- **代价**：3 人日（协议 2 + 简易 IME 1）。
- **风险**：中。`v1` 与 `v3` 接口差异大，微信两套运行时会 bind 不同版本，建议先只做 v3、对 v1 直接 return 失败（客户端有回退路径）。

### S5【现在做，视 Chromium 进度】把微信主框架作为第二条验收线 —— 代价 0.5 人日（评估）

- **做什么**：在 `docs/HANDOFF-PROMPT.md` 之外新增一条测试命令：解包 deb 后直接 `chroot` 式调用 `/usr/bin/wechat`，跳过 `WeChatAppEx`（不触发小程序）。观察是否能到登录二维码窗口。
- **为什么**：见 §2.1，微信主框架动态依赖面比 Chromium 小一个量级，是**当前卡点下的低成本旁路**。
- **代价**：0.5 人日（先只做可行性评估，不进主线）。
- **风险**：中。Qt 静态链接意味着崩溃时无符号可查（`stripped`），调试会盲；且【报道】称其默认倾向 XWayland，原生 Wayland 路径可能自身有问题。

### S6【将来做】D-Bus 用户态 stub —— 代价 2 人日

- **做什么**：不实现完整 D-Bus 协议，只提供 `AF_UNIX` socket + 最小 `org.freedesktop.DBus` 握手（`Hello`/`RequestName`/`AddMatch`），让 libdbus 的 `dbus_bus_get` 返回成功。
- **为什么**：【实测】主程序 NEEDED `libdbus-1.so.3`。Qt 的 `QDBusConnection::sessionBus()` 失败会导致单实例检测、通知、托盘一系列功能降级并刷屏错误。
- **代价**：2 人日。
- **风险**：低。这条同样服务于 DSH（DeepSeek Harness，Electron），可复用。

### S7【现在做】明确「不做 XWayland」的决策并写进 ROADMAP —— 代价 0.2 人日

- **做什么**：在 `docs/ROADMAP-2026-10-23.md` 里显式写明：HBOS 不实现 X11 服务端/XWayland，所有 GUI 兼容性押注在原生 Wayland 上。
- **为什么**：【报道】微信在 Wayland 桌面上的官方/社区推荐路径是 XWayland；HBOS 如果没有 XWayland，就**必须**让原生 Wayland 路径达到比腾讯自己测试更可靠的水平。这是需要团队显式知情并接受的风险，不能默认忽略。
- **代价**：0.2 人日（写文档）。
- **风险**：无（仅决策记录）。但决策本身的风险高：若原生 Wayland 路径在微信上确实有闪烁/输入法问题，HBOS 将无法运行微信。

---

## 4. 明确不值得抄的部分

| 项 | 原因 |
|---|---|
| Qt 静态链接策略 | 微信这么做是为了减小动态依赖面和二进制自包含，代价是崩溃无符号。**HBOS 是运行环境不是应用**，无动机 |
| `libmmmojo.so` 等自研 so | 微信私有 IPC/网络库，与 HBOS 无关 |
| SwiftShader 软件 Vulkan | 微信自带兜底。HBOS 该做的是真 `/dev/dri`，而不是支持软件 Vulkan（后者性能不可接受） |
| `ocr_model` / `vlc_plugins` / `XEditor` | 微信业务组件，与 OS 层无关 |
| 小程序运行时（RadiumWMPF）整体 | HBOS 只需支撑它跑起来，不需要理解或复刻它 |
| `.desktop` / MIME 关联 | 需要完整桌面环境，超出 A 战略范围 |
| AT-SPI2 无障碍桥 | HBOS 无读屏栈，投入产出比极低 |

---

## 5. 执行顺序建议

```
S1 keymap (0.5d)  ──┐
S2 输入接线 (1d)   ──┴─→ 键盘可用（Qt 客户端可打字）
                        │
S7 XWayland 决策 (0.2d) ──→ 战略定调（可与上面并行）
                        │
S5 微信主框架验收 (0.5d) ──→ 观察是否出登录窗口（旁路验证）
                        │
S3 viewporter+decoration (2d) ──→ 减少 Chromium/WeChatAppEx 的 bind 失败
                        │
S6 D-Bus stub (2d) ──→ 消除刷屏错误，DSH 复用
                        │
S4 text-input-v3 (3d) ──→ 中文可输入
```

S1+S2+S7 合计 1.7 人日，是**投入产出比最高的一组**：它同时推进 Chromium（Chromium 也缺 keymap）与微信两条线。

---

## 6. 我未能验证的部分

1. **微信原生 Wayland 路径的实际可用性**。我只能证明二进制里有 Ozone/Wayland 代码与字符串，无法证明它在真实合成器上不出问题。相反有【报道】说用户被告知改用 XWayland。这条需要 HBOS 真正跑起来才知道。
2. **`WeChatAppEx` 是否动态链接 `libwayland-client.so.0`**。`objdump -p | grep -i wayland` 返回空（退出码 1），但内部有 `wl_display` 等符号与 ozone wayland 源路径，【推断】为静态链接或自实现。无法进一步确认。
3. **微信主程序在无 D-Bus 环境下的降级行为**。我没有实机运行，无法判断 D-Bus 缺失是"警告继续"还是"启动失败"。
4. **`zwp_text_input_manager_v1` 与 v3 在微信中分别由哪个运行时/哪个场景使用**。只能确认两个字符串都在 `WeChatAppEx` 里。
5. **微信 AI 功能的客户端侧实现细节**（是否会在 Linux 桌面版加入本地推理）。当前公开信息只到"云端 Agent + 聊天入口"层面。

---

## 7. 附录：查询与命令记录

### 7.1 网络查询

| 查询 | 结果 |
|---|---|
| `https://linux.weixin.qq.com/` | 成功，版本 4.1.13，X86/ARM/LoongArch 架构与包格式表 |
| `https://dldir1v6.qq.com/weixin/Universal/Linux/WeChatLinux_x86_64.deb` | 成功，231,861,976 B |
| WebSearch `linux 微信 WeChatAppEx wayland "Failed to bind"` | 5 条，含 tsight.io、duanluan/shell-scripts |
| WebSearch `微信 AI 助手 元宝 2026 微信内 Agent 功能 官方` | 5 条，含光明网（马化腾/刘炽平 2026-03 表态）、界面新闻、新华网、央广网、百度百科 |

### 7.2 本地命令（全部在解包目录 `build/tmp/diff2611/x/` 执行）

```bash
dpkg-deb -I wechat.deb                      # 元信息、Depends
dpkg-deb -x wechat.deb x/                   # 解包，754 MB
readelf -d usr/bin/wechat                   # NEEDED 24 条
readelf -V usr/bin/wechat                   # GLIBC 最高 2.26
objdump -p usr/bin/wechat | grep NEEDED
objdump -p opt/wechat/RadiumWMPF/runtime/WeChatAppEx | grep NEEDED
strings -a usr/bin/wechat | grep -c QApplication     # 22
strings -a usr/bin/wechat | grep -c qt.qpa           # 22
strings -a usr/bin/wechat | grep -c QWaylandWindow   # 存在
strings -a opt/wechat/RadiumWMPF/runtime/WeChatAppEx | grep "Chrome/"   # Chrome/144.0.7559.236
strings -a opt/wechat/RadiumWMPF/runtime/WeChatAppEx | grep "Failed to bind"
du -sh opt/wechat/*                          # 目录体积
```

### 7.3 HBOS 侧核实命令

```bash
grep -n "wl_register_global" src/gui/wayland/core/wl_server.c       # 7 处
grep -rn "keymap" src/gui/                                          # 零命中
grep -rn "text_input" src/gui/                                      # 零命中
grep -n "case WL_SEAT_GET_KEYBOARD" src/gui/wayland/iface/wl_iface_seat.c  # :22
grep -n "wl_backend_keyboard_key" src/gui/wayland/backend/wl_backend_hive.c  # :338
grep -rn "wl_backend_pointer\|wl_backend_keyboard" src/tools/gui.c src/gui/*.c  # 零命中
grep -n '"/dev/' src/devfs.c                                        # 8 个节点，无 dri/snd
grep -c "case " src/syscall.c                                       # 374
grep -rln "dbus" src/                                               # 仅 selftest.c
```
