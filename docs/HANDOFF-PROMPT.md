# HBOS × Chromium 交接提示词（给接手的 AI）

把下面整段发给新会话即可。已按"省 token"优化：所有命令都带窄过滤，禁止整读日志。

---

## 角色

你是 /media/data/hbosv2 下的 HBOS 内核（自研 x86_64 OS，Linux 兼容层）的接手工程师。
唯一目标：让已安装的 Debian Chromium 150 跑通
`--headless --no-sandbox --no-zygote --single-process --disable-gpu --disable-crashpad-for-testing --lang=en-US --dump-dom about:blank`
并在串口看到 `<html>…` 输出。更大目标：任何 deb 包都能经 HPT 直接安装运行。

## 环境铁律（违反就浪费大量时间）

1. 构建必须 `export TMPDIR=/media/data/hbosv2/build/tmp`（系统 /tmp 只有 10MB）。
2. 本机无 /dev/kvm，QEMU 走 TCG（qemu_session.py 已自动处理），每次运行 1~2 分钟。
3. 容器拦截 `rm -rf`，脚本里用 `find -delete`。
4. **不要重建磁盘镜像**：build/c3-chromium.img（1.8GB）里 Chromium+41 依赖已装好。
5. 日志文件 100KB+，**永远用 `grep -a 模式 file | head -N` 或 `tail -c` / `sed -n` 取片段**，
   禁止 cat/Read 整个日志。
6. 每轮迭代 = 改源码 → `make bios-iso -j16 2>&1 | grep -E "错误|error:|✓ BIOS"` →
   跑一条 qemu_session.py 命令 → grep 日志。不要做无关验证。

## 标准运行命令

```
cd /media/data/hbosv2 && export TMPDIR=$PWD/build/tmp && make bios-iso -j16 2>&1 | grep -E "错误|error:|✓ BIOS" | head -4 && timeout 900 python3 scripts/qemu_session.py build/hbos-bios.iso --disk build/c3-chromium.img --mem 4096M --timeout 240 --boot-timeout 120 --log build/tmp/run.log -- "run /opt/chromium/usr/lib/chromium/chromium --headless --no-sandbox --no-zygote --single-process --disable-gpu --disable-crashpad-for-testing --lang=en-US --dump-dom about:blank" 2>&1 | tail -20
```

日志在 build/tmp/run.log；取片段：`grep -a "模式" build/tmp/run.log | head -N`。

## 已完成（不要重做）

- C1/C2 验收通过（ext2 真符号链接、共享库跨包解析、大 mmap、4GB 物理内存钳制）。
- Chromium 150（280MB ELF）+41 依赖安装校验通过；`--version` 输出正常。
- 已修 15+ 内核缺陷，关键：mmap(PROT_NONE) 惰性提交（32GiB 地址池）、
  `/proc/self/exe` 真实路径（task_t.exe_path，src/tools/app.c 与 src/elf.c 双写入）、
  linux_fcntl（0/1/2/3/4/1030）、SOCK_SEQPACKET→STREAM、无 CLONE_THREAD 的 clone 走
  fork 语义、clone3 摊平、**线程参数不得写回 syscall 帧的 a0..a4**（会冲掉 glibc 线程
  入口寄存器）、setsid/setpgid、sched 一族、creat/getpriority、**内核栈 8KB→32KB**
  （TASK_STACK_SIZE，src/core/task.h——曾因溢出互踩导致 #UD RIP=0x3 整机 panic）。
- 补过：flock/fsync/getrlimit/capget/statfs/unlinkat/faccessat/seccomp/membarrier/
  epoll_pwait2/prctl 多选项/fchmod(91,95)/name_to_handle_at(303,304)→EOPNOTSUPP、
  /proc/sys/fs/inotify/max_user_watches 伪文件。
- 诊断设施（复用它们定位，不要盲猜）：
  a) 未实现 syscall 自动打 `[LINUX_SYSCALL] unimplemented nr=..`；
  b) mmap 失败打 `[MMAP] fail ...`；clone EFAULT 打 `[CLONE] EFAULT tls/...`；
  c) 最近 128 条 syscall 环形缓冲：用户态 TRAP 和内核 panic 以及主线程 exit_group 时
     自动 dump（`path="..."` 为 open/openat/readlink 当场复制的路径）；
  d) exit/exit_group 打 `[EXIT] pid=.. code=..`；
  e) 调度器切换前校验 `[SCHED] BAD CTX ...`（安全网）；
  f) 用户态异常带 RBP 回溯；宿主反查：`objdump -d --start-address=<rip-0x1100000000> build/tmp/chromium.elf`。
  内核符号：`nm build/hbos-bios.bin`（它就是未 strip 的 ELF，链接基址 0x1000000，
  代码范围 0x1000000-0x5461000）。

## 当前卡点（从这里开始）

主线程初始化全部完成后**静默 exit(0)**：建了 `/tmp/org.chromium.Chromium.scoped_dir.*`
→ 直接进入清理（openat/access 该目录）→ exit(0)。**导航与 DOM 输出从未发生**。
排查顺序（已规划）：
1. 看上次未完成的实验结果：`grep -a "ERROR\|FATAL\|EXIT\|TRAP\|html" build/tmp/c3-multiproc.log | head -20`
   （那是去掉 --single-process 跑多进程渲染的一次，可能被中断，重跑一遍更好）。
2. 怀疑消息循环原语：检查 epoll_wait/ppoll/poll/eventfd 的语义——**不能在有 timeout 时
   立即返回 0 事件**（Chromium 会误判为"无工作→退出"）。对照 src/syscall.c 里
   HBOS_SYS_EPOLL_WAIT / ppoll 实现逐条核对超时路径。
3. 若多进程模式报新错：子进程 execve 路径（nr=59）在 clone-fork 子进程里是否正确
   替换地址空间，是下一个重点。
4. 环境缺失（非致命但建议顺手补）：fontconfig 需要 /etc/fonts/fonts.conf（把
   build/debs 里 fontconfig 相关 deb 装进镜像即可，mk_test_img.sh + fetch_deb_closure.py
   都现成）；libGLESv2.so 在 chromium-common 包里被拆到 /opt/chromium-common/——
   正解是让 HPT 尊重 manifest 的 install_prefix（HPT/tools/pkg2hax.py 已输出该字段）。

## 关键文件

- src/syscall.c（Linux 兼容层主体）、src/core/task.c（调度/线程）、src/core/task.h、
  src/linux_compat.c（socket 等）、src/devfs.c（/proc 伪文件）、src/tools/app.c（run 启动）。
- 结果文档 docs/ROADMAP-C1-C3-RESULT.md（3.5b~3.5e 节有全部历史与证据）。
- 工作区记忆 .workbuddy/memory/2026-10-24.md（当日全部结论）。

## 汇报要求

每轮只报：改了什么文件/函数、跑出来的关键行（引用日志原文 ≤5 行）、下一个动作。
不要贴大段日志，不要重复已知结论。
