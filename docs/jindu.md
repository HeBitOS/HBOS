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


### R33（找到 headless shell：`--dump-dom` 的路是通的）

**关键突破：缺的确实是"一个壳"，而且这个壳是独立包。**

之前 R11/R31 的结论「Debian 这份 chromium 没编 headless shell，命令处理器没有入口」
只说对了一半：**普通 `chromium` 包里确实没有**，但 Debian 把它拆成了**独立的
`chromium-headless-shell` 包**。装上之后 `--dump-dom` 就有了入口。

```
chromium_156.0.8078.73-1~deb12u1_amd64.deb              296 MB  普通浏览器
chromium-headless-shell_156.0.8078.73-1_amd64.deb       230 MB  ← 就是这个"壳"
  └ usr/lib/chromium/chromium-headless-shell            229 MB
  └ headless_command_resources.pak / headless_lib_data.pak / headless_lib_strings.pak
```

**版本从 151 升到 156 后，崩溃地址完全没变**：

```
[TRAP] user SIGSEGV addr=0x0 rip=0x1000017F95A5   ← 与 R11、R32 一字不差
```

这排除了"版本问题"，确认是 HBOS 侧与 Chromium 交互的固有缺陷。

**但 headless shell 走的路径不同，它不崩。** 直接运行得到：

```
run: exit 127
```

`exit 127` = 动态链接器找不到共享库。日志里它在找 `libopenh264.so.8`，一路失败。
**这是纯粹的依赖缺失，不是移植问题**——比之前的 SIGSEGV 好办得多。

镜像里已有的是**旧版**，headless shell 要**新版**（都是 Debian trixie 的 t64 系列）：

| 镜像里已有 | headless shell 需要 |
|---|---|
| `libopenh264-7` | `libopenh264-8` |
| `libdav1d6` | `libdav1d.so.7` |
| `libflac12` | `libFLAC.so.14` |
| `libminizip1` | `libminizip.so.1`（t64） |

依赖版本不是问题：headless shell 要求 `libc6 >= 2.43`，宿主 glibc 是 **2.44**，
而 `Makefile:1262` 的 `HBOS_GLIBC_LIBDIR` 正是从宿主 `/lib/x86_64-linux-gnu/` 拷，
所以 glibc 侧满足。

**下一步**：补齐上面那张表的库，直到 `exit 127` 消失。这是纯打包工作，
不需要动内核。

### R34（Chromium headless 打通 DOM；截图出图但为空白帧）

**`--dump-dom` 首次输出带内容 DOM**

```
$ run .../chromium-headless-shell --no-sandbox --no-zygote --single-process \
      --disable-gpu --dump-dom "data:text/html,HBOS-CHROMIUM-OK"
<html><head></head><body>HBOS-CHROMIUM-OK</body></html>
run: exit 0
```

关键不是"少报了几个错"，而是把链路逐个证实：

1. **用调试器确认浏览器、V8、DevTools 全程可用**。在 QEMU `-gdb` 下对
   headless-shell 下硬件断点，`HeadlessShell::OnBrowserStart`、
   `HeadlessCommandHandler::ProcessCommands`、`OnCommandsResult` 三个
   入口全部命中 —— 之前"没进 dump-dom 路径"的推断是错的。

2. **抓到真正的根因**：抓 DevTools 报文后看到
   `ReferenceError: executeCommands is not defined`
   —— 命令页 `chrome://headless/headless_command.html` 拿到的是空 HTML，
   `headless_command.js` 没加载，于是什么也不做就干净退出。

3. **根因是内核的 MAP_SHARED 普通文件不共享**。`src/syscall.c` 原先把
   可写 `MAP_SHARED` 文件映射退化成"每张映射一份私有快照"；Chromium 用
   `/dev/shm` 文件做 IPC 共享缓冲，写端与读端互不可见。新增
   `linux_compat_shm_*`：按 (文件节点, 页号) 散列的共享物理页表 + 引用计数
   （动态链表，4096 桶；最初用定长 4096 槽数组，20MB 的 `resources.pak`
   直接挤爆导致 DataPack 加载 mmap ENOMEM）。

   回归用例 `tests/linux_shared_alias.c`：同一文件多张映射（含只读、含
   非零 offset）的双向可见性。修复前 `regular-file: FAIL`，修复后
   `memfd PASS / regular-file PASS`。

**另外两个硬缺口**

