# HBOS × Chromium 推进日志（2026-10-24 起）

目标：A = `--headless --dump-dom` 打出 `<html>` + `--screenshot` 出图并能在 Hive 显示；
B = Hive 里能开 Chromium 窗口看到真实渲染画面（交互为 stretch）。

硬约束：无 KVM，QEMU 只能**单实例串行**跑；每轮 ≈ 改码 + `make bios-iso -j16` + QEMU 4~6 分钟。

---

## 当前状态总览（2026-10-31 19:30 更新）

### 目标达成情况

| 子目标 | 状态 | 证据 |
|---|---|---|
| Chromium 能启动、解析参数、初始化 | ✅ | `--version` → `Chromium 150.0.7871.100`；多进程（pid 2/23/24/27/39/40/43）全部 `[EXIT] code=0` |
| zygote 握手 | ✅ | `Cannot communicate with zygote` 已消失；`/proc/<pid>/exe` 返回真路径，`icudtl.dat` 正常打开 |
| 事件循环（ppoll） | ✅ | `unimplemented nr=271` 从刷屏归零（Chromium 事件循环的主等待点） |
| 沙箱探测（landlock） | ✅ | `nr=444/445/446` 返回 ENOSYS，Chromium 判定"内核不支持"并降级 |
| **Wayland 协议层（我们写的服务端）** | ✅ | 自研客户端全链路 PASS：registry → bind → surface → shm 像素 → configure → buffer release → pointer/keyboard 事件 |
| **registry 字节流对真 libwayland 的可解析性** | ✅ | R32 宿主机复现台：真 libwayland 1.26.0 回调 **7/7** 个 global，字段全对（`make wl-libwayland-test`）|
| **Chromium 连上我们的 Wayland 服务端** | 🟡 **R32 更正** | 早先记录的 `client accepted` + `flush 252 bytes` 属**更早一次运行**；R32 复测**一次 `client accepted` 都没出现** —— 它在连 Wayland **之前**就崩了 |
| Chromium 在 Wayland 上**出画面** | ❌ **未达成** | 阻塞点已前移：不是 libwayland 事件分发，而是 **fork 出的子进程在 libc 内解引用空指针**（`rip=0x1000017F95A5`，与 R11 同一地址） |
| `--dump-dom` 出 `<html>` | ❌ | Debian 这份 chromium **没编 headless shell**，命令处理器不存在（不是移植 bug） |
| Debian / AUR 软件包可安装 | 🟡 | `pkg2hax.py` 已支持 `.deb`/`.pkg.tar.zst`/`PKGBUILD` 三种来源；发行版身份系统已完成（见 R30） |
| 发行版身份标识 | ✅ | `distro` 命令：debian 12 / arch / hbos 三档案，**探测文件可并存**（deb+aur 同时装） |
| CI | ✅ | `host-tests` + `build-bios` + `qemu-tests` 三个 job（**已写好，未 push**） |
| `make test` | ✅ | 全绿（含两个真实失败的修复） |

### 关键数字（`scripts/perf_baseline.py`，TCG 无 KVM，3 次取中位数）

```
boot      = 3.02s     从 QEMU 启动到 "[KERN] Shell ready"
workload  = 9.97s     Wayland surface 测试端到端墙钟（含 QEMU 冷启动，非纯计算耗时）
```

### 现在的瓶颈在哪（Chromium 这一条）

**不在内核，也不在我们的 Wayland 服务端**。已排除：

- 服务端正确：收到 `get_registry`+`sync`，回了 7 个 global + done（252 字节）
- 无任何 `unimplemented` syscall 残留
- 线程正常：`tests/linux_thread.c` 4 线程创建/调度/mutex 800 次全对
- `ppoll` 正常：`nfds=1 to=∞ ret=1` —— 等到了可读、读了 168~252 字节

**是客户端收到 registry 事件后没有继续 bind。** 已排除"缺接口"假设：
补上 `wl_output`、`wl_data_device_manager`、`wl_subcompositor` 后行为完全不变。

> **R32 修正（重要）**：上面"没有继续 bind"这个结论，证据来源有缺陷。
> 服务端的 `rx`/`req` 日志用的是**有限预算**（`wl_rx_log_budget` /
> `wl_dbg_dispatch_budget`），前几条打完就静默。所以"日志里没有 bind"
> **不等于**"客户端没发 bind"——后者从未被真正观测过。
> R32 已加了一条**无条件**的 `[WL] BIND from client obj=` 打印（不受预算限制），
> 下一轮 QEMU 用它来给出确定答案。

---

## 接下来的计划（按优先级）

### P0 —— Chromium 在 Wayland 上出画面

1. ~~拿到 libwayland 的协议日志~~ **R32 已完成**：`WAYLAND_DEBUG=1` 之所以
   "没生效"，根因不在 Chromium 清环境，而是**我们自己的 `build_linux_env()`
   用写死的固定 7 项构造 envp**，用户设的变量根本进不了子进程
   （`src/tools/app.c`）。R32 已修并新增 `export NAME=value` 命令。
   宿主机复现台 `make wl-libwayland-test` 已能拿到完整 `WAYLAND_DEBUG` 日志。
2. ~~核对 `wl_registry.global` 的字节布局~~ **R32 已证伪**：用真实
   `wl_wire.c` 生成 252 字节喂给真 libwayland 1.26.0，**7 个 global 全部
   正确回调**（`tests/host/wl_libwayland_replay.c`）。序列化层无缺陷，
   这个"最可疑的地方"可以划掉。
