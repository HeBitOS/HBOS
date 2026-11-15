# 给 Gemini 的提示词（HBOS KDE Plasma 6 + Chromium）

把下面整段发给 Gemini。它包含当前真实状态、已确认的 bug、以及不要重复踩的坑。

---

## 任务背景

仓库 `/media/data/hbosv2`（分支 `Experimental`）。HBOS 是自研 x86-64 操作系统，
实现了一套 Linux 兼容 ABI，能直接跑 Debian trixie 的用户态程序。目标是让
**KDE Plasma 6 + Konsole + Chromium** 在它上面真实跑起来并显示在屏幕上。

启动链：QEMU 用 `-cdrom build/hbos-bios.iso -boot d` 启动，同时挂
`build/hbos-kde-chromium.img`（2G MBR+ext2）作 AHCI 磁盘。guest 内
`/etc/hbos-autostart` 触发 `/usr/bin/start_kde_chromium.sh`，它起 dbus，
再 `hbos_wl_fb wayland-fb kwin_wayland ... --exit-with-session "bash /usr/bin/kde_session.sh"`。
`hbos_wl_fb` 是自研 Wayland 服务端，把 KWin 的合成结果桥接到 `/dev/fb0`。

guest rootfs 解包在 `build/tmp/kwin_root`（普通目录，1.5G，约 39k 文件）。
改完用 `bash scripts/mk_kde_image.sh` 重打镜像（19 秒，含 e2fsck 校验）。

## 已经打通的部分（不要再动）

1. **Chromium 真实渲染**。截图证据：`docs/evidence/chromium-rendered-hbos.png`
   （标签栏、地址栏、页面正文全部正确绘制）。
2. **帧提交路径的核心 bug 已修**。`tools/hbos_wl_fb.c` 的 `surf_commit()` 原来
   只在客户端「attach 了新 buffer」时才拷贝上屏并回 `frame` callback。但 Wayland
   客户端的常规节奏是：第一帧 attach+commit，之后**只 commit 不 attach**（复用
   同一 buffer）来请求下一帧。于是 KWin 画完第一帧后永远等不到 callback，
   整个桌面停在第一帧。修法：在 `struct fb_surface` 里存一份上一帧的**像素副本**
   （`uint8_t *last_px`，注意是副本不是 `wl_shm_buffer*`，因为 buffer release
   之后指针可能被复用），无 attach 的 commit 时把副本重画一遍并回 callback。
   修复后帧数从 1 涨到 6+，整屏从纯黑变成有内容。
3. **`kde_session.sh` 的死锁已解除**。原第 29 行
   `dbus-monitor ... | grep -m1 ...` 是致命的：镜像里根本没有 `grep`
   （HBOS rootfs 缺 grep/sed/awk/find/xargs），grep 以 127 退出后
   `dbus-monitor` 拿 EPIPE 却不退出，bash 永远 wait 在这个管道上，
   后面的 konsole/chromium 永不执行。已整段删除，改成错峰启动。
4. **`grep` 和 `sed` 已补进 rootfs**（从宿主 `cp`，依赖 `libpcre2-8` /
   `libattr` 在镜像里本就有）。

## 现在卡住的三个问题（按优先级）

### P0：壁纸和任务栏（Plasma 面板）不显示

`plasmashell` 进程**起来了**（`[EXEC] pid=32 ... path=/usr/bin/plasmashell`），
但屏幕上既没有壁纸也没有底部任务栏。Chromium 窗口却能正常显示。

已知事实：
- 壁纸素材齐全：`build/tmp/kwin_root/usr/share/wallpapers/Next/contents/images/`
  下有 20 个分辨率的 PNG，**包括 `1920x1080.png`**（屏幕正是 1920x1080）。
- Qt imageformats 插件有 `libqjpeg/libqsvg/libqgif/libqico`（PNG 是 Qt6Gui 内建）。
- `etc/xdg/` 下**没有任何 Plasma 默认配置**，只有 `plasmanotifyrc`、`taskmanagerrulesrc`、
  一些 `autostart/*.desktop`、`menus/plasma-applications.menu`。
  **没有 `plasmashellrc`、没有 `plasma-org.kde.plasma.desktop-appletsrc`**。
- `desktopcontainment` 的 QML **确实被执行了**，日志里有它的真实报错：
  `main.qml:178:25: QML FolderViewDropArea: Binding loop detected for property "minimumWidth"`
- 日志里有大量 `[OPEN-FAIL] path=.../*.qmlc errno=2`，那是编译缓存找不到、回退到
  `.qml` 源码，通常不是根因。
- **历史对照很关键**：早前某轮 konsole/chromium 都没启动时，任务栏**是出现过**的
  （`build/tmp/kde_final_test.log`，frame count=5，底部有完整面板）。
  现在 konsole+chromium 都起来后，面板反而没了。

怀疑方向（请用实测验证，别猜）：
- plasma 没有默认 appletsrc，桌面 containment 可能根本没有被创建；
  `$HOME=/tmp/home` 是每轮开机的临时目录，配置不会跨轮保留，
  所以需要把默认配置作为种子放进 rootfs 并在会话启动时复制过去。
- 也可能是 plasmashell 的两个独立窗口（桌面壁纸 surface + 面板 surface）
  与 Chromium 抢 CPU/焦点，在 `-smp 1` 下 plasmashell 画不出来。
  **注意 `-smp 2` 会让 HBOS 内核直接 `All tasks terminated, halting`，多核不可用，
  必须 `-smp 1`。**

### P1：Konsole 窗口不出现

`[EXEC] pid=43 t=3290 path=/usr/bin/konsole` 出现了，但画面上没有 Konsole 窗口。
需要区分：是它没映射窗口（xdg_toplevel 没 commit），还是被 Chromium 完全遮挡。
Chromium 用的是 `--window-size=1000,700`，Konsole 用 `--geometry 72x20+16+16`。

### P2：可交互性未验证

QEMU monitor `sendkey` 注入按键后截图前后 sha256 完全相同（changed_pct=0.0）。
这在「没有窗口能接受焦点」时是预期的。Chromium 可见之后应该重测一次。

## 绝对不要踩的坑

1. **guest 内不要用 `/usr/bin/sleep`**。HBOS 上 `clock_nanosleep` 会挂起整个会话。
   等待要用忙等，且时钟源**不能**用 `read < /proc/uptime`
   —— HBOS 的 `/proc` 节点不支持 lseek，bash 的 `read` 会先 seek，
   实测报 `[FDERR] nr=8 ret=-29 (ESPIPE)` 并把整条命令弄失败。
   要用 `/bin/cat /proc/uptime | { read ...; }`，且必须容忍空值。