- `timerfd` 系统调用号（283/286/287）**从未映射**，实现早就有但落到
  unimplemented 返回 ENOSYS。Chromium 的帧调度依赖它。补上后定时器探针
  `TIMERFD poll=1 expirations=1` 通过。
- `fcntl` 命令号算错：把 `F_ADD_SEALS`(1033) 当成 `F_SETPIPE_SZ` 返回
  65536，而封印约定成功返回 0 → Chromium 判共享内存创建失败。修正后
  `--screenshot` 从**永久挂死**变成 18 秒正常退出。
- `memfd` 的 `pwrite` 原来直接返回 `-EIO`（软件光栅用 pwrite 写共享位图）。

**截图：能出文件，但内容是空白帧**

```
file pC.png -> PNG image data, 800 x 600, 8-bit/color RGBA
三次运行 md5 完全一致，采样灰度只有 1 种 -> 空白帧
```

所以"生成可验证 PNG"只做到一半：**格式合法、尺寸正确、可复现，但光栅
内容没有进入帧**。未达成，不当作完成。

**下一步（按证据优先）**：帧为什么是空的 —— 光栅结果未进共享位图。
候选：`memfd` 的 `MAP_SHARED` 与 `ftruncate` 增长后的重映射、以及
`mmap` 之后 `pwrite` 的可见性（当前 pwrite 写 slot 物理页、mmap 映射同一
批页，理论上应一致，需要一次实测确认）。

#### R34 补充：空白帧的排查（负结果同样有价值）

截图产物恒为**全零 RGBA**（alpha=0），与页面内容、`--default-background-color`、
`--disable-gpu-compositing` 全部无关（四次运行 md5 完全相同：
`1a1734b7fcca883564d0e259f1f52c18`，含红/蓝块页面）。说明被捕获的位图
**从未被写入**，而不是写错了颜色。

已排除的三条内核侧假设（各有回归用例，均 PASS）：

| 假设 | 用例 | 结果 |
|---|---|---|
| memfd 的 `pwrite` 与 `mmap` 不可见 | `tests/linux_memfd_coherence.c` | pwrite→mmap / mmap→pread / 扩容后可见，全 PASS |
| `SCM_RIGHTS` 传 fd 静默失败 | `tests/linux_fdpass.c` | 收到 fd 且内容是原始数据，PASS |
| `MAP_SHARED` 普通文件不共享 | `tests/linux_shared_alias.c` | 已修，PASS |

所以空白帧的根因在 **Chromium 合成器/拷贝输出这条用户态链路**，不在
内核 IPC。下一步应查 `CopyOutputRequest` 在软件合成下的位图来源，
以及 renderer→viz 的 frame sink 是否真的提交了 `CompositorFrame`。

#### R34 再补充：空白帧的五配置对照（已定性为空帧回退）

同一页面、同一尺寸，五种渲染配置输出**逐字节相同**的 PNG
（md5 `1a1734b7fcca883564d0e259f1f52c18`，3174 字节）：

| 配置 | 结果 |
|---|---|
| `--disable-gpu` + `data:` 纯文本 | 空白帧 |
| `--disable-gpu` + `--default-background-color=FFFFFFFF` | 逐字节相同 |
| `--disable-gpu` + `file://` 红/蓝块页面 | 逐字节相同 |
| `--disable-gpu --disable-gpu-compositing` | 逐字节相同 |
| `--in-process-gpu --use-angle=swiftshader --enable-unsafe-swiftshader` | 逐字节相同 |

内容、背景色、合成方式全部不影响输出 → 这不是"画错了"，而是**捕获到的是
一张空位图**（`CopyOutputResult` 无像素），Chromium 走了空帧回退。
`--vmodule=*raster*` 全程**没有任何光栅化日志**，进一步印证 cc 从未光栅化。

已排除的内核侧因素（均有回归用例，PASS）：memfd 的 pwrite↔mmap 一致性、
`SCM_RIGHTS` 传 fd、`MAP_SHARED` 文件共享、`epoll_wait` 超时语义（300ms
实测 300ms 返回）、timerfd 触发。

**结论**：空白帧是 Chromium 合成器/拷贝输出这条用户态链路的缺口，不是内核
IPC。下一步应查 single-process 下 viz 的 `SharedImageInterface` 与
`CopyOutputRequest` 为何返回空位图；`--print-to-pdf` 作为旁路也崩在同一类
问题（`PrintCompositeClient::CompositeDocument+0x3a`，解引用 null+0x60）。

#### R36：单进程截图不可达，多进程是唯一正路