3. 若 registry 解析正常，则查 Chromium 的 ozone 初始化要求哪些接口
   （`wl_data_device_manager` / `wl_subcompositor` / `xdg_wm_base` /
   `wl_output` 之外可能还有 `wl_surface` 版本、`zwp_linux_dmabuf`）。

### P1 —— Wayland 服务端补完（为"之后的应用"铺路）

4. **窗口化**：surface 现在贴在全屏 `(0,0)`（`wl_surface_origin` 写死 0），
   要接 HIVE 的窗口管理 —— 多窗口不能重叠，要有标题栏、能拖、能关。
5. **真实键鼠接入**：现在是 `--probe-input` 自动注入，要接 HIVE 的
   `mouse_poll` / 键鼠事件源，把 `wl_backend_pointer_*` 挂到真实输入上。
6. **`xdg_decoration` / `wp_viewporter` 等可选接口**：Chromium 完整版会用到。

### P2 —— 发行版身份与包管理

7. **`pkg2hax.py` 写 `distro` 字段**：manifest 加 `distro.id`（从
   `source_format` 推），`hpt install` 据此调 `distro add`。
8. **`.deb` 版本号从仓库 Release 读**：`dpkg-deb -x` 拿不到，需要
   `fetch_deb_closure.py` 那边把 Release 信息带进来。
9. **P2 阶段之后**：`dpkg` / `pacman` 最小数据库骨架（让 `dpkg -l` 不报错）。

### P3 —— 性能

10. **建立热点 profile**：现在有基线脚本了（`scripts/perf_baseline.py`），
    下一步要能定位到具体热点 —— 建议加 `--vmodule` 级别的 syscall 计数
    统计（按 nr 聚合次数 + 累计耗时），而不是现在这种逐条打印。
11. 已知可优化点：
    - `wl_buffer_read` 已加整块快速路径（已做）
    - GUI 重绘：`gui_blit_argb8888` 每行扫一遍 alpha 才决定 memcpy，
      可以用"buffer 是否曾出现透明像素"缓存
    - Chromium 场景下串口输出（`[LINUX_SYSCALL]` 等）会拖慢一切，
      稳定后要能彻底关掉

---

## 轮次记录

### R0（早先，已并入本文件）
- 诊断设施：`[EXIT-BT]` 用户态回溯、`[FDERR]`（fd/路径/errno）、mmap 失败回溯、`scripts/symbolize_rip.py` + chromium dbgsym 符号化。
- 定位静默 `exit(0)` 根因：`HistoryService::Init` → `ContentVisitDelegate::Init`（`VisitedLinkMaster` 要共享内存）失败 → `TerminateCurrentProcessImmediately(0)`。

### R1
改动：
- `src/lib/posix.c` / `src/vfs.[ch]` / `src/fs.c` / `src/ext2.[ch]`：`ftruncate` 支持任意长度（原只认 0），ext2 缩小逐块 free + 清指针，扩大只改 `i_size`。
- `src/syscall.c`：补 `pwrite64`(nr=18)、`fallocate`(285)、`fchown/lchown`(93/94)、`fcntl` 记录锁（`F_GETLK/F_SETLK/F_SETLKW/F_OFD_*`）。
结果：UKM 库不再 `disk I/O error`。

### R2
改动：
- `src/syscall.c`：可写 `MAP_SHARED` 文件映射不再返回 `EOPNOTSUPP`（退化为进程内私有快照，VMA 标 `VM_BACKING_VFS_SHARED`）。
结果：`VisitedLinkMaster::Init` 通过，`HistoryService::Init` 不再自杀。

### R3
改动：
- `src/syscall.c`：`madvise` 容忍未映射空洞（原来整段任一页缺页就 `ENOMEM`）。
结果：PA `DecommitSystemPages` 的 `int3` 消失。

### R4
改动：
- `src/syscall.c`：mmap/mremap 的空闲 VA 判定改为「任何 VMA 覆盖即视为占用」，并按 VMA 跳跃查找（避免扫 32 GiB 预留池的 840 万页）。
结果：PA 池与普通 mmap 不再重叠；多进程的 `FieldTrialList` OOM/double-free 消失。

### R5
改动：
- `src/syscall.c`：`prlimit64` 校验输出缓冲区「已映射且可写」，否则 `EFAULT`。
结果：PA `CheckMemoryReadOnly` 的 `int3` 消失；单进程启动推进到字体/GPU stub 阶段。

### R6
改动：
- `src/signal.h`：`_NSIG` 64 → 65（Linux 语义：合法信号 1..64）。
结果：子进程 `sigaction (set) failed.` 从每进程一次降到偶发；仍有一条 `rt_sigaction(sig=1)` 返回 `-EFAULT`。

---

### R7（sigaction 去掉非法校验）
- `src/syscall.c`：`linux_rt_sigaction` 删除「handler/restorer >= 0x8000… 就 EFAULT」的非 Linux 校验，改为前 8 次打 `[SIGACT]`。
- 结果：`[SIGACT] sig=1 h=0x0 f=0x4000000 r=0x1000017176f0 m=0x0` 全正常，`sigaction (set) failed.` 消失。
- 新崩溃：多进程主线程 `__stack_chk_fail`（`MessagePumpGlib::HandleCheck` 栈金丝雀不符）；另一子线程 `ShutdownDetector::ThreadMain` int3。

