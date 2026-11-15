# HBOS × KDE Plasma 6 + Konsole + Chromium 交付说明

> 最后更新：2026-11-15 18:50
> 状态：**部分达成**。面板与 Chromium 可同屏；壁纸、Konsole 终端内容、键鼠交互仍未完成。
> 本文只记录**实测过**的结论，未验证的内容会明确标注。

---

## 1. 交付物

| 产物 | 路径 | 说明 |
|---|---|---|
| BIOS 镜像 | `build/hbos-bios.iso` | 含内核（已实现 `pselect6`） |
| KDE 磁盘镜像 | `build/hbos-kde-chromium.img` | 2.0 GiB，MBR + ext2，含完整 Plasma 6 用户态 |
| 镜像打包脚本 | `scripts/mk_kde_image.sh` | 幂等；先建独立分区影像 → `e2fsck -fn` 校验 → 再嵌入 MBR |
| 监控面板 | `scripts/kde_monitor.py` | 网页：左上实时画面，右下 serial 日志 |
| 验证脚本 | `scripts/kde_verify.sh` | 带 flock、时间线截图、按键测试、镜像漂移检测 |

### 一键启动

```bash
make run-kde                 # 默认 -m 6144M
make run-kde KDE_MEM=4096M   # 内存不足时可回退（但 4G 下 plasmashell 会 OOM 崩溃）
```

`make run-kde` 的 serial 输出会**同时**打印到终端（像普通 Linux 那样滚动）并写入
`build/tmp/monitor/serial.log`，供监控面板读取。

### 监控面板

```bash
python3 scripts/kde_monitor.py --port 8770
# 浏览器打开 http://127.0.0.1:8770/
```

---

## 2. 已达成（有截图证据）

| 项目 | 证据 |
|---|---|
| Plasma 6 **底部面板**（Kickoff、应用图标、托盘、时钟） | `docs/evidence/panel-konsole-480s.png` |
| **面板 + Konsole + Chromium 三者同屏** | `docs/evidence/THREE-ON-SCREEN-panel-konsole-chromium.png` |
| **Chromium 真实渲染**（标签栏/地址栏/页面正文） | `docs/evidence/chromium-stable.png` |
| Konsole 窗口上屏（带 Breeze 标题栏与工具栏） | 同上三屏截图 |

### 关键时间参数（单核，务必知道）

| 事件 | 时间 |
|---|---|
| KWin 接受客户端连接 | ~22 s |
| plasmashell 进程启动 | ~30 s |
| **面板首次出现** | **~400–600 s** |
| Chromium 窗口渲染 | ~60 s |

**验收时观测窗口必须 ≥ 500 秒**，否则会误判成"没有面板"。

---

## 3. 未完成

| 项目 | 现状 | 已知阻塞点 |
|---|---|---|
| **桌面壁纸** | 桌面区域 100% 纯黑 | 换成纯色壁纸（`org.kde.color`）也是黑的，说明 **desktop containment 窗口未被合成**，与壁纸插件/图片解码无关 |
| **Konsole 终端内容** | 窗口在，内容空白 | bash 能运行并打印 `root@hbos:/#`，但提示符进了 serial；`fcntl(0,F_GETFL)=EBADF` → 子进程 fd 0/1/2 未接到 pty slave |
| **键鼠交互** | 未验证 | 依赖上一条 |
| **bilibili 视频** | 未验证 | 依赖 Chromium 稳定 + 网络 + 解码链 |

---

## 4. 已修复的真实缺陷（都在代码里）

1. **`hbos_wl_fb` 帧提交缺"无 attach 的重放"路径**（`tools/hbos_wl_fb.c`）
   Wayland 客户端的常规节奏是首帧 `attach+commit`、之后只 `commit`。
   原实现在没有新 buffer 时既不上屏也不回 `frame` callback，KWin 画完第一帧就永久等待。
   修复：缓存上一帧像素副本，无 attach 时重放并回 callback。帧数 1 → 6+。

2. **`pselect6`（syscall 270）从未实现**（`src/syscall.c`）
   bash 交互式命令行用它等键盘。已实现：fd_set → pollfd → `linux_compat_poll` → 写回就绪位。