2. **guest 内 bash 不要重定向到 `/dev/null`**（返回 EIO），改写 `/tmp/xxx`。
3. **guest 内 bash 里 `echo ... >&2` 会失败**（`2: Bad file descriptor`），
   打点信息只能写文件，再由会话末尾转发。
4. **HBOS rootfs 缺 grep/sed/awk/find/xargs/sh**。任何 guest 脚本里用到它们都会
   静默失败或返回 127。
5. **镜像打包不要把中间文件放 `/tmp`**。`/tmp` 是 7.7G tmpfs，被塞满后
   连宿主工具链都会 `EDQUOT` 全挂。中间文件一律放 `/media/data`
   （`scripts/mk_kde_image.sh` 已用 `mktemp -d "$(dirname "$OUT")/..."`）。
6. **同一时刻只能一台 VM**：宿主约 4G 可用内存，一台 VM 就是 `-m 4096M`。
   跑 VM 必须 `flock /tmp/hbos-vm.lock`。
7. **不要让一台 VM 在镜像被重打包时运行**，会拿到中间态镜像，结果不可归因。

## 可用的工具

- `bash scripts/kde_verify.sh --smp 1 --timeout 330 --shots 120,180,240,300`
  带 flock、时间线截图、按键测试、镜像漂移检测。退出码是契约
  （0 全通过 / 10 桌面在但缺窗口 / 20 kwin 在但 plasmashell 没起 / 30 启动失败 / 40 QEMU 起不来 / 75 抢不到锁）。
- `bash scripts/mk_kde_image.sh`：从 rootfs 重打镜像（幂等，带 e2fsck）。
- `debugfs -R "cat /path" "build/hbos-kde-chromium.img?offset=1048576"`：直接在宿主读镜像内文件。
- 单核下 plasmashell 从 exec 到画出第一笔界面要 **约 190 秒**，超时要给足。

## 请优先做的事

按 P0 → P1 → P2。每做一步都给**截图路径 + 具体数值**作为证据，
不要用「应该可以」「理论上」下结论。如果某个假设被实测证伪，
把证伪的证据也写出来——负结果同样有价值。

---

# 附录 A：本轮新增发现（12:25 更新，直接追加给你）

## A1. `-smp 2` 已被证伪，不要再试
实测 `--smp 2` 时 guest 在 serial 5117 字节处直接
`[KERN] All tasks terminated, halting.` —— HBOS 内核多核调度不可用。
必须 `-smp 1`，不要把它当作提速手段。

## A2. locale 数据完全缺失（重要，可能影响 plasma QML）
rootfs 里**没有 `/usr/lib/locale/` 目录**。日志里反复出现
`setlocale: LC_ALL: cannot change locale (C.UTF-8)`，
以及 kwin 的 `Qt depends on a UTF-8 locale, but has failed to switch to one.`
已从宿主 `cp -a /usr/lib/locale/C.utf8` 到镜像，并额外复制一份命名为
`C.UTF-8`（两个名字都放，避免 setlocale 按名查找失败）。共 752K。

## A3. 补了 Plasma 配置种子（P0 的主要修复尝试）
确认 rootfs 的 `etc/xdg/` 里**没有任何 plasmashellrc / appletsrc / kwinrc**，
且 `$HOME=/tmp/home` 是每轮开机的临时目录，配置不跨轮保留。
因此新增 `usr/share/hbos-kde/wallpaper-seed.sh`，由 `kde_session.sh`
第 62 行 source（**在 plasmashell 启动之前**），写入：
- `plasma-org.kde.plasma.desktop-appletsrc`
  - Containment 1：`plugin=org.kde.plasma.folder`，
    `wallpaperplugin=org.kde.image`，
    `Image=file:///usr/share/wallpapers/Next/contents/images/1920x1080.png`，
    `FillMode=2`
  - Containment 2：`plugin=org.kde.panel`（底部，`location=4`），
    AppletOrder=3;4;5;6 → kickoff / taskmanager / systemtray / digitalclock
- `plasmashellrc`（关掉启动更新检查）
- `kwinrc`（`Backend=QPainter`，`LatencyPolicy=HighLatency`）

**这份配置尚未验证过。** 如果 plasma 6 的 appletsrc 键名/结构与实际不符，
plasmashell 会回退到默认行为（仍然没壁纸），不会崩。请在实测后按真实格式修正。

## A4. 诊断技巧：怎么确认 seed 真的被执行
会话里 mark 只写文件（`/tmp/session-timing.log`），因为 guest bash 的
`echo >&2` 会 `2: Bad file descriptor`。要看 seed 是否执行，
可在宿主用 debugfs 读镜像，或让 harness 在会话末尾把
`/tmp/session-timing.log` 与 `/tmp/wallpaper-seed.log` 的内容 cat 到 stdout。

---

# 附录 B：bisect 结论（12:52，**请勿重复这些实验**）

## B1. 已用 A/B 实测排除的假设

| 假设 | 实验 | 结果 |
|---|---|---|
| `-smp 2` 能提速 | `--smp 2` | **证伪**。guest 在 serial 5117B 处 `[KERN] All tasks terminated, halting.` |
| Plasma 配置种子能修壁纸 | 加 `wallpaper-seed.sh` | **有害**。有 seed：`plasma=0 konsole=0 FRAME=1`；无 seed：`plasma=1 konsole=1 FRAME=5`。seed 会**阻止 plasmashell 被 exec** |
| `QT_WAYLAND_CLIENT_BUFFER_INTEGRATION=wayland-shm` 有帮助 | 注释掉该 export | 镜像里**根本没有** `libwayland-shm.so`（只有 `libqt-plugin-wayland-egl.so` / `libshm-emulation-server.so` / `libdrm-egl-server.so`）。设它会让 Qt 报 `Failed to load client buffer integration: "wayland-shm"`。已注释 |
| 壁纸缺 C++ 插件库 | 查 rootfs | **证伪**。`libplasma_wallpaper_imageplugin.so`、`plasma_wallpaper.so`、`wallpaper_images.so` 都在，`org.kde.image` 的 QML 也完整 |
| `mark()`/`delay()` 打点有用 | 加 HBOS_MARK 打点 | **有害**。`echo >> file` 与 `/proc/uptime` 读取都会落 `[FDERR] nr=8 ret=-29 (ESPIPE)`，把 bash 弄成 `code=1` 退出，导致 plasmashell 完全不 exec。已整套删除 |