### R8（单进程复测）
- 结果：仍 `[EXIT] pid=1 code=0`，无 TRAP、无 History 报错、无 `<html>`。

### R9–R11（关键定性实验）
- 把 Chromium 详细日志写进镜像（`--log-file=/tmp/vlog.txt`），宿主机用
  `debugfs -R "cat /tmp/vlog.txt" "build/c3-chromium.img?offset=1048576"` 读（镜像有 MBR，偏移 1 MiB）——**这条以后常用，能拿全量 VLOG 而不占用串口带宽**。
- 发现：`--v=1` 下 1100+ 行日志里 **没有任何 WebContents / Navigation 记录**，
  `--dump-dom`、`--screenshot=/tmp/a.png` 都没有产物（debugfs 验证文件不存在）。
- 发现：`--headless` 时浏览器完成全部启动（Profile/History/扩展/字体/GPU stub/扩展 renderer）
  后 **2 秒内干净退出**；**去掉 `--headless` 只留 `--ozone-platform=headless` 时浏览器一直存活**
  （r12：`[EXIT] pid=1` 计数 0，跑到会话超时）。即：**`--headless` 在这份 Debian 构建里
  不会走 dump-dom 命令处理器，只会让浏览器"无窗口→退出"**。
- 符号证据：`headless::HeadlessBrowserImpl` 在二进制里 **0 个符号**（只有
  `ChromeBrowserMainExtraPartsHeadless` / `HeadlessCommandHandler` /
  `HeadlessModePolicy`），说明 `--headless` 的新 headless 浏览器主体没编进来；
  `--dump-dom/--screenshot` 的命令处理器存在但没有调用者（那是 chrome-headless-shell 的路径，
  Debian 的 chromium 包不含该二进制）。
- 最简 flag 组合（默认 zyogte 多进程）反而在 libc 内 `SIGSEGV/GP`（`0x1000017F95A5`），
  zygote/fork 链路还没打通。

## 结论（15:00 口径）

- A（`--dump-dom` 出 `<html>`）**用这份 chromium 二进制做不到**：命令处理器根本没有入口，
  不是内核 bug。
- B（Hive 里出窗口）需要：① 跨进程真共享内存（当前 `MAP_SHARED` 是私有快照）；
  ② 最小 Wayland compositor（Ozone/Wayland 后端已编入，245 个符号）。
- 已确认可行且存活的是「GUI 模式 + `--ozone-platform=headless`」，下一步可走
  `--remote-debugging-port` + 回环 TCP（`src/net.c` 已有 `NET_LOOPBACK_V4`）用 CDP 取帧/取 DOM。

### R14–R16（CDP / 网络路线探活，结论：今天走不通）
- 新增 `scripts/cdp_probe.py`：QEMU 加 `-netdev user,hostfwd=tcp:127.0.0.1:PORT-:PORT`
  + `virtio-net-pci`，宿主机直接对 guest 的 Chrome DevTools 端口发 HTTP。
- R15：宿主机 `/json/version`、`/json/list` 全部超时；串口日志里**没有任何 virtio/DHCP/10.0.2.x 行**。
- R16（客机内自测）：HBOS shell **不支持 `&` 后台**（整行被当成一条未知命令），
  且 `wget http://127.0.0.1:19222/json/version` 返回 **`wget: link down`** ——
  网卡/链路没起来。故 CDP 路线今天不可用（要先修 virtio-net 绑定 + 链路初始化，
  或给 shell 加 `&`）。
- 另外发现：`--remote-debugging-port` 时串口**没有** `DevTools listening on ws://…` 行，
  说明浏览器侧也没到开 DevTools 服务那一步（不能排除是启动参数被吞）。

## 今天（15:00）实际交付

1. 内核侧修复（已用日志逐条验证）：`ftruncate` 任意长度、`pwrite64`、`fallocate`、
   `fchown/lchown`、`fcntl` 记录锁、可写 `MAP_SHARED`、`madvise` 空洞容错、
   `prlimit64` 的 `EFAULT` 语义、mmap 把 PROT_NONE 预留池算作占用（修 PA 重叠/双重释放）、
   `_NSIG`=65、`rt_sigaction` 去掉非 Linux 的 EFAULT 校验。
2. 诊断设施：`[EXIT-BT]`、`[FDERR]`、mmap 失败回溯、`scripts/symbolize_rip.py`、
   `debugfs` 读镜像内 `--log-file`（拿全量 VLOG 不占串口）。
3. 结论性发现（写进交接，避免重复踩）：
   - Debian 这份 chromium **没有编 headless shell**：`--dump-dom/--screenshot` 的命令处理器
     没有入口，`--headless` 只会让浏览器"无窗口→干净退出"（`[EXIT] pid=1 code=0`）。
   - `--ozone-platform=headless`（不带 `--headless`）时浏览器**长期存活**（r12 验证），
     这是后续 GUI 路线的基础。
   - B 的硬前置仍是：跨进程真共享内存（当前 `MAP_SHARED` 只是私有快照）+
     最小 Wayland compositor（Ozone/Wayland 后端已编入）。

### R17（install-app：deb → 镜像 → QEMU；宿主机读写镜像；ISO 附加载荷）
- 新增 `scripts/install_app.py`：
  - `--deb x.deb`：`pkg2hax.py` 取 manifest（包名/`install_prefix`/entrypoint）→ `dpkg-deb -x` 解树
    → `debugfs -w -f`（镜像带 MBR，`?offset=1048576`）一次性 mkdir/write/sif/symlink 写进镜像；
  - `--img-ls P=/opt/...`、`--img-get`、`--img-put F= D=`：**在宿主机直接读写客机文件系统**；
  - 默认装完直接调 `qemu_session.py` 开机展示。