3. **`DBUS_SESSION_BUS_ADDRESS` 没有传进 `kde_session.sh`**
   `kde_session.sh` 是被 `kwin_wayland --exit-with-session` 拉起的独立进程，不保证继承。
   缺失时 plasmashell 报 `Did not find a valid screen to place a new panel` + D-Bus NoReply。

4. **guest 内存 4GB 不足**（`Makefile` 的 `KDE_MEM ?= 6144M`）
   4GB 时 plasmashell 映射 `libxml2.so.2` 得到 `ENOMEM`，kickoff applet 加载失败，随后 SIGSEGV。
   6GB 下这些错误全部归零。

5. **guest rootfs 缺 `grep`/`sed`/`locale`**，已补入；`/etc/localtime` 也已补（原先每次开机上千次 open 失败）。

6. **QML 缓存放在 tmpfs 导致每次都重新编译**
   `XDG_CACHE_HOME` 改为 `/var/cache/hbos`（磁盘），首次编译的缓存留在镜像里，后续开机复用。

---

## 5. HBOS 用户态的重要限制（写脚本必读）

| 限制 | 说明 |
|---|---|
| **不能用外部命令** | 在 `kde_session.sh` 这类被 KWin 拉起的会话脚本里，调用 `cp`/`mkdir`/`cat` 等**外部命令**会出现"命令执行成功但父 shell 不再继续"的现象，后续服务全都不启动。只能用 bash 内建：`echo`/`printf`/`[ ]`/`read`/`unset`/`export`。需要建目录/拷文件请放到 `start_kde_chromium.sh`。 |
| **不能用 `sleep`** | `clock_nanosleep` 会挂起整个会话。 |
| **不能重定向到 `/dev/null`** | 返回 `EIO`，改写 `/tmp/xxx`。 |
| **`echo >&2` 会失败** | 报 `2: Bad file descriptor`。诊断信息请写文件。 |
| **`/bin/cat` 跑不起来** | 实测数百次调用全部返回 127。 |
| **不支持行首 `VAR=val cmd`** | 会报 `cannot execute binary file: Exec format error`（退出码 126）。用 `export`/`unset`。 |
| **`-smp 2` 会让内核停机** | `All tasks terminated, halting`。当前只能单核。 |
| **不要手写 Plasma 配置** | 手写 `plasma-org.kde.plasma.desktop-appletsrc` 会导致上百次 `inaccessible config location`，且面板消失。让 plasmashell 自己创建。 |

---

## 6. 已知未修的内核/用户态缺口

- `mincore`(27)、`clone3`(435/436)、`waitid` 的部分路径等仍返回 `ENOSYS`（数量已很少）。
- `setpriority`(141) 是空实现（直接返回 0），内核其实已有 `task_set_priority()`，接上后
  可以让 plasmashell 在单核上优先获得时间片——这是**不修 SMP 也能缓解启动慢**的一条路。
- HBOS 的 ext2 可读写（`Filesystem features` 无 `metadata_csum`），因此把缓存/配置放磁盘是可行的。

---

## 7. 复现与排错

```bash
# 看当前镜像里有哪些关键文件
debugfs -R "stat /usr/bin/plasmashell" "build/hbos-kde-chromium.img?offset=1048576"

# 检查 guest 依赖是否齐全（宿主直接 ldd 会误报缺库！）
ldd --root build/tmp/kwin_root build/tmp/kwin_root/usr/bin/plasmashell

# 重打镜像
bash scripts/mk_kde_image.sh
```

详细的排查过程与负结果见 `docs/HANDOFF-to-gemini.md`（约 700 行，含每一轮的实验与证伪）。

---

## 9. 内存修复：只读文件映射改为共享页缓存（19:57）

### 改了什么
`src/syscall.c` 新增 `vm_fill_vfs_shared()`：**只读文件映射**（`.so` 共享库、`.pak` 数据包等）
不再给每个进程各分配一份物理页并读盘，而是按 `(文件节点, 页号)` 复用同一批物理页
（等价于 Linux 的 page cache 语义），并把 VMA 标记为 `VM_BACKING_VFS_SHARED`，
`munmap` 时正确递减引用。