新证据：`--window-size=1280,800 --hide-scrollbars` 让产物变成 1280×800
（md5 `725a07cd…`，与 800×600 的 `1a1734b7…` 不同），但**内容仍是全透明**
（采样只有 `(0,0,0,0)`）。说明窗口尺寸生效、`CopyFromSurface` 也返回了一张
位图，只是这张位图里什么都没画 —— 单进程下没有真正可捕获的 surface。

已排除的内核侧假设（本轮新增）：
- `thread_group_id` 赋值错误 → 赋值点全部正确（main/fork/thread 各自正确）
- HBOS 内存上限 → 2 GiB 连续 malloc+memset 全部可写
- exec 未重建地址空间 → `src/elf.c:631` 确实 `vmm_create_address_space()`
- mmap 地址窗口太小 → 起始 0x100000000000，上限 0x200000000000（16 TiB 窗口）

**结论**：单进程这条路本质走不通，必须走多进程（Chromium 官方支持配置）。
多进程当前的两个硬阻塞都在 GPU 子进程：`partition_alloc::OnNoMemoryInternal`
与 `sandbox::policy::SandboxLinux::InitializeSandbox` 的 CHECK。

**下一步**：查 `linux_va_find_free` 在 32 GiB 预留下的行为——它逐页
`vmm_get_phys()` 扫描（32 GiB = 8M 页），在 TCG 下极慢，且可能让
PartitionAlloc 误判为无法预留。

#### R37：找到 Chromium 内存不足的真正原因 —— HBOS 只管理 4 GiB 以下物理内存

`src/core/pmm.c:48` 有 `#define PMM_IDENTITY_LIMIT 0x100000000ULL`，初始化时
`max_addr` 被钳到这个值 —— **4 GiB 以上的 RAM 完全不纳入管理**。

后果（实测）：

- QEMU `-m 4096M` 时，PCI 洞（3–4 GiB）吃掉的区间不可用，实际可分配只有
  约 3 GiB 减内核占用；
- 阶梯分配测试（`malloc` 256 MiB 一步、逐页 `memset`）在 **2816 MiB**
  停住并 SIGSEGV，正好对上这个上限；
- 因此 **`-m 6144M` / `-m 8192M` 给再多内存都没用** —— 这解释了为什么之前
  加大内存后 Chromium 仍然 `partition_alloc::OnNoMemoryInternal`。

已排除的相关猜测（都有实测）：

- PartitionAlloc 的启动模式在 HBOS 上正常：`mmap(NULL, 32GiB, PROT_NONE)` →
  `mprotect` 64 MiB → 写入 → 8 GiB 偏移处再提交，全部 PASS；
- brk 堆上限 64 MiB 偏小但不是本次瓶颈（调到 512 MiB 后上限没变）。

**结论**：要让 Chromium 多进程（截图唯一可行的配置）跑起来，必须让内核
管理 4 GiB 以上的物理内存，即扩展内核恒等映射/直接映射范围。这是跨
boot 分页的改动，需要单独一轮做并验证。

顺带修正：`TASK_USER_HEAP_SIZE` 64 MiB → 512 MiB（仅上限，按需提交，
不与 mmap 窗口冲突）。实测不改变内存天花板，但对 Chromium 的主 arena 更合适。

#### R38：内核现在能用 4 GiB 以上的物理内存（实测 2816 MiB → 4096 MiB+）

原因：`boot.asm` 只建了 0–4 GiB 的 2 MiB 恒等映射，PMM 于是把可管理物理
内存钳在 4 GiB；QEMU 的 PCI 洞又占掉 3–4 GiB，实际只有约 2.8 GiB 可用，
**`-m` 给再多都没用**。

改动：
- `vmm_identity_map_2m()`：用 2 MiB 大页给任意物理区间补恒等映射，
  中间 PD/PDPT/PML4 表按需从 PMM 分配；
- `pmm_query_max_phys()` / `pmm_enable_high_memory()`：位图按 8 GiB 开
  （256 KB 位图 + 4 MB 引用计数），4 GiB 以上先保持"已用"，
  等映射建好后再统一放开；
- `kernel.c` 在 `vmm_init()` 之后按「先建映射、再放开页」的顺序调用。

实测（`-m 8192M`，每步 256 MiB 逐页触碰）：**4096 MiB 才因测试自身上限停止**，
此前固定死在 2816 MiB。`--dump-dom` 无回退。