- `Makefile` 新增：`make install-app APP_DEB=./x.deb`、`make img-ls P=…`、`make img-get P=… O=…`、`make img-put F=… D=…`；
  `$(ISO_BIOS)` 规则支持 `ISO_PAYLOAD=<dir>`（拷进 ISO 根 `payload/`）；`src/fs.c` 已有 ISO9660 读取代码。
- 实测 `cowsay-off_3.03+dfsg2-8_all.deb`：
  ```
  [install-app] cowsay-off 3.03+dfsg2-8 -> /opt/cowsay-off  (4 files, 2 links)
  客机: ls /opt            -> cowsay-off/  <DIR>
        ls /opt/cowsay-off  -> usr/         <DIR>
        cat /opt/cowsay-off/usr/share/cowsay/cows/bong.cow -> ## A cow with a bong...
  ```
  注意：HBOS 的 `ls` **不支持 `-l`**（给了参数就静默无输出），脚本已改成裸 `ls`。
  另外 `cowsay-off` 只是补充 .cow 数据文件，`Depends: cowsay`；`cowsay` 本体是 Perl 脚本，
  要真正看到牛得再装 `cowsay` + `perl`。

## 当前卡点

1. 子进程 fork 后 `rt_sigaction` 被我们的「handler/restorer 地址合法性」校验拒成 `EFAULT` → `RAW_LOG(FATAL,"sigaction (set) failed.")` → `int3`。
2. 单进程初始化全过、线程干净退出 `code=0`，但 `--dump-dom` 无输出，`--vmodule=headless*=3` 无 VLOG → headless 命令处理器未执行。

### R18（termios / tty / pty / 作业控制）
- `src/tty.[ch]` 重写：共用行规程 `ldisc_t`（ICANON/ECHO/ECHOE/ECHOK/ISIG/ICRNL/OPOST+ONLCR、VMIN、^C/^\/^Z → 前台进程组、^D、^U/退格）；ioctl：`TCGETS/TCSETS{,W,F}/TCFLSH/TIOCGWINSZ/TIOCSWINSZ(→SIGWINCH)/TIOCSCTTY/TIOCNOTTY/TIOCGPGRP/TIOCSPGRP/TIOCGSID/FIONREAD/FIONBIO`。
- PTY：`open("/dev/ptmx")` 每次新建 master，`/dev/pts/N` 为 slave；`TIOCGPTN/TIOCSPTLCK`；`isatty()` 识别 tty/pty。`/dev/tty` 已注册，fd 0-2 的 ioctl 落到 tty0。
- 作业控制：`task_t` 增 `pgid/sid/stopped`；`setpgid/getpgrp/getpgid/setsid/getsid`、`kill(0/-pgid)`；SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU 真正停止，SIGCONT 恢复；`waitpid(WUNTRACED)` 上报停止；后台读/写终端触发 SIGTTIN/SIGTTOU。
- 验证：`tests/linux_tty.c`（`gcc -nostdlib -static-pie`，镜像内 `run /linux_tty`）QEMU 15 项全 PASS（pty、行规程、ONLCR、winsz、setsid/TIOCSCTTY/tcgetpgrp、SIGSTOP+WUNTRACED）。已知限制：master 无 slave 关闭/HUP 通知，VMTIME 未实现，fd 0-2 的实际读写仍走原 console 路径（未经新行规程）。
- R18b：fd0 读改走 tty0 行规程（`tty_console_read`：键盘→ICANON/ECHO/ISIG；^C 发给前台组，无前台组则发最近读者；无前台组时忽略 ^Z 以免卡死 shell）。回归 15/15 PASS；交互式 ^C 未实测。
- R18c：shell 新增 `jobs/fg/bg/stop`（对象=shell 子任务，`spawn` 出来的）。QEMU 验证 spawn→stop→jobs(Stopped)→bg→jobs(Running) 通过；`fg` 会给子任务独立进程组并设为 tty0 前台组（^C/^Z 作用于它，^Z 回到 shell 显示 [stopped]），但 ^C/^Z 的交互实测尚未做（需子进程在 read(0) 才泵键盘）。测试程序 `tests/linux_spin.c`。

### R27（Chromium 兼容层收口：zygote 握手打通）
- `MSG_NOSIGNAL(0x4000)` 进 `linux_compat_unix_send/recv` 白名单。Chromium 的每条
  `sendmsg` 都带它，原来被 `flags & ~MSG_DONTWAIT` 判成 `EOPNOTSUPP(-95)`，直接退回。
- `linux_message_io()` 的 control 解析改为遍历全部 cmsg，非 `SCM_RIGHTS` **静默忽略**
  （Linux 语义）。原来只认 cmsghdr[0] 且级别不符就 `EOPNOTSUPP`。
- **exec 继承 fd 表**（新增 `task_exec_inherit`）：HBOS 的 exec 是「建新任务 + 老任务退出」，
  新任务拿的是空 fd 表，fork 继承的 socketpair 全丢 → zygote 通信断。
  现在按 Linux 语义搬：非 `O_CLOEXEC` 的 fd 共享老表（`refs++`）、`parent_id/pgid/sid`
  对齐、信号掩码保留、`SIG_IGN` 保留、自定义 handler 复位。