## B2. 当前最好结果（run `20261115-125155`）
```
FRAME=6   plasma=1   konsole=1   chromium=1
```
截图 `build/tmp/kde_verify_runs/20261115-125155/shots/250_shot_250s.png`：
**Chromium 窗口完整渲染**，带 Breeze 标题栏与投影，页面正文全部正确。

## B3. 剩下两个问题（都需要新思路）

### 问题 1：壁纸始终是纯黑
插件齐全、图片齐全、无配置。但**任何形式的配置种子都会让 plasmashell 不被 exec**。
需要查清 plasmashell 为什么对 `$XDG_CONFIG_HOME` 里出现的 appletsrc 如此敏感
（怀疑：读到不完整/格式不符的 appletsrc 后，在 containment 创建阶段死锁或崩溃）。
建议：先在 guest 里让 plasmashell **自己生成**一份 appletsrc（跑一次不带 seed 的会话，
然后用 debugfs 从 `/tmp/home/.config/` 取出真实生成的文件），再以它为基准改壁纸键，
而不是手写配置。

### 问题 2：面板时有时无（竞态）
- run `124518`：有面板（截图确认，底部完整 KDE 面板）
- run `125155`：无面板（只有 Chromium）
两轮脚本几乎相同。说明 plasmashell 的首帧提交存在时序竞态，
或与 konsole/chromium 抢 CPU 有关。需要按帧数/时间点细化观察。

## B4. 关于 `g_main_surf`
`tools/hbos_wl_fb.c` 只把**第一个** ≥320x240 的非 cursor surface 记为 `g_main_surf`，
并只在它上面发 keyboard/pointer enter 事件。如果 KWin 的合成输出 surface
不是第一个创建的，输入焦点会挂错对象。**交互测试失败可能与此有关**，值得排查。

---

# 附录 C：13:40 状态（**新一轮 bisect，含 2 个我自己引入又修掉的回归**）

## C1. 我已引入并修掉的回归（重要，别重复）
1. **`VAR=val cmd` 行首前缀在 HBOS 的 bash 上不被支持**
   写 `LIBGL_ALWAYS_SOFTWARE=1 /usr/bin/plasmashell ...` →
   `cannot execute binary file: Exec format error`，退出码 **126**，plasmashell 完全不启动。
   同理 `export LIBGL_ALWAYS_SOFTWARE=1` / `GALLIUM_DRIVER=llvmpipe` 之后
   plasmashell **同样在 exec 阶段失败**（连续两轮 126）。
   **结论：HBOS 上不要给 plasmashell 设 MESA 变量。** 当前脚本已清空。
2. **全局 `LIBGL_ALWAYS_SOFTWARE=1` 会让 Chromium 渲染回退成白页**
   （只显示 "Loading..."，正文不绘制）。Chromium 靠 `--disable-gpu` 自己走软件路径即可。

## C2. 已确认的硬事实
| 事实 | 证据 |
|---|---|
| Debian trixie 的 Qt6 **只有 `wayland-egl`**，没有 `wayland-shm` | `qt.qpa.wayland: Available client buffer integrations: QList("wayland-egl")` |
| `/bin/cat` 在 HBOS 上 **192 次调用全部返回 127** | 跨 6 轮 serial 统计，成功次数为 0 |
| `wait $PLASMA_PID` 不可靠 | `[EXIT] pid=16 code=0`（session 脚本）后 plasmashell 仍在跑 |
| EGL / Mesa swrast / libgbm 在镜像里齐全 | `libEGL.so.1`、`swrast_dri.so`、`libgbm.so.1` 均存在且 `ldd` 无缺失 |

## C3. 当前稳定结果（run `20261115-133341`）
```
FRAME=6   execfmt=0   Chromium 完整渲染
```
截图 `docs/evidence/chromium-stable.png`。

**仍然没有：壁纸、任务栏、Konsole 窗口。** 这三者需要 KWin 接受更多 surface 并为它们
分配 buffer —— 目前 `HBOS_WL_FB_FRAME` 全部是 1920x1080，说明只有 KWin 的全屏合成层
在提交，客户端窗口内容疑似没有进入合成。

## C4. 下一轮建议方向（我不再猜，需要新证据）
1. **确认 KWin 是否真的把 plasmashell/konsole 当作窗口接进来**。
   需要 KWin 侧日志（`KWIN_LOGGING` / `QT_LOGGING_RULES="kwin_*=true"`）。
2. **确认 `hbos_wl_fb` 收到的 surface 数量**。当前代码只打印 FRAME 尺寸，
   建议临时加 `HBOS_WL_FB_SURF new id=N` 打印，看 KWin 到底创建了几个 surface。
3. 如果 KWin 只提交全屏合成层，那壁纸/面板应该已经**在**那张 1920x1080 里显示。
   但现在里面只有 Chromium —— 说明合成内容本身只有 Chromium 窗口。
   这指向 `<b>plasmashell 的 surface 没有被 KWin 合成</b>`，而不是壁纸插件问题。

---

# 附录 D：13:53 关键定论（**这是目前最接近根因的结论**）

## D1. 三个已修复的真实缺陷
1. **`DBUS_SESSION_BUS_ADDRESS` 没有传进 kde_session.sh**
   `start_kde_chromium.sh` 里 export 了，但 `kde_session.sh` 是被
   `kwin_wayland --exit-with-session` 拉起的独立进程，不保证继承。
   缺它时 plasmashell 报：
   ```
   kde.plasmashell: Did not find a valid screen to place a new panel.
   QDBusError("org.freedesktop.DBus.Error.NoReply", ...)
   ```
   **修复后该报错归零**（`noreply=0`）。

2. **plasmashell 的 MESA 环境变量会让它 exec 失败**（附录 C 已述，退出码 126）。

3. **`hbos_wl_fb` 帧提交缺"无 attach 的重放"路径**（附录取，帧数 1→6）。

## D2. 决定性观测：KWin 只提交 1 个 surface
给 `hbos_wl_fb` 加了按 surface 的独立计数（`HBOS_WL_FB_SURF res=... commits=N WxH`），
实测（run `20261115-134740`）：
```
HBOS_WL_FB_SURF res=0x2000005090 first=1 commits=1 1920x1080
HBOS_WL_FB_SURF res=0x2000005090 first=1 commits=2 1920x1080
HBOS_WL_FB_SURF res=0x2000005090 first=1 commits=3 1920x1080
```
**只有 1 个 surface，全是 1920x1080。** 也就是说 `hbos_wl_fb` 收到的始终是
KWin 的整屏合成输出，**没有任何客户端窗口以独立 surface 形式出现**。