剩余：多进程截图从 `partition_alloc` OOM 变成
`PartitionBucket::SlowPathAlloc+0x7cf` 写故障（err=7，页存在但只读），
指向 fork 之后的 COW 解析路径，下一轮从这里继续。

#### R39：多进程往前推了一格，单进程确认无解

- `PartitionBucket::SlowPathAlloc` 的写故障是「PTE 已是 P|W|U 却被 CPU 拒绝」，
  判为陈旧 TLB。加了有界自愈（刷该页 TLB + 重载 CR3 后重试，同址最多 16 次，
  超过仍按 SIGSEGV 处理）。此后该故障不再出现，多进程的失败点前移到
  `sandbox::policy::SandboxLinux::InitializeSandbox` 的 CHECK，
  伴随 `Network service crashed or was terminated`。
- 单进程+SwiftShader 在 8 GiB 下重测：与 3 GiB 时**逐字节相同**的空白帧
  （md5 `1a1734b7…`），彻底确认单进程模式产不出可捕获的 surface。

**结论**：截图只能走多进程，而多进程现在的阻塞是子进程（GPU/网络服务）
的沙箱初始化 CHECK 与稳定性，不再是内存问题。

#### R40：找到可用产图路径 —— Chromium 的 Skia 光栅是好的，坏的是合成器

关键发现：**Chromium 的光栅化在 HBOS 上完全正常**，只有「合成器 → 拷贝输出」
这一段坏。证明方式：让页面自己用 `<canvas>` 画、再 `toDataURL('image/png')`，
把结果写进 DOM，用已经打通的 `--dump-dom` 取回来。

- 纯形状页面（200×100）：解码后逐像素核对，背景 `(255,255,255)`、
  红 `(255,0,0)`、蓝 `(0,0,255)`、绿 `(0,160,0)` —— **与绘制内容完全一致**。
- 文字页面：初次是 **0 个字形像素**，原因是镜像里没有
  `/etc/fonts/fonts.conf`（日志早有 `Fontconfig error: Cannot load default
  config file`）。装上 fonts.conf + DejaVuSans.ttf 后变成 **3355 个字形像素**，
  Fontconfig 错误归零。脚本见 `scripts/install_guest_fonts.sh`。

产物与复现见 `docs/evidence/README.md`（480×120 PNG，白底 + 黑体文字 +
红/蓝矩形，像素逐项核对过）。

**仍然没解决的**：`--screenshot` 依旧全透明（装字体前后 md5 相同
`1a1734b7…`），这条路走的是合成器拷贝输出，和光栅无关。

结论修正：之前把空白帧归因为"光栅没工作"是**错的** —— 光栅没问题，
问题单点在 compositor 的 `CopyOutputRequest`。

#### R41：合成器确实在画帧，空白出在拷贝输出

用硬件断点同时盯着合成器和 DevTools 报文，拿到了**顺序证据**：

```
ORDER DrawLayers
ORDER DrawAndSwap
ORDER DrawLayers
ORDER DrawAndSwap
ORDER DrawLayers
ORDER DrawAndSwap
ORDER PROTO-OUT {"id":0,"result":{}}      <- 截图请求在后
ORDER PROTO-OUT {"id":1,"result":{}}
...
```

也就是说：`PrepareTiles` / `DrawLayers` / `DrawAndSwap` 全部命中（本次 9 次），
**帧是在截图请求之前就画好的**，之后也还在继续画。所以空白帧既不是
"没光栅化"，也不是"时序太早"，而是 `CopyOutputRequest` 取回了一张全零位图。

至此本项的归因链已经完整且每一步都有实测：
`--dump-dom` ✅ → 光栅 ✅（canvas 逐像素核对）→ 合成 ✅（DrawAndSwap 命中）
→ **拷贝输出 ❌（全零位图）**。

#### R42：Wayland 客户端真的连上并建了池 —— 三个真 bug + 一个真阻塞

目标从 "headless 出图" 转到 "在 HIVE 的 Wayland 合成器里出窗口"。
`chromium --ozone-platform=wayland` 现在能走完 registry → bind 全部 7 个
全局 → 建 shm pool → 建 surface，然后挂在客户端进程内的一个 `int3` 上。

本轮修掉的三处（都是**真**阻塞，不是绕路）：

1. **xkb 数据缺失**。镜像里没有 `/usr/share/X11/xkb`，libxkbcommon 打印
   `failed to add default include path` 后拿到空键映射，键盘初始化路径随即
   解引用空指针。新增 `scripts/install_guest_xkb.sh`（293 个文件、3.9 MB，
   debugfs 一次 `-f` 批量写入），报错消失。