- `rt_sigaction(SIGKILL/SIGSTOP, NULL, &old)` 纯查询返回 0 并填 `old_action`，
  只有带 `action` 时才 `EINVAL`。
- **`/proc/<pid>/exe` 返回真路径**：原来 `task_set_exe_path(new_id, node->name)` 只给
  basename，`readlink` 走到 `"/<任务名>"`（8 字节），Chromium 用 `dirname()` 得到 `/`，
  于是去 `/icudtl.dat` 找资源、无限重试。新增 `elf_set_exec_path()`，由 `execve` 调用方
  把绝对路径交给 loader。实测 `readlink` 从 8 字节变 39 字节，
  `/opt/chromium/usr/lib/chromium/icudtl.dat` 正常打开（fd=5）。
- 新增 `/proc/<pid>/task/{<tid>}/` 线程组视图（`status/stat/comm/maps`），
  修 `sandbox/linux/services/thread_helpers.cc:41` 的 FATAL。
- 结果：`Cannot communicate with zygote` 消失；browser(pid=4) 与 zygote 子进程(pid=6)
  并行跑 1100+ 条 syscall；`sendmsg` 返回 20/52（成功）。
- 仍存：主线程 capset 之后仍有 `int3`（无 FATAL 文本），待查。

### R28（Wayland 服务端第一版）
- 新增 `src/gui/wayland/`：
  - `wl_protocol.h`：接口名/版本/opcode/事件号常量（与 wayland.xml 逐位对齐）。
  - `wl_server.c`：socket 监听、连接池、对象表、线协议编解码、事件写入原语。
  - `wl_interfaces.c`：display/registry/compositor/surface/region/shm/pool/buffer/
    seat/xdg_wm_base/xdg_surface/xdg_toplevel 的请求处理。
- shell 新增 `wayland [path]`，默认 `/tmp/wayland-0`。
- 两个关键坑：
  1. **监听 socket 必须在服务线程自己的 fd 表里创建**。在 shell 里 `socket()` 拿到的
     fd 在线程里 `unix_for_fd()` 查不到，`accept` 永远 `EBADF`，客户端 `connect` 成功
     后一直等事件。
  2. **`new_id` 是客户端指定的号，服务端必须「认领」**。写成「要求该 id 已存在」
     会让每个带 `new_id` 的请求被静默丢弃，表现同样是 connected 但无事件。
     新增 `wl_client_adopt_id()`。
- `tests/linux_wayland_client.c`（PIE，raw syscall）验证通过：
  ```
  wayland-test: global 1 wl_compositor v4
  wayland-test: global 2 wl_shm v1
  wayland-test: global 3 wl_seat v5
  wayland-test: global 4 xdg_wm_base v2
  wayland-test: sync done
  wayland-test: PASS
  ```
- 已知缺口（下一步）：`wl_shm` 的 pool 还没 `mmap`（`buffer->pixels` 是空指针），
  所以还不能真传像素；`wl_registry.bind` 之后的 surface 生命周期只做了状态机，
  没接 HIVE 合成器出画面。

### R29（Wayland 四块补齐 + 目录分层 + 构建降级）

**目录分层**（`src/gui/wayland/`）：

```
core/    wl_wire.[ch]    线协议：消息头、参数游标、事件写入（纯函数，可单测）
         wl_server.[ch]  监听、连接、对象表、分派
iface/   wl_iface.h      共享状态（surface/buffer/pool 池）
         wl_iface_state.c   池分配 + 从 memfd 读像素
         wl_iface_display.c wl_display + wl_registry
         wl_iface_compositor.c wl_compositor + wl_surface + wl_region
         wl_iface_shm.c     wl_shm + wl_shm_pool + wl_buffer
         wl_iface_seat.c    wl_seat + wl_pointer + wl_keyboard
         wl_iface_xdg.c     xdg_wm_base + xdg_surface + xdg_toplevel + xdg_popup
backend/ wl_backend.h    渲染/输入后端接口
         wl_backend_hive.c HIVE 贴图 + 键鼠注入
```

分层的意义：core 不含任何协议语义，换协议不动；iface 只管协议状态；
backend 是唯一碰像素和输入设备的地方。

**补的四块**

1. **wl_shm 像素通路**：pool 记住客户端 fd，buffer 记住 pool 内偏移，
   合成时用 `linux_compat_memfd_read_at()` 按行取像素（按页边界分段拷贝）。
   新增 `gui_blit_argb8888()`：ARGB8888 与 HBOS surface 像素格式一致，
   整行不透明时直接 `memcpy`，只有真带 alpha 的像素才逐点混合。
2. **surface 接合成器出画面**：backend 每轮 poll 后遍历 dirty surface，
   读像素 → blit → 归还 buffer。
3. **xdg configure 链**：首次 commit（无 buffer）时发
   `xdg_toplevel.configure(w,h,states)` + `xdg_surface.configure(serial)`；
   `ack_configure` 后客户端才会真正 attach 上屏。
4. **输入回传**：`wl_backend_pointer_motion/button/axis`、
   `wl_backend_keyboard_key/modifiers`，按焦点 surface 分发。
   新增 `--probe-input` 开关：窗口首次上屏后自动注入一次
   enter/motion/button + key，供无鼠标键盘的自动化环境验证。

**构建降级**：`HIVE_REPO` 目录不存在时，`make run` 自动等价于
`make run-nogui`（no-GUI 构建不引用 HIVE/ 下任何文件）。