### 为什么这是内存问题的关键
原实现的注释写得很清楚：**只读映射"刻意仍走私有快照"**。而 Chromium 有 6~7 个进程
（browser / zygote / gpu / renderer / utility…），每个都要映射几百 MB 的共享库：
- 同一份 `libQt6Gui.so` / `libicu*` / `libxml2` 被复制 6~7 次；
- 这直接造成 4GB guest 下"连 1.8MB 的 libxml2.so.2 都映射不了（ENOMEM）"。

改为共享后，同一页只存在一份，物理内存随进程数增长的量大幅下降。

### 为什么之前不敢这么做
旧注释担心"20MB 的 resources.pak 要 5000 个共享槽位，会把共享页表挤爆"。
那是**定长槽位数组**时代的问题；现在的共享页表是**散列桶 + 动态链表**，
每个页一个 `kmalloc` 条目，不再有固定槽位上限。

### 仍然待做（真正的"回收"）
共享只解决**重复**，不解决**回收**。HBOS 目前仍然：
- 没有 swap；
- 没有页面回收（`src/core/` 里没有任何 reclaim / page_cache 反向映射逻辑）；
- `pmm_alloc_page()` 在 `g_free_pages == 0` 时直接返回 0，**不做任何尝试**。

要做到能交付给用户，还需要：
1. **可回退的页缓存**：对"干净且未被映射"的文件页建立反向映射，压力下释放、缺页时重新读盘；
2. **swap**（哪怕是 zram 式的内存压缩）；
3. `pmm_alloc_page()` 在失败前调用一次 reclaim，而不是直接返回 0。

这三项是后续工作，不在本次改动范围内 —— 本次只做去重，因为它是当前最大的浪费来源。

---

## 10. 一次我自己引入又修掉的回归（20:19）：共享页表吃光了内核堆

### 现象
4GB 下 KDE 完全起不来，`kwin_wayland` 直接死：
```
[MMAP] fail err=12 fd=3 len=3781592 flags=0x802 size=3774800
/usr/bin/kwin_wayland: error while loading shared libraries:
    libicui18n.so.76: failed to map segment from shared object
```
而**同一份日志**的 meminfo 显示：
```
Memory: 1323924 KB used / 5242880 KB total (25%)
```
**物理内存只用了 25% 却报 ENOMEM** —— 所以不是内存不足。

### 根因
第 9 节引入的共享页表为**每个 (文件,页号)** 分配一条 `shm_page_t`（40 字节），
用的是 `kmalloc()`。而 HBOS 的内核堆是 **2MB 固定 BSS 池**（`HEAP_POOL_SIZE`）。
KWin 加载共享库要映射几万页，`40B × 几万 ≈ 2MB`，**把堆池耗尽**，
之后所有 kmalloc 失败，映射失败被上层当成 ENOMEM 抛出。

### 修复
在 `src/linux_compat.c` 给共享页表条目做**专用页级 slab**：
- `shm_entry_alloc()`：freelist 为空时从 PMM 取一整页，切成
  `PAGE_SIZE / sizeof(shm_page_t)` 个条目挂到 freelist；
- `shm_entry_free()`：回收进 freelist，不归还给 2MB 堆池。
- `linux_compat_shm_reclaim()` 也改用 `shm_entry_free()`。

这样共享页表的元数据随实际用量增长（走 PMM），不再挤占内核那 2MB 固定堆。

### 教训
**"ENOMEM" 不一定真的是物理内存不够。** 排查时必须同时看：
1. `[MMAP] fail` 的具体文件与长度；
2. meminfo 的 used/total；
3. 是否有**固定大小的池**（内核堆 2MB、任务表、fd 表…）被打满。
这次如果只看 mmap 失败就继续加内存，会完全走错方向。

---

## 11. 使用方式与注意事项（提交前必读）

### 启动
```bash
make run-kde        # 默认 -display gtk（会弹出窗口，鼠标键盘可操作）
```
`make run-kde` 现在有两个可调参数：
```bash
make run-kde KDE_MEM=6144M          # 内存（4GB 会 OOM，见第 8/10 节）
make run-kde KDE_DISPLAY=none       # 无窗口，仅供自动化验证
```