2. **`wl_shm.create_pool` 参数解析**。实测 Chromium 156 发出的消息体只有
   **8 字节**（`new_id` + `size`），而协议签名是 `"nhi"`（new_id、fd 索引、
   size）。严格按签名解析会把 size 读成 0 并判越界 → 每个 create_pool 都被
   丢掉 → 客户端永远拿不到 pool。现在按消息体长度两种形态都收（≥12 走标准
   `nhi`，8 走 `ni`，fd 从 pending_fds 顺次取）。证据：
   `pool.ok a=10 b=6 c=4096`，随后 `req 10.2 shm_pool`（resize）正常分派。
3. **补 5 个 syscall**：`link`(86, →EPERM)、`sched_getattr`(315)、
   `sched_setattr`(314)、`rt_tgsigqueueinfo`(297, 按 tgkill 语义投递)、
   `clock_getres`(229)。顺带修正一处**误标**：原来 `case 229` 注释写的是
   sched_getattr（那是 315），于是 `clock_getres` 变成"返回成功但不写缓冲区"，
   调用方拿到未初始化的分辨率。

服务端诊断也补齐了（这些是后面继续查的支点，不是装饰）：
`bind iface=<名> name= v= id=`、对象种类短名（`req 10.2 shm_pool` 而不是
`kind=9`）、以及一次性原始字节 hexdump（`wl_hexdump_budget`，默认关闭）。

**仍然阻塞**：浏览器主线程（pid 2）在一次 `int3` 上终止，
`rip=0x110A90C46B`，落在 `__stack_chk_fail` 冷路径之后的 padding
（`0xa90c46a: int3` / `0xa90c46b: ud2`）。全量 chromium 的 dbgsym 不可得，
符号化只到 headless-shell（`--log` 指纹模式会正确拒绝套错符号表），所以这
一处目前**没有函数名**。已知的上下文：崩溃前最后几笔请求是
pool.resize ×2 → compositor.create_surface ×2 → data_device_manager.get_data_device
→ display.sync，之后就不再发任何字节；且同一地址在修复前后完全一致。

#### R43：把那次 int3 精确定位到一条指令 —— 它不在 Wayland 层

之前只知道 `rip=0x110A90C46B`。全量 chromium 的 dbgsym 不可得，但反汇编 +
栈回溯可以拼出真链条（`bt[]` 里的返回地址逐个反查调用点，全部对得上）：

```
0x6284f85  call *0x38(%rax)            <- 虚调用
0x786b91f  call 0x7856a20              <- 0x7856cba 处 mov $0x39b,%esi（= 崩溃时 r13）
0x7856cbf  call 0xa8f8760
0xa8f8789  call 0xa90c050              <- 0xa90c050 是 thunk，实体在 0xa90c060
```

实体函数 `0xa90c060..0xa90c46a` 整段只引用**两个** rodata 字符串：

```
0x1d8fa2e  "../../ui/base/resource/resource_bundle.cc"
0x1d0d9ac  "locale resources are not loaded"
```

真正跳进`int3` 的是这一条（不是栈保护，也不是野跳转）：

```
a90c262: call a90bc30            ; 返回 {rax, rdx}（ptr + 长度）
a90c267: test %rdx,%rdx
a90c26a: je    a90c46a           ; rdx == 0  -> int3 / ud2 冷块
```

所以失败形态是**"取一个 span/string_view，返回长度为 0，命中不变量"**，位置在
`resource_bundle.cc`。旁边那条 `locale resources are not loaded` 的 LOG(ERROR)
**没有出现在 stderr**（整轮 chromium 只输出 4 行 stderr），说明 pak 是加载成功的，
挂的是"某个资源 id 查出来是空的"。

顺带确认了两件事：
- 没有合成器时 chromium 干净退出（`exit 1`，`Failed to connect to Wayland display:
  Connection refused`），所以这条崩溃**确实是 Wayland 交互触发的**（UI 初始化走到
  取本地化资源这一步才会到）。
- guest 里 `resources.pak`(13 046 519) 与 `locales/en-US.pak`(577 990) 都在，
  头 4 字节是 `05 00 00 00`（DataPack 格式版本 5），文件本身没坏。

下一步只有两条路，都不需要再猜：①拿到 chromium 156 的 dbgsym（或从 Debian
`chromium-dbgsym` 取），直接把 `0xa90c060` 的函数名与行号读出来；②反汇编
`0xa90c060..0xa90c230` 把被查的全局/资源 id 认出来，再比对 pak 里的 id 表。