**踩的坑（都值得记）**

- **服务线程不能把"有连接"当成"有活干"**。原来 `poll_once` 对每个存活
  连接都返回 handled>0，于是协作式调度下服务线程永不让出 CPU，客户端
  被饿死 —— 日志显示 `flushed 124 bytes`，客户端却卡在 read 上。
  改成只有真正读到数据才返回 1。
- **解析循环必须独立于 recv**。一次 read 可能只拿到半个请求；客户端把
  请求全发完后不会再有 read 来触发解析，缓冲里那半条就永远躺着。
  拆出 `wl_client_consume()`，每轮 poll 都跑。
- **首个 buffer 不会触发 release**。原来只在"attach 新 buffer 替换旧
  buffer"时发 release，第一个 buffer 没有旧的可换，单缓冲客户端第二帧
  就卡死。改成合成完立刻归还。
- **`cmsg_len` 必须是 `CMSG_LEN(4)=20`，不是 `sizeof(struct cmsghdr)`**。
  后者末尾按 8 字节对齐补到 24，内核算出「2 个 fd」把垃圾当 fd，整条
  sendmsg 返回 EINVAL。
- **测试程序的请求 size 不能手写**。`wl_shm` 的 bind 是 32 字节，
  手写成 40，服务端按 40 切分，后面所有请求跟着错位。改成发送前回填。

**验证**（`tests/linux_wayland_surface.c`，`scripts/build_wl_tests.sh` 编译）：

```
global 1 wl_compositor / 2 wl_shm / 3 wl_seat / 4 xdg_wm_base
configure ok, serial=3
buffer released
pointer ok
keyboard ok
PASS
```

**误判纠正：不是栈不对齐，也不是文件系统，是文件名对不上**

这一轮连着误判了两次，都记下来，免得下次再踩。

**误判一：用户态栈不对齐。** 依据是"用 `-mpreferred-stack-boundary=3`
重编就好了"。实测否掉了：

- `main` 里 `rsp%16` 是 8 还是 0，取决于 GCC prologue 里 `sub` 了多少
  （默认 `sub $0x28,%rsp` 恰好把 8 变成 0）。**两者都符合 ABI**，
  拿这个值判对齐本身就是错的，要看看进入函数**之前**的 rsp。
- `__builtin_return_address(0)` 落在 `0x1000002A78x`（内核运行时段），
  说明 HBOS 不走 glibc 的 `__libc_start_main`，main 由内核侧运行时直接
  调用，进 main 前 `rsp%16==8`，栈本来就是对的。

**误判二：文件系统覆盖可执行文件会静默失败。** 依据是"`gcc -o` 之后
`ls` 不到新文件"。也不成立 —— 又建又删地实测过，`rm` 和 `gcc` 都正常。

**真凶：编译产物的名字和安装进去的名字不是同一个。**

`build_wl_tests.sh` 用 `basename` 生成 `build/tests/linux_wayland_client`
（源文件名带 `linux_` 前缀），而安装步骤一直装的是
`build/tests/wayland_client` —— 那是最早手工编的一个旧文件。
于是"改代码 → 重编 → 装上 → 行为没变"，每次都在跑旧二进制。

所以那个 `[SCHED] BAD CTX rsp=0` 从头到尾就是**一个早就不该用的旧文件**
造成的，跟栈、跟文件系统都没关系。

**修法**：脚本里把"源文件 → 客机路径"的对应关系写死（`pair` 列表），
输出名去掉 `linux_` 前缀，和 `/bin/` 下的名字严格一致。不再依赖
`basename` 猜。

教训很朴素：**先确认自己跑的是不是刚编出来的那个文件**（比大小、
看时间戳、`strings` 找一句新加的日志），再去怀疑内核。

**一条命令跑完整回归**：`make wl-proto-test`

编译 → 装进镜像 → QEMU 跑两个用例 → 打印结果行。名字对应关系由
`scripts/build_wl_tests.sh` 的映射表统一给出，不再靠人工对。日志落在
`build/tmp/wl-proto-test.log`。

（注意：这个内核在跑用户程序时会往串口打大量 `pid=N nr=X ret=Y` 的
syscall trace，一条用例能刷几百行。日志文件里用
`grep -E "wayland-test:|wl-surface:"` 过滤即可。）

### R30（身份系统 / CI / 鼠标测试 / ppoll）

**1. `make test` 两个真实失败，都修了**

- `ui-test` 链接失败：`theme.c` 调 `vfs_mkdir`，但 `tests/host/theme_fs_stub.c`
  没提供。补了个 stub（`/themes` 置真，其它路径假装成功）。
- `dirty-test` 两项失败：测试期望「相邻矩形合并」，但实现**刻意不合并**
  （相邻合并得到的外接矩形会把中间空隙纳入重绘，那块空隙被重画成当前
  桌面状态，正好抹掉本该保留的窗口内容 —— 就是用户反馈过的"残留方块"）。
  实现是对的，改测试：用一个同时压住左右两块的矩形来触发级联合并。

**2. 发行版身份系统（`src/tools/distro_seed.c`）**

- 三个档案 `hbos` / `debian`(12, bookworm) / `arch`(rolling)。
- **探测文件与 primary 解耦**：`/etc/debian_version` 和 `/etc/arch-release`
  按"装过没有"独立维护，可以同时存在；`/etc/os-release` 的 `ID` 跟 primary。
  这就是"同时装 deb 和 aur"的落地方式 —— 靠文件探测的安装脚本两边都过。