## D3. 由此得出的结论（推翻"壁纸插件问题"的假设）
屏幕上那张 1920x1080 里**只有 Chromium**，没有壁纸、没有面板。
既然这是 KWin 自己的合成结果，说明 **KWin 没有把 plasmashell 的窗口合成进去**。
所以：
- 不是壁纸插件缺失（插件、图片、库全部齐全，已在附录 B 核实）
- 不是 `hbos_wl_fb` 丢帧（它忠实拷贝了 KWin 给的每一帧）
- 不是 D-Bus 断连（已修复且报错归零）

**是 KWin 侧不接受/不合成 plasmashell 与 konsole 的 xdg_toplevel。**

## D4. 下一轮的唯一正确切入口
不要在 rootfs / 环境变量 / 壁纸配置上继续试。需要的是 **KWin 的内部日志**：
在 guest 里以
```
KWIN_LOGGING=1 QT_LOGGING_RULES="kwin_*=true;qt.qpa.wayland*=true"
```
启动 kwin_wayland，观察它对 `xdg_toplevel` 的接受、映射、合成决策。
具体要看：客户端 surface 是否被 `SurfaceInterface` 接管、是否被判为
"not mapped"、是否因缺少 `zwlr_layer_shell`/`xdg_decoration` 而被丢弃。

## D5. 当前稳定产物
- 镜像 `build/hbos-kde-chromium.img` 可复现 Chromium 完整渲染
  （`docs/evidence/chromium-noreply-fixed.png`，sha256 `003c3b30…`，多轮一致）。
- 壁纸 / 面板 / Konsole 窗口 / 键鼠交互 / bilibili 视频 **均未达成**。

---

# 附录 E：14:20 起的进展与一个**指标陷阱**（务必读 E1）

## E1. `changed_pct=0.0` / `responded: False` 是假阴性 —— 交互其实是通的

`kwin-dbg-3` 的按键测试报告写：
```
sent keys: k o n s o l e at t=340s
changed pixels: 0.0%  mean absdiff: 0.0
responded: False
```
但**两张图的 sha256 是不同的**：
```
keys_340s_before.png  63aa75b65f3b5021...
keys_340s_after.png   5fdec0e2f63da4e2...
```
逐像素对比得到的真实差异：
```
pixel diff bbox = (524, 223, 540, 239)     # 只有 16x16 像素
```
该区域像素值（before 全白 → after 出现蓝色）：
```
before: (255,255,255) 全白
after : (20,93,209) (55,117,216) (194,213,244) ...   <- Chromium 的蓝色
```
**结论：按键确实到达了 Chromium，并在页面上产生了可见变化（蓝色选区/光标）。**
`changed_pct` 之所以是 0.0，是因为它按**整屏 1920x1080 = 2,073,600 像素**取比例，
16x16=256 像素只占 **0.0123%**，四舍五入成 `0.0`。

**这是指标精度问题，不是"无响应"。**
判定交互请改用：`pixel diff bbox is not None` + `diff_pixel_count > 0`，
不要用整屏百分比阈值。

## E2. `kwin-dbg-3` 的三个真实改善
| 指标 | 修复前 | dbg-3 | 说明 |
|---|---|---|---|
| `HBOS_WL_FB_FRAME` 打印行数 | 6 | **37** | KWin 合成循环被真正激活 |
| `konsole: Unknown option 'geometry'` | 有 | **0** | `--geometry` 对 Konsole 25.04 无效，已移除 |
| 按键响应 | 无 | **有**（见 E1） | 输入链路打通 |

## E3. 仍未解决：plasmashell 从不创建窗口
`kwin-dbg-3` 里 `HBOS_WL_FB_TOPLEVEL` 只有 **1** 条，且 KWin 日志中
**唯一被 activate 的 `XdgToplevelWindow` 是 Chromium**：
```
kwin_core: activating KWin::XdgToplevelWindow(... caption="Untitled - Chromium")
```
plasmashell 的状态是「授权齐全、连上了 Wayland、但不建窗口」：
```
kwin_core: authorized "/usr/bin/plasmashell" "org_kde_plasma_window_management"
kwin_core: authorized "/usr/bin/plasmashell" "org_kde_plasma_activation_feedback"
kwin_core: authorized "/usr/bin/plasmashell" "org_kde_kwin_keystate"
```
所以壁纸/面板缺失的根因收敛到 **plasmashell 内部**，不是 KWin、不是 hbos_wl_fb、不是协议缺接口。

**新线索**：日志末尾出现
```
[EXEC] pid=89 t=10927 path=/usr/bin/plasma_waitforname
[EXEC-ARGV] [/usr/bin/plasma_waitforname] [org.freedesktop.Notifications]
```
`plasma_waitforname` 会**阻塞等待** D-Bus 名字出现。镜像里没有通知服务，
如果它卡住，会拖住 plasmashell 的启动流程。**下一轮应优先验证它是否挂死**：
- 查它有没有 EXIT 行（当前日志被截断，未见到）
- 若挂死，把 `/usr/bin/plasma_waitforname` 替换成 `true`（之前有人做过 `plasma_waitforname.orig` 备份，可复用）

## E4. 已排除的方向（不要再试）
- `-smp 2` → 内核 `All tasks terminated`
- 手写 Plasma 配置种子 → plasmashell 完全不启动
- `QT_WAYLAND_CLIENT_BUFFER_INTEGRATION=wayland-shm` → Qt6 无此插件（已在两处 unset）
- `LIBGL_ALWAYS_SOFTWARE` 给 plasmashell → exec 失败 126；给 Chromium → 渲染回退白页
- `wl_subcompositor` 缺失 → KWin 日志中无任何抱怨
- 壁纸插件/图片/库缺失 → 全部齐全

## E5. `plasma_waitforname` 已被处理，不是当前阻塞点（修正 E3 的猜测）

E3 里我怀疑 `plasma_waitforname org.freedesktop.Notifications` 会阻塞。
**实测核查后确认：这个问题前一轮已经解决了。**

```
plasma_waitforname       43432 B  mtime 11月15日 09:55   <- 已被替换成 GNU true
plasma_waitforname.orig  35352 B  mtime 2025年7月22日     <- 真程序（含 QDBusServiceWatcher）
```
证据：当前版本 `nm -D` 只有 `error` 一个 T 符号，`strings` 里**没有任何 DBus/Qt 符号**，
是 coreutils 的 `true`。所以它立即返回，不会阻塞 plasmashell。

