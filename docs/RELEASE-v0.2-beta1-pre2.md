# HBOS v0.2-beta1-pre2 发布说明

> 本次为 **pre-release**：内核与 Linux 兼容层的稳定性修复 + KDE 桌面链路打通。
> 桌面「完整可用」尚不成立，请先读文末《已知限制》。

---

## 亮点

HBOS 现在可以启动 **KDE Plasma 6** 桌面环境，并在同一屏上同时运行
**Konsole** 与 **Chromium**，由 KWin 6.3.6 合成、经自研 Wayland 服务端
`hbos_wl_fb` 输出到 `/dev/fb0`。全程无 GPU（QPainter 软件渲染）。

启动方式：

```bash
make run-kde          # 默认 6144M 内存、gtk 窗口（可鼠标键盘操作）
```

开机后可选择：

```
请选择启动方式 / Select boot mode
    [Enter / T] 命令行 Shell        [K] KDE Plasma 6 桌面
    （进入 Shell 后输入 kde 可随时进入桌面）
```

新增 `kde` 命令：任何时刻在 Shell 里输入 `kde` 即可进入桌面。

---

## 修复

### Linux 兼容层 / 内核

| 修复 | 影响 |
|---|---|
| 实现 `pselect6`(270) | 交互式 shell 无法读键盘输入（`unimplemented nr=270`） |
| `dup2`/`dup`/`F_DUPFD` 的 `FD_CLOEXEC` 语义 | **Konsole 终端空白**的直接原因：KPty 先设 CLOEXEC 再 dup2 到 0/1/2，exec 时被关掉 |
| pty master 的 `poll` 不再「永远就绪」 | 每次 read 都阻塞、事件循环空转 |
| pty master 的 `TIOCGPGRP` 返回成功 | Konsole 反复报 `Failed to get foreground process group id` |
| `vfs_read/write` 对 tty 节点绕开全局 VFS 锁 | 终端一 read 就阻塞，整个 GUI 停摆 |
| 只读文件映射改为共享页缓存 + 页缓存回收 | 4GB 下连 1.8MB 的 `libxml2` 都 ENOMEM、plasmashell SIGSEGV |
| 共享页表元数据改用专用页级 slab | 原先会吃光 2MB 的内核固定堆 |
| 开机静态配置 IPv4（`10.0.2.15/24`, gw/dns `10.0.2.2/3`） | 之前开机从不配置地址，guest 永远没有 IP |
| e1000 置 `RAH0` 的 AV 位 | 缺它时单播帧被硬件丢弃 |

### 图形 / 输入

- `hbos_wl_fb`：帧提交增加「无 attach 的重放」路径。
  Wayland 客户端首帧 `attach+commit`、之后只 `commit`，原实现会让 KWin
  画完第一帧后永久等待 frame callback。

### 构建与工具

- `scripts/mk_kde_image.sh`：幂等两段式打包（独立分区影像 → `e2fsck -fn` → 嵌入 MBR）。
- `scripts/kde_monitor.py`：网页监控面板（左实时画面 / 右 serial 日志），
  递归发现 QEMU monitor socket 与日志，任何验证脚本的产物都能自动显示。
- `scripts/kde_accept.sh` / `kde_bili_demo.sh` / `kde_input_test.py` / `kde_browse.py`：
  自动验收、浏览器演示、按键响应测试、地址栏输入。
- `Makefile`：`run-kde` 支持 `KDE_MEM` / `KDE_DISPLAY`，serial 同时输出到终端与文件；
  新增 `kde-accept` 目标。

---

## 已验证（有截图 + serial 互证）

- KDE 底部面板（Kickoff、应用图标、系统托盘、时钟）可正常显示。
- Konsole 与 Chromium 两个窗口与面板**同屏共存**：
  `docs/evidence/THREE-ON-SCREEN-panel-konsole-chromium.png`
- Chromium 能完整渲染页面（标签栏/地址栏/正文）。
- 4GB → 6GB 内存后，`[MMAP] fail` / `SIGSEGV` / `Did not find a valid screen` 归零。

---

## 已知限制（请勿当作已支持）

1. **壁纸为纯色**（`org.kde.color`, `#1d99f3`）。真实 Breeze Next 图片壁纸尚未打通：
   `org.kde.image` 的 `Image` 配置没有落进 containment 的壁纸分组。
2. **浏览器上网不可用**。e1000 的 TX 正常但 **RX 恒为 0**（`tx=3 rx=0`），
   ARP 应答收不到 → 网关/外网全断，页面报 `ERR_CONNECTION_REFUSED`。
   另外 HBOS 的 Linux socket 层**没有 UDP**，浏览器 DNS 查询无法工作。
3. **Konsole 终端内的可用性未经实机确认**：窗口能显示、bash 能运行，
   但标准输入输出是否已接到 `/dev/pts/N` 仍缺证据。
4. **鼠标键盘未经真人实测**。通路各段均在代码中确认
   （PS/2 驱动 → `/dev/input/event0/1` → `hbos_wl_fb` → `wl_keyboard`/`wl_pointer`），
   但尚未有人实际在窗口里操作验证。
5. **单核**：`-smp 2` 会让内核 `All tasks terminated` 停机。
6. **启动很慢**：单核 + 软件渲染，面板首次出现需约 400~600 秒；
   QML 缓存位于 `/var/cache/hbos`，重打镜像会清空，下一轮会重新变慢。
7. **必须 e1000**：`virtio-net-pci` HBOS 识别不到，网络会直接不可用。

---

## 使用方法

```bash
make run-kde                       # 6144M + gtk 窗口
make run-kde KDE_MEM=6144M         # 显式指定内存
make run-kde KDE_DISPLAY=none      # 无窗口，仅供自动化验证
make kde-accept                    # 自动验收（含 flock、按时间采样、判定面板/桌面）
```

监控面板：

```bash
python3 scripts/kde_monitor.py --port 8770
# 浏览器打开 http://127.0.0.1:8770/
```

---

## 升级注意

- 镜像与 ISO 必须配套使用；`build/hbos-kde-chromium.img` 由
  `scripts/mk_kde_image.sh` 从 `build/tmp/kwin_root` 生成，**不要手工改镜像**。
- 完整排查记录（含每一步的证伪结论与负结果）见 `docs/HANDOFF-to-gemini.md`。
- 交付与注意事项详见 `docs/kde-chromium.md`。