- primary 只在**首次** add 时设置，之后装别家的包不会悄悄改掉整个身份。
- `distro` shell 命令：`status` / `use ID` / `add ID [VER]` / `reset`。
- 状态存在 `/etc/hbos-distro.conf`，单一来源，避免半吊子状态。
- 实测：`distro add debian` + `distro add arch` → `installed formats: deb arch`，
  `cat /etc/debian_version` → `12`，os-release 里 `ID=debian` 且 `HBOS_*` 字段都在。

**3. 真实鼠标移动测试（`scripts/mouse_move_test.py` + `make mouse-test`）**

QEMU monitor 注入 `mouse_move`，截图对比验证整条链路。
两个坑：
- 不先 `sendkey ret` 会在引导菜单上干等 —— 截图只有十几种颜色，
  鼠标当然不动，看起来像"指针坏了"，其实是桌面没启动。
- 必须裁掉顶栏再比：HIVE 顶栏有时钟，秒数每帧在跳，不裁的话
  "时钟跳了一秒"会被误判成"鼠标动了"。

实测：基线差异 0，注入 3×90px + 70px 后差异区域 `(960,501)-(1245,592)`
面积 25935 —— 正好是从屏幕中心往右下，和注入方向一致。

顺带加了 `mouse` shell 命令（读 `gui_pointer_publish` 发布的坐标），
主循环每帧钳制后发布一次 —— 以前指针位置是纯局部变量，外部完全看不到。

**4. `ppoll` 实现（Chromium 事件循环的硬缺口）**

日志里刷 `unimplemented nr=271`，271 = ppoll。Chromium 的事件循环主等待点
就在这条上。实现方式是 timespec → 毫秒后转 `linux_compat_poll`，
sigmask 忽略（HBOS 没有"等待期间原子换信号掩码"的语义，忽略只会让信号
晚送达，不会丢）。

结果：`unimplemented nr=271` 从刷屏变成 **0 次**，只剩一个 `nr=444`
（landlock_create_ruleset，沙箱用）。

**5. `make test-all`**

`test`（宿主机单测，20s，CI 第一道门）+ `wl-proto-test`（Wayland 协议）
+ `mouse-test`（真实鼠标）。CI 里拆成 `host-tests` 和 `qemu-tests` 两个 job，
后者依赖前者通过，失败时上传 `build/tmp/*.log` 和截图。

**Chromium 当前状态**：进了 Wayland 后端（`ui/ozone/platform/wayland/.../drm_render_node_path_finder.cc`
报错说明真的走在 wayland 平台上）。`drmGetDevices2()` 找不到设备**并不妨碍它继续连上我们的
socket** —— 现场日志里 `[WL] client accepted` + `rx 24 bytes` + `flush 252 bytes` 都已出现，
即它连上了、拿到了全部 registry 事件，**但一个 bind 都不发**。所以 DRM/landlock 不是当前
阻塞点，阻塞点在客户端收到 registry 之后的协议处理。
（注：`browser/chromium/args.hbos.gn` 里的 `ozone_platform = "hbos"` 是**源码构建的备选路线**，
这份 Debian 二进制并没有编入 hbos 后端，实际走的是 `--ozone-platform=wayland`，两者别混。）

### R31（Chromium 深度排查：ppoll / landlock / wl_output / 线程探针 / 性能基线）

**内核侧补的三个 syscall**

- `ppoll`（271）：Chromium 事件循环的主等待点，原来一直刷
  `unimplemented nr=271`。实现 = timespec 转毫秒后调 `linux_compat_poll`；
  `sigmask` 忽略（HBOS 没有"等待期间原子换信号掩码"的语义，忽略只会让信号
  晚送达，不会丢）。**修完归零**。
- `landlock_create_ruleset` / `add_rule` / `restrict_self`（444/445/446）：
  统一返回 **ENOSYS**。必须是 ENOSYS 不能是 EPERM —— ENOSYS 的语义是
  "这内核没这个系统调用"，Chromium 会判定不支持、安静走无沙箱路径；
  EPERM 会被理解成"支持但不让你用"，沙箱初始化直接 FATAL。

**Wayland 服务端补的接口**

- `wl_output`（v3）：**必须注册**。客户端靠它的 mode 事件确定屏幕尺寸，
  缺了它 Chromium 拿到 registry 之后就不动了。bind 的那一刻就发
  geometry → mode → scale → done。
- `wl_data_device_manager`（v3）、`wl_subcompositor`（v1）：注册全局对象，
  请求只创建子对象、不实现实际数据传输。Chromium 的 ozone 初始化会 bind
  它们，缺失时初始化会静默停住。
- 服务端加了受预算限制的诊断日志（`client accepted` / `rx N bytes` /
  `req obj= op= kind=` / `flush N bytes`），正常运行时只打前几条。

**排除掉的假设**

- **线程没问题**：`tests/linux_thread.c`（新写）—— 4 线程创建、调度、
  `pthread_mutex` 800 次加减，全部正确。不是 clone(CLONE_THREAD) 的问题。
- **不是缺接口**：补上 `wl_output` / `wl_data_device_manager` /
  `wl_subcompositor` 后，`flush` 从 136 → 168 → 252 字节，客户端行为
  **完全不变**（依旧一个 bind 都不发）。