**结论：不要在这条上浪费时间。** 下一步应查 plasmashell 内部更早的阶段。

## E6. 一个我差点误判的坑：不要在宿主机上 `ldd` guest 二进制

我用 `ldd build/tmp/kwin_root/usr/bin/plasmashell` 得到 8 个 `not found`：
```
libKF6StatusNotifierItem.so.6 / libKWaylandClient.so.6 / libPlasmaQuick.so.6
libkworkspace6.so.6 / libKF6UserFeedbackCore.so.6 / libPlasma.so.6
libKF6Svg.so.6 / libPlasmaActivities.so.6
```
**这是假象。** 宿主机没装 Plasma，当然找不到。
实际核查 guest rootfs，**8 个库全部存在**：
```
lib/x86_64-linux-gnu/libKF6StatusNotifierItem.so.6   OK
lib/x86_64-linux-gnu/libKWaylandClient.so.6          OK
lib/x86_64-linux-gnu/libPlasmaQuick.so.6             OK
lib/x86_64-linux-gnu/libkworkspace6.so.6             OK
lib/x86_64-linux-gnu/libKF6UserFeedbackCore.so.6     OK
lib/x86_64-linux-gnu/libPlasma.so.6                  OK
lib/x86_64-linux-gnu/libKF6Svg.so.6                  OK
lib/x86_64-linux-gnu/libPlasmaActivities.so.6        OK
```
**教训：验证 guest 依赖必须在 guest 里查，或者用 `ldd` 时指定
`--root build/tmp/kwin_root`，不要在宿主机直接跑。**

## E7. plasmashell 的确定症状（这部分是硬证据）
`kwin-dbg-3` 里：
```
行580  [EXEC] pid=21 t=3081 path=/usr/bin/plasmashell
行583  [FORK] pid=26 t=3083                          <- 立刻 fork 子进程
行1468 [EXIT] pid=26 code=0                          <- 子进程退出
行2278 [DEVNULL-REQ] pid=21 ...                      <- plasmashell 才继续
```
并且 **EXEC 之后 plasmashell 产生了 0 条日志**（`grep -cE 'kde\.plasmashell|plasmashell:|QML'` = 0）。
一个正常启动的 Qt 程序至少会输出插件/Qt 信息。

**结论：plasmashell 卡在极早期（main 之前或刚进 main），并且长时间无系统调用。**
下一步应该查它 fork 出的子进程（pid=26）在干什么、以及它在等哪个 D-Bus 名字。
`plasma_waitforname` 已确认被替换成 `true`（见 E5），不是它。

---

# 附录 F：Konsole「窗口出现但终端空白、敲键无反应」的完整诊断（16:10）

## F1. 现象
Konsole 窗口能正常显示（标题栏、New Tab / Split View 工具栏都在），
但内容区**一片空白没有提示符**，键盘输入无反应。

## F2. 已确认的原因链（全部有 serial 证据）

### ① pselect6 从未实现 —— 已修复
```
[LINUX_SYSCALL] unimplemented nr=270 ...
```
x86_64 的 **270 = pselect6**，bash 交互式命令行用它等键盘。
`src/syscall.c` 里只有 271（ppoll）映射，没有 270。
**已实现**：`HBOS_SYS_PSELECT6`，把 fd_set 位图转成 pollfd 走
`linux_compat_poll`（与 ppoll 同一条路径），返回后把就绪位写回。
修复后 `unimplemented nr=270` **归零**，`make test` 全绿。

### ② bash 在 pty 里 exec 失败 —— 未解决（当前唯一阻塞）
```
[EXEC] pid=140 t=23772 path=/usr/bin/bash
  [EXEC] load failed ret=18446744073709551615 path=/usr/bin/bash
[EXIT] pid=140 code=255
```
Konsole 起得来，但它 fork 的子进程在 pty 环境下 exec bash 失败。
对照：`kde_session.sh` 自己的 bash（pid=17）**能正常跑**，
所以不是 `/usr/bin/bash` 这个文件坏了，而是 **forkpty 之后 exec 的路径有问题**。

### ③ 更早的一轮里，bash 曾成功运行但输出进了 serial
```
bash: cannot set terminal process group (138): Bad file descriptor
bash: no job control in this shell
root@hbos:/#            <-- 提示符出现在 serial，不在 Konsole 窗口里
[FDERR] nr=72 fd=0 cmd=3 arg=0 ret=-9     <-- fcntl(0,F_GETFL)=EBADF，fd 0 无效
```
说明当时 pty 未建立（或 fd 0/1/2 没 dup 到 pty slave），bash 继承了
Konsole 的 fd（→ serial），所以"提示符跑到日志里、窗口里空白"。
pselect6 修复后这条消失（`no job control` 归零），但 ② 浮现。

### ④ Konsole 侧仍在报
```
org.kde.konsole: Failed to get foreground process group id for 17: Inappropriate ioctl for device
```
`Inappropriate ioctl for device` = **ENOTTY**，来自 `tcgetpgrp/TIOCPGRP`。
`src/tty.c` 的 `TIOCGPGRP` 在 `!ld->sid || !cur || cur->sid != ld->sid` 时
返回 `-ENOTTY`。注意这里报的是 **pid 17**（会话脚本自己的 bash），
Konsole 似乎把父进程误认为自己的子进程 —— 值得查 fork 后父子/pgid 的记录。

## F3. 已证伪的尝试（不要重复）
| 尝试 | 结果 |
|---|---|
| 给 Konsole 精简环境（unset 全套 QT_/KDE_ 变量，规避 elf loader 对 envp 长度敏感） | **更糟**：bash 连 fork 都不再发生。已回退 |
| `ldd` 判缺库 | 假象：宿主机没装 Plasma。用 `ldd --root build/tmp/kwin_root` 查为 0 缺失 |
| `org/kde/plasma/plasmoid` QML 模块缺失 | 假象：它由 `libPlasmaQuick.so.6` **C++ 注册**（`strings` 可证），不是独立目录 |

## F4. 下一步该查什么
1. HBOS 的 `elf64_load_vfs_and_exec` 在 **forkpty 子进程**里的失败路径
   （`load failed ret=-1` 的具体分支，需要加日志）。
2. fork 之后 `task_exec_inherit` 对 fd 0/1/2 的处理：pty slave 是否被正确 dup。
3. `TIOCGPGRP` 为何在 Konsole 侧拿到 ENOTTY（`ld->sid` 何时被设置）。