#### R44：两个真根因 —— 资源文件版本不匹配、控制台队列野指针

**① 资源文件不是一个版本。** 之前那个 `0xa90c26a: je -> int3` 用反汇编 + 栈回溯
认了出来：函数是 `ui::ResourceBundle::GetLocalizedStringImpl(int)`（用 headless-shell
的符号表按"同形状"比对确认：`call GetRawDataResourceForScale` → `test %rdx,%rdx`
→ `je <冷块>` 三条指令逐字对应）。也就是说**本地化资源 id 查出来是空的**。

再把镜像里的资源文件和包里那份逐个比大小，问题一目了然：

| 文件 | 镜像里（错） | chromium 156 包（对） |
|---|---|---|
| resources.pak | 13 046 519 | **20 368 637** |
| locales/en-US.pak | 577 990 | **592 014** |
| icudtl.dat | 10 876 560 | **10 824 576** |
| chrome_100_percent.pak | 696 632 | **583 800** |
| snapshot_blob.bin | 365 488 | **374 096** |

pak 头还是 `05 00 00 00`（DataPack v5），所以"加载成功"，但 id 表对不上，
查不到就命中不变量。用新脚本 `scripts/install_guest_tree.sh` 把
`build/tmp/app-tree/usr/lib/chromium` 整棵（18 个文件 / 68 175 560 字节）写回镜像后，
**int3 崩溃消失**，客户端一路走到真正的窗口握手。

**② 控制台队列的野指针把内核打崩。** 修完 ① 之后立刻换了个死法：内核 panic，
`vec=0x0D (#GP) err=0 cr2=0`。`addr2line` 落到 `fb.c:push_to_queue`，
再反汇编认到具体一条：`0x103BE45 mov (%r8),%rax` —— r8 是数据源 `c`，
取值非规范（err=0 + cr2=0 正是非规范地址的特征）。

链条：`flanterm_fb_move_character` 里 `c = &q->c`，`q = ctx->map[i]`；
`map[i]` 是 `&queue[queue_i++]` 的快照。`push_to_queue` 里
**`queue_i` 无上界检查**，写满就写穿 `queue` 缓冲，堆一坏，map 里的快照就成
野指针，随后被当数据源解引用。

修法（`src/fb.c`）：加 `fb_map_at()` 统一取 map 项并校验指针落在 `queue`
区间内，越界就自愈置 NULL；`push_to_queue` 在 `queue_i` 到容量时丢这一格而不是
写穿。宁可丢一个字符，不要 panic。

修完 `PANIC` 计数 0，客户端走到：
```
req 20.1 xdg_surface    <- get_toplevel
req 21.3 xdg_toplevel   <- set_app_id
req 21.2 xdg_toplevel   <- set_title
req 21.7/21.8           <- set_min_size / set_max_size
req 16.6 surface        <- commit
req 20.3 xdg_surface    <- set_window_geometry
req 20.4 xdg_surface    <- ack_configure
```
xdg 握手全通，**只差 attach + 带 buffer 的 commit**。

#### R45：xdg 握手全通，卡在"没有 buffer 可 attach"

资源文件换成同版本之后，客户端把整套窗口握手走完了（服务端日志顺序）：

```
19.1 region / 18.4 surface      <- set_opaque_region
20.1 xdg_surface                <- get_toplevel
21.3 xdg_toplevel               <- set_app_id
21.2 xdg_toplevel               <- set_title
21.7 / 21.8 xdg_toplevel        <- set_min_size / set_max_size
16.6 surface                    <- commit（无 buffer 的首次 commit，协议要求）
20.3 xdg_surface                <- set_window_geometry
20.4 xdg_surface                <- ack_configure
```

`ack_configure` 说明 configure 已经送达并被接受 —— **合成器这一侧是通的**。
缺的是最后一步：`wl_surface.attach(buffer)` + 带 buffer 的 `commit`。
`wl_shm_pool.create_buffer`（op 0）始终没有出现，也就是说渲染端一个帧都没交上来。

两条渲染路都不通，各有各的日志：

- 默认路径：`Could not dlopen native EGL: libEGL.so.1: cannot open shared object file`。
  镜像里 `/usr/lib/x86_64-linux-gnu/libEGL.so` 是个 **47 字节的符号链接**，
  而 ANGLE 找的是 `libEGL.so.1` —— 名字对不上，且 HBOS 上也没有任何 GL 驱动，
  所以这条路本来就必然失败。