- **不是内核卡住**：ppoll 诊断显示 `nfds=1 to=∞ ret=1` —— 它等到了可读、
  读了数据就静止；进程还活着，最后在 `madvise`/`mprotect`（内存分配）。
- **不是 syscall 缺失**：`unimplemented` 统计已清零。

**结论**：服务端正确、客户端收下了响应，但**没有继续 bind**。问题在
libwayland 的事件分发侧（收到 `wl_registry.global` 后没有回调）。

**性能基线（`scripts/perf_baseline.py`，新写）**

测两个客观量，3 次取中位数（TCG 无 KVM）：

```
boot      = 3.02s     QEMU 启动 → "[KERN] Shell ready"
workload  = 9.97s     Wayland surface 测试端到端墙钟（含 QEMU 冷启动）
```

有了这把尺子，后面改代码才有"优化了"的依据 —— 之前只能凭感觉。

**其它**

- `make test-all`：`test`（宿主机单测）+ `wl-proto-test`（Wayland 协议）
  + `mouse-test`（真实鼠标），全绿。
- 新增 `tests/linux_thread.c`（线程探针）、`scripts/perf_baseline.py`
  （性能基线）、`tests/linux_wayland_align.c`（栈对齐探针）。

### R32（宿主机复现台 + 真机单轮验证：Wayland 嫌疑被证伪，阻塞点前移到 fork）

**1. 修掉一个把排查带偏的诊断盲区（最重要）**

状态总览里"客户端一个 bind 都不发"这个结论，证据来源是**有限预算日志**：
`wl_rx_log_budget` / `wl_dbg_dispatch_budget` 打完前几条就静默。所以
"日志里没有 bind"**不等于**"客户端没发 bind"——后者从未被真正观测过。

- 新增**无条件**打印 `[WL] BIND from client obj=N`（`wl_server.c`，
  不受任何预算限制）。

**2. `WAYLAND_DEBUG=1` 为什么"没生效"——是我们自己的 bug**

之前怀疑"Chromium 会 `clearenv()`"。真实原因：`src/tools/app.c` 的
`build_linux_env()` 用**写死的固定 7 项**构造 envp，用户设的变量根本进不了
子进程。修法：`app_envp` 扩到 16 项，追加透传表；新增 `export NAME=value`
shell 命令（`export` / `export -c` / 裸 `export` 列已设项）。

**3. 宿主机复现台：序列化层被证伪**

新增 `tests/host/wl_libwayland_replay.c` + `make wl-libwayland-test`。
用**真实的 `wl_wire.c`** 生成 252 字节，经 socketpair 喂给真 libwayland 1.26.0：

```
真 libwayland 回调 wl_registry.global 次数 N = 7 / 7
name=1 wl_compositor 4    name=5 wl_output 3
name=2 wl_shm 1           name=6 wl_data_device_manager 3
name=3 wl_seat 5          name=7 wl_subcompositor 1
name=4 xdg_wm_base 2
```

**据此排除**：消息头 size/opcode 位域、string 长度含 NUL、4 字节补齐、
消息 size 计算、name/version 编码。R31 计划里"最可疑的地方"（global 字节布局）
可以划掉。

台子带负对照（证明它有检出能力），3/3 全中。其中**变异3 揭示了一个真实盲区**：
libwayland **完全不校验 `global.version`**，`0xdeadbeef` 原样透传、N 仍为 7。
所以「N=7」本身证明不了 version 正确，靠独立结构校验器补上。

**4. 真机单轮验证：阻塞点不在 Wayland，在 fork 后的子进程**

带 `export WAYLAND_DEBUG=1` 重跑 Chromium（`build/tmp/r32-chromium2.log`）：

```
[FORK] pid=4 rip=0x1000017f9546
[TRAP] user SIGSEGV addr=0x0 rip=0x1000017F95A5
  err=5 (PF_P|PF_U，存在页 + 用户态 + 读，无 PF_W、无 PF_I)
```

- **`[WL] client accepted` 一次都没出现** —— Chromium 根本没连我们的 socket。
  所以"连上了但不出画面"这个描述本身是错的，阻塞点更早。
- 崩溃地址 `0x1000017F95A5` 与 R11 记录**完全相同**，是 fork 出的子进程
  在 libc 内解引用空指针（CR2=0）。**不是内核 bug，也不是 Wayland 问题。**
- `vma: (rip 不在任何映射区间内)` 但 `flags=665`（P=1/U=1/NX=0）——页本身
  有效可执行，说明是用户代码自己解引用了 0，rip 只是恰好落在库里。

**5. 顺带修掉 T2 审计确认的 3 处真缺陷**（都不是本轮阻塞点，但都是真 bug）

- `wl_client_flush` 遇 `EAGAIN` 会把剩余字节丢弃**并关闭连接**。accept 出的
  fd 继承了监听 fd 的 `SOCK_NONBLOCK`，一次瞬时背压就等于"截断 + 掉线"。
  改为保留未发送部分、返回 0。
- `wl_out_begin` 空间不足时只 `return 0` **不置 `in_error`**，调用方普遍不查
  返回值，后续写入会覆盖上一条事件的尾部，产出 header 与 body 不符的畸形消息。
- `wl_display.delete_id` / `wl_display.error` 从不发送（后者连常量都没有）。

**本轮结论**：Wayland 服务端这条线**已经从嫌疑名单上划掉**。下一轮要查的是
**fork 之后子进程为什么在 libc 里解引用 0**——那才是挡住 Chromium 的地方。