---

# 附录 G：面板消失的**决定性 bisect**（16:40，这是目前最硬的结论）

## G1. 三组对照实验（同一镜像、同一内核、`-smp 1`、KVM）

| 会话里启动的组件 | 画面 nonblack | 底部面板 |
|---|---|---|
| **只有 plasmashell** | **3%** | ✅ 完整面板：Kickoff + 应用图标 + Chromium 图标 + 托盘尖角 + 🕐 + "Time zone is not set" + 方框 |
| plasmashell + Chromium | 30% | ❌ 纯黑（底部 50px 采样只有 1 种颜色 `(0,0,0)`） |
| plasmashell + Konsole + Chromium | 30% | ❌ 纯黑 |

证据截图：
- `docs/evidence/panel-only.png` （只有 plasmashell，面板完整）
- `docs/evidence/two-windows-staged.png` （Konsole + Chromium 两窗口共存，但无面板）

**结论：plasmashell 自身完全有能力画出面板和桌面 containment。
只要任何一个客户端窗口存在，面板就消失。**

## G2. 已排除的解释
- 不是 plasmashell 崩溃：pid=21 全程无 `[EXIT]` 记录，一直活着。
- 不是 KWin 停止合成：`HBOS_WL_FB_FRAME` 到 `count=30`，一直在提交帧。
- 不是 hbos_wl_fb 丢 surface：`HBOS_WL_FB_TOPLEVEL` 只有 1 条（KWin 的唯一全屏输出），
  `HBOS_WL_FB_SURF` 全是同一 res、同一 1920x1080。
- 不是 KWin 日志规则没开：`QT_LOGGING_RULES` 已确认生效（能看到
  `kde.plasmashell: evaluating startup script` 与 `activating XdgToplevelWindow`）。

## G3. 我试过但**无效**的办法
- **把 Chromium 推迟到 plasmashell 之后启动**：用纯 bash 轮询
  `/tmp/home/.config/plasma-org.kde.plasma.desktop-appletsrc` 是否存在，
  上限 600 万次自旋。实测 Chromium 仍在 plasmashell 之后仅 **1.6 秒**就启动
  （t=3145 → t=4734），因为 **plasmashell 很早就写出了 appletsrc**，
  所以"等文件出现"不能代表"面板已经画出来"。这条等待策略需要换成
  "等面板窗口真的 mapped"的信号，但 guest 内没有可靠手段观测。

## G4. 下一步最值得试的三个方向
1. **Plasma 的面板自动隐藏**：确认是否有 `panelvisibility=1`（auto-hide）
   被默认写进了 `plasmashellrc`。若有窗口时面板被自动隐藏，
   就会呈现"有窗口没面板、没窗口有面板"的现象 —— 与实测完全吻合。
   试：在 `$HOME/.config/plasmashellrc` 里写 `[General] panelVisibility=0`。
2. **KWin 的 layer-shell 支持**：面板在 Plasma 6 里是 `zwlr_layer_shell_v1` surface。
   查 KWin 日志有无 layer shell 相关告警；`hbos_wl_fb` 只广告 4 个 global
   （compositor/xdg_wm_base/seat/output），**没有 `wl_subcompositor`**，
   KWin 若因此降级处理 overlay/layer，可能丢掉面板。
3. **窗口出现后 KWin 的 scene 重建**：观察 `activating` 之后有没有
   `SurfaceInterface` 被 unmap、或 panel window 被 `hide()` 的日志。

## G5. 顺带确认的其它事实
- `-smp 2` 仍会让内核 `All tasks terminated, halting`，多核不可用。
- Konsole 窗口现在能与 Chromium **同时存在**（各自有标题栏/边框），
  但 Konsole 终端内容仍是空的（bash 在 pty 内 exec 失败，见附录 F）。
- 面板出现的那轮（只有 plasmashell），**壁纸仍然没有**（桌面纯黑），
  说明壁纸与面板是两个独立问题：面板依赖 containment 建出来，
  壁纸依赖 `org.kde.image` 壁纸插件在那个 containment 上真正绘制。

---

# 附录 H：17:00–17:20 的重大修正（内存是主因之一）

## H1. 4GB 下 plasmashell 必然崩溃（已用 6GB 对照证明）
4GB 时的致命链（serial 实证）：
```
[MMAP] fail err=12 fd=21 len=1801672        <- err=12 = ENOMEM
kf.plasma.quick: error when loading applet "org.kde.plasma.kickoff"
  ... libkickerplugin.so: libxml2.so.2: failed to map segment from shared object
[TRAP] user SIGSEGV addr=0x200149EAB8 rip=0x1000032886A6  fault pid=24 (main)
```
**换成 `-m 6144M` 后：`[MMAP] fail` 0 次、`SIGSEGV` 0 次、`Did not find a valid screen` 也归零。**
已把 `KDE_MEM ?= 6144M` 写进 Makefile，`run-kde` 默认用它（`make run-kde KDE_MEM=4096M` 可回退）。

## H2. 预置 appletsrc 会造成 139 次配置失败（已证实）
手写 appletsrc 时日志里：
```
kf.config.core: Created a KConfigGroup on an inaccessible config location
                "plasma-org.kde.plasma.desktop-appletsrc"        <- 139 次
kf.config.core: Couldn't write "" . Disk full?                   <- 文件名为空
```
**不预置配置后，这两类错误从 139/1 降到 2/0。**
结论：让 plasmashell 自己创建配置，不要用手写文件干预。

## H3. 面板其实已经"被创建"了（最新发现，关键）
不预置配置 + 6GB 时，serial 里出现：
```
kde.plasmashell: "Containment 0 Does not have a _sep1 item; it must be a panel."
kde.plasmashell: "Containment 1 Does not have a _sep1 item; it must be a panel."
kde.plasmashell: "Panel 1 Seems to have a middle-click paste action defined; deleting it!"
```
**`Panel 1` 表明 plasmashell 已经建出了面板对象**，布局脚本也执行了：
```
kde.plasmashell: evaluating startup script: ".../org.kde.breeze.desktop/contents/layouts/org.kde.plasma.desktop-layout.js"
```
但屏幕底部 50px 采样仍是**纯黑（1 种颜色）**，即面板窗口没有被 KWin 合成上屏。