- `--disable-gpu`（纯软件合成）：握手同样走完，但在
  `0x9cfa0dd` 上是 `int3; ud2` 冷块，`run: exit 132`（SIGILL）。
  该冷块前面是一条 `call 0x2d94c50` —— 调用方按"不返回"编排，
  实际却返回了，于是落到 ud2。和 R42 的 `__stack_chk_fail` 是同一个形状，
  说明这类 glibc 终止路径在 HBOS 上还有没走通的地方。

下一步是**明确二选一**：要么把 SwiftShader（`libvk_swiftshader.so` +
`libvulkan.so.1` + `vk_swiftshader_icd.json`，这三样 app-tree 里都有）跑起来，
要么把 shm 软件合成的 `create_buffer` 路径接上。两条都不需要再猜协议。

#### R46：`madvise` 不该因为 advice 不认识就 EINVAL

给 `setenv tracepid <pid>` 加了一个"只报失败 syscall"的模式（全量每条都打，串口根本跑不动），
把浏览器进程的负返回抓出来看。绝大多数是正常的 ENOENT 探测
（`openat`/`newfstatat`/`access`/`readlink` 在找不存在的文件），但有一条一直在报：

```
[E] nr=28 (madvise) a2=0x66 / 0xffffffff / 0x12  ret=-22 (EINVAL)
```

`madvise` 是**建议性**调用，advice 不认识应当当 no-op 成功返回。原来的白名单只放到
0..3/14..17/20..23，于是 `MADV_WIPEONFORK(18)`（PartitionAlloc 给元数据区用的）、
以及 Chromium 实际会带的非标准值全被 EINVAL 掉 —— 拿到非 0 返回的调用方直接走 CHECK。
改成：只特殊处理 `MADV_DONTNEED(4)` / `MADV_FREE(8)`，其余一律成功返回。

修完之后同样参数跑一遍：**没有 PANIC、没有新的致命 trap**，客户端稳定停在
"xdg 握手完成但渲染端不交帧"这个状态（上一轮 R45 描述的同一个卡点），
中间只有早已存在的 pid=4 辅助进程 SIGSEGV 和几次干净 exit。

顺带把上一轮那个 `int3; ud2`（`exit 132`）重新定位了一次：冷块前面的
`call 0x2d94c50` 返回了，而调用方是按"不返回"编排的 —— 和 R42 的
`__stack_chk_fail` 同一个形状。这条线索还没走完。

#### R47：扩镜像失败 —— HBOS 挂不上更大的 ext2（这是个真 bug）

bilibili 解开要 465 MB，镜像只剩 356 MB，只能扩。结果扩完**必定内核 panic**，
而且连 `[PANIC] vec=` 都打不出来（panic 发生在控制台还不可用的阶段）。
一路二分下来：

| 镜像 | 块数 | 块组 | 结果 |
|---|---|---|---|
| 1 887 436 800（原） | 460 544 | 15 | 正常 |
| 2 056 257 536 | 501 760 | 16 | **panic** |
| 2 937 061 376 | 716 800 | 22 | **panic** |

排除了"磁盘写满"这条路：把 chromium-headless-shell 的 219 MB 二进制删掉
（空闲块 447 → 56 407）之后 1.96 GB 那份**照样 panic**。所以不是 ENOSPC，
是 ext2/AHCI 侧对大文件系统的处理问题。这条没修完 —— 是下一轮的第一优先级。

**代价**：原镜像被覆盖过一次，已按原尺寸（1 887 436 800 字节 / 460 544 块）
重建并用 e2fsck 修好，启动验证 `Shell ready`、PANIC 计数 0。

#### bilibili 装进去了，但 guest 里看不见

`dpkg-deb -x` 出 7 925 个文件 / 487 MB，用新的 `scripts/install_guest_tree.sh`
写进镜像 `/opt/apps/io.github.msojocs.bilibili/`（electron 二进制 219 MB）。
装完的问题：

- `debugfs` 能看到 `/opt/apps/io.github.msojocs.bilibili/files/bin/electron` 及其内容；
- guest 里 `ls /opt/apps` 输出为空，`cd` 该路径报 `not found`，`run` 报
  `run: app not found`（= `open()` 返回 ENOENT）。

也就是说**目录项写进去了、HBOS 的 VFS 却解析不到**。这条只到定位，没修完。