### 开机选择
启动后会询问：
```
请选择启动方式 / Select boot mode
    [Enter / T] 命令行 Shell        [K] KDE Plasma 6 桌面
    （进入 Shell 后输入 kde 可随时进入桌面）
```
- **K** → 直接进入 KDE 桌面
- **Enter / T** → 进入 HBOS 命令行
- 进入命令行后，随时输入 **`kde`** 即可启动桌面（等价于 `run /usr/bin/bash -c "/usr/bin/start_kde_chromium.sh"`）

### 注意事项（重要）

1. **一定要给足内存**。`KDE_MEM=4096M` 下 plasmashell 会因 `ENOMEM`/`SIGSEGV` 起不来；
   默认 6144M 是当前可用值。
2. **第一次启动很慢**。单核 + 全软件渲染，KDE 桌面/面板首次出现需要 **数分钟**；
   面板最晚实测约 400~600 秒。不要以为卡死。
   QML 缓存位于 `/var/cache/hbos`（磁盘），**重打镜像会清空它**，下一轮又会变慢。
3. **网卡必须用 e1000**。`make run-kde` 已改为 `-device e1000`；
   用 `virtio-net-pci` 时 HBOS 识别不到网卡（`[KERN] net: configure FAILED`）。
4. **网络目前仍不可用**（详见第 12 节）：能识别网卡、能配上 `10.0.2.15/24`，
   但 e1000 收包不通（`tx=3 rx=0`），所以 ARP/网关/外网全部失败，
   浏览器会报 `ERR_CONNECTION_REFUSED` / `DNS_PROBE_FINISHED_NO_INTERNET`。
5. **不要手写 Plasma 配置**（`plasma-org.kde.plasma.desktop-appletsrc`）：
   会导致上百次 `inaccessible config location`，面板会消失。
6. **不要在 `kde_session.sh` 里调用外部命令**（`cp`/`mkdir`/`cat`）：
   会让父 shell 停止执行，后续组件全都不启动。只能用 bash 内建。
7. **不要用 `sleep`**（HBOS 的 `clock_nanosleep` 会挂起），
   不要重定向到 `/dev/null`（返回 EIO）。
8. **`-smp 2` 会让内核停机**（`All tasks terminated`），只能单核。
9. **壁纸当前是纯色**（`org.kde.color`，`#1d99f3`）。
   真实 Breeze Next 图片壁纸尚未打通：`org.kde.image` 的 `Image` 配置没有落进
   containment 的壁纸分组；`layout.js` 里已留好后续修改点。
10. **Konsole 终端内容**：窗口能显示，bash 也能运行，但标准输入输出的 fd
    是否接到 pty 仍需实机确认（内核 `dup2`/`FD_CLOEXEC` 语义已修）。

---

## 12. 网络现状（仍未打通，勿写入 release notes）

已定位的完整链条：

| 层 | 现状 |
|---|---|
| 网卡 | `virtio-net-pci` **识别不到**；`e1000` 可识别 |
| 地址 | 已在 `kernel.c` 开机调用 `net_configure(10.0.2.15/24, gw 10.0.2.2, dns 10.0.2.3)` |
| 收发 | TX 正常（`tx=3`），**RX 恒为 0**（`rx=0`），ARP 应答收不到 → `arp_resolve` 失败 → ping/网关/外网全断 |
| DNS | HBOS 的 Linux socket 层**没有 UDP**（`socket(AF_INET)` 只走 TCP），glibc 的 DNS 发不出去 |

排查建议（按性价比）：
1. e1000 RX：检查 `RCTL` 的 `BSIZE`/`UPE`、描述符地址是否真是物理地址、
   `RDH/RDT` 的初始顺序；已修 `RAH0` 的 AV 位（原先缺失会让单播帧被硬件丢弃）。
2. socket 层补 UDP：`SOCK_DGRAM` 需要独立的 `udp_pcb`、`sendto/recvfrom` 与就绪判定。
3. 临时绕过 DNS：Chromium 的 `--host-resolver-rules="MAP host ip"`（已在镜像里），
   但底层 TCP 不通时无意义。