## H4. 当前完整状态（截图 `docs/evidence/konsole-chromium-6gb.png`）
- ✅ Chromium 窗口渲染（1000x700，Breeze 标题栏）
- ✅ Konsole 窗口渲染（标题 `- : elf_app — Konsole`，工具栏完整）
- ✅ 两窗口同屏共存
- ❌ 面板：对象已创建，未上屏
- ❌ 壁纸：纯黑
- ❌ Konsole 终端内容：bash 起得来并打印 `root@hbos:/#`，但提示符进的是 serial，
  说明 **fd 0/1/2 没接 pty**（`fcntl(0,F_GETFL)=EBADF`）

## H5. 下一步（按性价比排序）
1. **P0 面板上屏**：查 plasmashell 的 PanelView 用的是什么 surface 角色
   （Plasma 6 用 `zwlr_layer_shell_v1` 的 top layer）。KWin 是 compositor，
   需要 KWin 广告并正确处理 layer_shell。在 hbos_wl_fb 侧看不到，要看 KWin 日志
   （但打开全量 KWin 日志会显著拖慢单核，建议只开 `kwin_core.*=true` 且短暂开启）。
2. **P0 壁纸**：面板上屏后再看桌面 containment 是否绘制 `org.kde.image`。
3. **P1 Konsole 可用**：查 Konsole(KPty) 在 forkpty 后 dup2 到 0/1/2 的路径，
   以及 HBOS `TIOCSCTTY`/`TIOCGPGRP` 的 `ld->sid` 何时被设置。
   注意 bash 已经能跑并打提示符，缺的只是 fd 重定向。

## H6. 交付件状态
- `build/hbos-bios.iso`：含 pselect6（nr=270 归零）
- `build/hbos-kde-chromium.img`：`scripts/mk_kde_image.sh` 幂等重打，含 e2fsck 校验
- `make run-kde`：`-m 6144M`、终端实时滚动 + `tee` 给监控面板
- 监控面板：`python3 scripts/kde_monitor.py` → `http://127.0.0.1:8770/`

---

# 附录 I：面板问题的最终定位 —— **单核 CPU 饥饿**（17:45）

## I1. 四组对照实验（全部 `-smp 1`、KVM、6GB、无配置种子、无详细日志）

| 会话内运行的组件 | 底部面板 | 整屏 nonblack |
|---|---|---|
| 只有 plasmashell | ✅ **52%** | 3% |
| plasmashell + Konsole | ✅ **52%** | 25% |
| plasmashell + Konsole + Chromium (1000x700) | ❌ 0% | 30% |
| plasmashell + Konsole + Chromium (**240x180**) | ❌ 0% | 21% |
| plasmashell + Konsole + **纯 bash 自旋循环 600 万次** | ❌ 0% | 25% |

**关键推理：**
- Chromium 窗口缩到 240×180 后，面板仍然消失 → **不是窗口几何/全屏状态导致的隐藏**。
- 用一个**纯 CPU 自旋循环**（不做任何图形相关的事）替代 Chromium，面板同样消失 →
  **说明杀死面板的是 CPU 占用本身，与 Chromium 无关。**
- Konsole 空闲（等待输入，几乎不占 CPU）时面板正常。

**结论：plasmashell 在单核上被 CPU 密集进程饿死，来不及完成面板绘制。
这是 `-smp 1` 的硬限制。**

## I2. 已排除的因素（都有对照数据）
| 假设 | 结论 |
|---|---|
| 4GB 内存不足 | ✅ 确实存在（ENOMEM mmap libxml2 → SIGSEGV），**换 6GB 后归零**，但不是面板消失的原因 |
| 预置 appletsrc | ✅ 确实有害（139 次 `inaccessible config location`），**移除后降到 2 次**，但也不是面板消失原因 |
| `pselect6` 实现有副作用 | ❌ A/B 对照（临时退回 ENOSYS）面板没有恢复，**已确认无关，保持启用** |
| 详细 KWin/Qt 日志拖慢 | ❌ 关掉日志后面板仍未出现 |
| KWin 缺 `layer_shell`/`plasma_shell` 协议 | ❌ `libkwin.so.6.3.6` 里两者都有 |
| 面板对象没被创建 | ❌ 日志里有 `Panel 1 ... middle-click paste action; deleting it!`，对象确实建了 |

## I3. 当前最可靠的可运行配置
`kde_session.sh`：plasmashell → konsole → chromium，无预置配置、无自旋等待、无 MESA 变量。
- `make run-kde` 使用 `-m 6144M`
- **已知限制**：只要 Chromium 一起跑，面板就会被饿死；只跑 plasmashell+Konsole 时面板正常。

## I4. 真正的解法（用户已决定暂缓）
`-smp 2` 当前会让内核 `All tasks terminated, halting`。
`src/core/task.c` 的注释显示之前已修过"把 main 偷到 AP 导致 BSP 无任务"的问题，
现在仍有另一个调度 bug。修好 SMP 后 plasmashell 能拿到独立 CPU，面板问题预计自然消失。

**备选绕过方案（不需要 SMP）**：`setpriority`(141) 目前是空实现（直接 `return 0`），
而内核已有 `task_set_priority()`（0 最高）。把 141 接上该函数，再加一个极小的
用户态 helper 给 plasmashell 提权，就能在单核上让 plasmashell 抢到时间片。

---

# 附录 J：**三者同屏已达成**（17:50，目标关键里程碑）

## J1. 决定性发现：面板不是"不出现"，而是"很晚才出现"
在 `-smp 1` + Chromium 同时在跑的情况下，**面板约需 540~600 秒才画出来**。

实测采样（同一次运行，每 60 秒取一次）：
```
t≈240s  panel= 0%  screen= 0%
t≈300s  panel= 0%  screen= 0%
t≈360s  panel= 0%  screen=21%
t≈420s  panel= 0%  screen=32%
t≈480s  panel= 0%  screen=32%
t≈540s  panel=41%  screen=36%   <-- 面板开始出现
t≈600s  panel=52%  screen=36%   <-- 面板完整
```

**之前所有"面板消失"的结论，本质都是"观测窗口太短"。**
单核 + Chromium 软件渲染把 plasmashell 的绘制推迟了近 10 分钟。

## J2. 达成状态（截图 `docs/evidence/THREE-ON-SCREEN-panel-konsole-chromium.png`）
- ✅ **KDE 底部面板**：Kickoff + 应用图标 + 2 个 Chromium 图标 + 系统托盘 + 时钟 +
  "Time zone is not set; click here to open Date & Time settings and set one" + 最右方框