另外 `debugfs write` 建出来的文件是 0644，可执行文件必须补 x 位，
`install_guest_tree.sh` 已改成按源文件 mode 写 `sif ... mode 0100xxx`
（bilibili 里 7 个可执行文件已修）。

#### R48：bilibili 装不进去，卡在三个内核侧上限（都还没修）

这一轮把 bilibili 的 7 925 个文件真写进了镜像，然后一路撞墙，逐一记录：

**① 内核文件表是固定大小 `FS_MAX_FILES = 8192`**（`src/fs.h:21`，
`file_t files[FS_MAX_FILES]`）。启动日志直接写着 `fs: ext2 mounted, files=8192`，
紧接着 `[SELFTEST] FAIL open create`。镜像里已经有 11 万多个文件，
这个 8 192 的表就是硬顶。

**② inode 用光了**。`debugfs stats` 显示 `Inode count: 115200 / Free inodes: 0`。
原因是我把镜像从 1.88 GB 扩到 2.8 GB 再缩回来，`resize2fs` 缩容时把
inode 数从 168 960 砍到 115 200，而文件数没变。没有空 inode 之后
`write`/`symlink` 全部失败 —— 第一次安装看起来"成功"，其实
`/opt/apps/.../files/bin/electron` 里一个文件都没有。

**③ 镜像一扩容就内核 panic**（R47 已记）：块组 15 → 16 是分界，
1.96 GB / 2.8 GB 都直接 panic，连 `[PANIC] vec=` 都打不出来。
所以"扩镜像换空间"这条路当前是堵死的。

**回归证据**：把 bilibili 装进去之后，
`run /opt/chromium/usr/lib/chromium/chromium --version` 也变成
`run: app not found`（同样的路径之前能跑）。也就是说整个 `/opt` 下的
路径都解析不到了 —— 怀疑与 ① 的表满有关，但没有定论。

**当前状态（已回滚）**：bilibili 的 7 925 个文件已从镜像删掉，
`Free blocks: 132250`(517 MB) / `Free inodes: 915`，启动到 Shell、PANIC 0。
chromium 侧没有代码改动，仍是 R46 的状态。

下一步必须先修 ①（把文件表改成可增长/可淘汰，或至少提到能覆盖镜像规模），
然后才谈得上装 bilibili；②③ 是同一类"固定上限"问题，建议一并处理。

#### R49：文件表 8192 → 131072 —— chromium 恢复，并拿到下一个 FATAL 的准确坐标

R48 里那个"装完 bilibili 就整个 `/opt` 解析不到"的根因找到了：
`ext2_rebuild_files()` 把整棵树**平铺进静态数组** `file_t files[FS_MAX_FILES]`，
而 `FS_MAX_FILES` 只有 **8192**。镜像有 11 万个文件，表一满
`ext2_scan_cb` 就 `return 1` 停止遍历，后面的路径（`/opt/chromium`、`/opt/bili`）
根本没进表 —— 于是 `run` 报 `app not found`。之前能跑只是因为 `/opt` 恰好
落在前 8192 项里，加进 bilibili 的文件后就被挤出去了。

修法：把 `FS_MAX_FILES` 提到 **131072**（`file_t` 约 456 字节，约 57 MiB 静态
BSS；BSS 不进内核镜像，`hbos-bios.bin` 仍是 20.5 MB）。同时把查找/扫描循环的
上界从 `MAX_FILES` 改成 `fs.file_count`，避免每次查找扫满 13 万项；
目录改名用的两张祖先追踪表改成固定 16384 项并显式拒绝超规模改名。

> 走过的弯路：一开始把表改成"堆上按需增长"，结果 `fs: ext2 mounted, files=0`
> —— `src/core/heap.c` 的 `HEAP_POOL_SIZE` 只有 **2 MB** 静态池，
> 3.7 MB 的首次分配必然失败。静态 BSS 才是这里的正解。

**验证**：`fs: ext2 mounted, files=13340`（不再卡在 8192），
`run /opt/chromium/usr/lib/chromium/chromium --version` →
`Chromium 156.0.8078.73`。PANIC 0。

**下一个卡点（已拿到准确坐标）**：带 `--ozone-platform=wayland` 跑，
`run: exit 132`(SIGILL)，栈上残留的字符串是

```
FATAL:base/path_service.cc:264] Failed to get the path for 1001
```

Chromium 的路径常量里 1001 = **`DIR_CURRENT`**，也就是 `getcwd()` 失败。
所以下一步不是 wayland 协议问题，是 HBOS 的 `getcwd` 在这个进程里返回了错误。