- ✅ **Konsole 窗口**（标题 `- : elf_app — Konsole`，New Tab / Split View 工具栏完整）
- ✅ **Chromium 窗口**（页面正文完整渲染）
- ✅ 三者**同屏共存**
- ❌ 壁纸仍是纯黑

## J3. 当前可复现配置（不加任何 hack）
`kde_session.sh`：plasmashell → konsole → chromium，顺序启动；
无预置 appletsrc、无自旋等待、无 MESA 变量、无 `--geometry`。
QEMU：`-m 6144M -smp 1 -enable-kvm -cpu host`。
**验收时必须等够 ~10 分钟**，否则会误判为"面板不存在"。

## J4. 仍需解决
1. **壁纸**：桌面 containment 已创建（`desktopcontainment/main.qml` 有真实 QML 告警说明已加载），
   `wallpaperplugin=org.kde.image` 未生效或图像未绘制。桌面区域仍 `(0,0,0)` 100%。
2. **Konsole 终端内容**：bash 已运行并打印 `root@hbos:/#`，但提示符进的是 serial，
   说明 `fd 0/1/2` 没接到 pty（`fcntl(0,F_GETFL)=EBADF`）。
3. **键鼠交互**：尚未在"三者同屏"状态下验证。

---

# 附录 K：Konsole 终端的精确诊断（18:35）

## K1. 已确认的事实链
1. **Konsole 窗口能上屏**，但内容区空白。
2. 日志里 `org.kde.konsole: Failed to get foreground process group id for 17:
   Inappropriate ioctl for device` —— 这实际说明 **Konsole 已经拿到了 pty master**，
   并对它调用 `tcgetpgrp`，而 `src/tty.c` 的 `TIOCGPGRP` 在
   `!ld->sid || cur->sid != ld->sid` 时返回 `-ENOTTY`。
   即：**pty 有了，但还没有会话（sid）挂上去**。
3. 某些轮次 bash 确实被 exec 并打印了 `root@hbos:#`，但提示符进的是 serial，
   且 `fcntl(0,F_GETFL)=EBADF`（fd 0 无效）→ 说明子进程的 fd 0/1/2 没被 dup 到 pty slave。
4. 多数轮次 Konsole 的子进程**根本没有 exec bash**（`bash EXEC=0`）。

## K2. 已排除
- `setsid` 实现正确（`task_setsid` 逻辑无误）
- `TIOCSCTTY` 已实现，要求会话首进程
- `/dev/ptmx` 由 devfs 注册，内核侧 PTY 测试（`tests/linux_tty.c`）此前 15 项全 PASS
- Konsole 缺 profile：已补 `Shell.profile` + `konsolerc`（**注：这一步曾因为
  在会话脚本里用 `mkdir -p` 而打断父 shell，konsole 直接不启动；已改为把目录
  加进 `start_kde_chromium.sh` 的 mkdir 列表**）

## K3. 一个反复踩的坑（务必记住）
**在 `kde_session.sh` 里调用任何外部命令（`cp`/`mkdir`/`cat`）都可能让父 shell
停止继续执行**，后续的 plasmashell/konsole/chromium 全都不会启动。
现象是 serial 里那条 `[EXEC] ... path=/usr/bin/mkdir` 之后就没有然后了。
只能使用 bash 内建：`echo`、`printf`、`[ ]`、`read`、`:`、`unset`、`export`。
需要建目录/拷文件，一律放到 `start_kde_chromium.sh` 里做（那里执行外部命令是安全的）。

## K4. 下一步
查 Konsole 的子进程为什么没有 exec bash：
- `KProcess`（Konsole 用它 spawn shell）在 HBOS 上的 fork/exec 路径
- 子进程是否卡在 `login_tty()`（setsid → TIOCSCTTY → dup2）
- 建议在 HBOS 的 `execve` 里加一行日志，记录 exec 即将发生时该进程的 fd 0/1/2
  指向什么，就能直接看出 dup2 有没有生效

---

# 附录 L：壁纸根因的**修正结论**（19:40，以实测为准）

## L1. 先撤回我自己的两个错误判断

| 我之前的说法 | 实测结论（wallpaper-investigator 的 A/B 实验） |
|---|---|
| "桌面是黑的，说明 desktop containment 窗口没被合成" | **错**。把 layout 脚本里的 `wallpaperPlugin` 换成 `org.kde.color` 后，桌面立刻整屏亮蓝（#1d99f3）+ 面板 + Konsole 同时可见。**desktop containment 窗口一直被 KWin 正常创建并合成上屏。** |
| "`wayland-shm` 缺失导致窗口/壁纸出不来" | **错**。那几轮 `QT_WAYLAND_CLIENT_BUFFER_INTEGRATION=wayland-shm` 一直设着，面板、蓝色桌面、Konsole 全都正常出图 —— Qt 自动回退到 `wayland-egl`。unset 它只是清理，**不是根因修复**。 |

## L2. 真正的根因（当前最佳解释）
- `org.kde.image` 壁纸插件本身能加载，QML 也加载了；
- 但它对应的 KCFG 分组 `[Containments][N][Wallpaper][org.kde.image][General]` 里
  **`Image` 是空串**；
- look-and-feel 的 `org.kde.breeze.desktop/contents/defaults` 里写的是
  ```
  [Wallpaper]
  Image=Next
  ```
  这个默认值**没有落到 containment 的壁纸配置分组里**；
- 于是 `org.kde.image` 没有任何图像可画 → 桌面黑。
- 而 `org.kde.color` 不需要任何配置就有默认色 `#1d99f3` → 立刻可见。

## L3. 正在验证的修复路线
在 layout 脚本
`/usr/share/plasma/look-and-feel/org.kde.breeze.desktop/contents/layouts/org.kde.plasma.desktop-layout.js`
里显式给桌面 containment 写壁纸配置：

```js
currentConfigGroup = ["Wallpaper", "org.kde.image", "General"];
writeConfig("Image", "file:///usr/share/wallpapers/Next/contents/images/1920x1080.png");
// 并把该 containment 的 wallpaperPlugin 设为 org.kde.image
```

## L4. 教训（对后续所有人）
**"屏幕上没有 X" 有三种完全不同的原因，必须用 A/B 区分，不能从现象直接归因：**
1. 窗口没被合成 → 换成不需要配置的实现（如 `org.kde.color`）看**窗口本身**在不在；
2. 窗口在但内容为空 → 查该插件的配置/资源是否为空；
3. 资源在但没画 → 查具体绘制路径。
我前面两次都直接跳到第 3 步，浪费了大量轮次。
