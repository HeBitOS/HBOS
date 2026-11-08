# HBOS 性能基线体系（R32）

> 上一轮（R31）自己承认：性能这块"只做了一处，说实话不够"，根因是
> **拿不到有说服力的基线**——不想凭感觉改热路径然后说"优化了"。
>
> 本文补的就是这件事：先把"怎么量"做扎实（§1–§3），再用它定位热点
> （§4），最后基于实测数据给建议（§5）。**所有数字都是跑出来的**，
> 每条都标了数据来源和样本量；跑不出来的地方明确写"未测"。

配套：`docs/wiki/PERFORMANCE.md` 记录历史优化（启动时间、文件系统、inflate
等），本文只记录 R32 这一轮新增的测量能力与结论。

---

## 1. 这次改了什么（速查）

| 目标 | 命令 | 需要 QEMU | 说明 |
|---|---|---|---|
| 启动 + 负载基线 | `python3 scripts/perf_baseline.py --runs 3` | 是 | 修掉了失败值污染与计时段错误 |
| 统计口径自检 | `make perf-selftest` | 否 | 4 组断言，钉死"失败不进统计" |
| GUI 侧宿主基准 | `make gui-bench` | 否 | blit 帧成本 + PNG 解码吞吐 |
| syscall 热点（新采集） | 串口里 `setenv sysprof 1` → `sysprof` | 是 | 内核按 nr 聚合次数 + 累计 tsc |
| syscall 热点（旧日志） | `make sysprof-log LOG=...` | 否 | 从已有串口日志聚合 |
| 启动时间（更细） | `make bench-boot` | 是 | 已有，把固件/内核两段分开 |

---

## 2. `perf_baseline.py` 的审计与修复

### 2.1 审计结论：三个问题，两个真、一个假

| # | 疑点 | 结论 | 证据 |
|---|---|---|---|
| 1 | 失败返回 `-1.0` 会污染统计 | **真问题，但不是"混入 median"** | 原代码 `if w > 0: works.append(w)` 已把它过滤掉；真正的危害是**样本量被静默缩小** |
| 2 | 是否真取中位数 | **真** | `statistics.median(boots)` / `median(works)`，口径正确 |
| 3 | 冷/热缓存混淆 | **真问题** | 见 2.2 计时段错误 |
| 4 | 失败会被误当成"很快" | **假**（不会进 median），但**结果 JSON 会撒谎** | 见下 |

**问题 A（真）：`runs=3` 但可能只有 1 个样本参与计算。**

原代码把 `args.runs` 原样写进结果，失败样本被 `if w > 0` 静默丢弃：

```python
# 旧代码
result = {"boot_median_s": ..., "workload_median_s": ..., "runs": args.runs}
```

3 次里失败 2 次时，JSON 里仍是 `"runs": 3`，但 `workload_median_s` 其实是
**1 个样本**。读者会以为那是 3 次的中位数。复现：

```python
# 旧逻辑等价于
samples = [-1.0, -1.0, 10.1]
works = [w for w in samples if w > 0]      # -> [10.1]
statistics.median(works)                    # -> 10.1，而 runs 仍写 3
```

**问题 B（真）：`workload` 计时段包含了启动和 1 秒固定等待。**

旧的 `measure_workload` 用 `subprocess.run` 把整个 `qemu_session.py` 子进程
计时，而那个子进程里包含：

- QEMU 进程启动 + BIOS/引导器（约 3s，和 `boot` 指标重复计）
- 拿到 prompt 之后 `time.sleep(1.0)`（`qemu_session.py:94`，固定 1s）
- 才轮到真正的负载

所以历史上的 `workload=9.97s` **不是负载耗时**，是 `3s 启动 + 1s 等待 +
负载`。1s 的固定噪声足以淹没小改动带来的收益。

**问题 C（假）：`-1.0` 参与 median。** 不成立——旧代码过滤了。但"失败值
这个设计"确实是隐患：只要有人把 `if w > 0` 改成别的写法（或新增一个指标
忘了过滤），`-1.0` 立刻变成"很快"。所以这轮把它从**类型层面**去掉。

### 2.2 修复内容

1. `measure_boot` / `measure_workload` 的失败返回 **`None`**（不再是 `-1.0`），
   失败样本根本不进列表，不存在"过滤漏了"这条路径。
2. 计时段收窄：`measure_workload` 只取 `run /bin/wayland_surface` 那一条
   命令的墙钟。为此给 `qemu_session.py` 加了 `--timings`（**默认关闭**，
   不开时输出与原来逐字节相同，不影响其他调用方）。
3. 结果结构改成 `{"median_s", "n", "requested", "min_s", "max_s", "spread_s"}`，
   `requested` 与 `n` 分开报，丢样本藏不住了。
4. 新增 `--min-samples`（默认 2）：样本不足时**退出码非 0**，不给结论——
   1 个样本的"中位数"就是它自己，没有抗抖动意义。

### 2.3 修复验证（有证据）

`make perf-selftest` 跑 4 组断言，全部通过：

```
[PERF-BASELINE] 统计口径回归测试
 - summarize 的 n/median/spread
 - measure_workload 的失败哨兵
 - measure_workload 的计时段解析
 - CLI 无样本行为：跳过（加 --with-qemu 才跑，需独占 QEMU）

[PERF-BASELINE] all checks passed
```

其中"计时段解析"用假的 `qemu_session` 输出验证：给了
`cmd 12.500s: wayland --probe-input` 和 `cmd 3.250s: run /bin/wayland_surface`
两行时，必须取 **3.25s**（不是 12.5，也不是两者之和）。

> **注意（诚实标注）**：`--with-qemu` 那条（真启动一次 QEMU 验证 CLI 退出码）
> 本轮**没有跑**——QEMU 由调度者独占。见 §6。

---

## 3. 新增：GUI 侧客观量（`make gui-bench`）

### 3.1 为什么放宿主机

GUI 帧成本在 QEMU/TCG 下被两件事淹没：串口逐字符输出、以及 TCG 译码预热。
且 QEMU 要独占。所以把两条**纯计算**热路径搬到宿主机直接测，输入确定性、
无 I/O、可重复。

关键点：测的是**真实内核源码**，不是复制粘贴的替身——

- `gui_blit_argb8888` 来自 `src/tools/gui.c`（用 `-ffunction-sections` +
  `--gc-sections` 只保留被测函数所在的段，其余 291 个外部符号不进产物）
- `png_decode` 来自 `src/png.c` + `src/inflate.c`

### 3.2 实测数字（1280x720，200 帧 x 5 次取中位数）

宿主机：16 核 x86_64，load average ≈ 3.3，无 KVM 相关影响（纯宿主）。

| 项目 | 中位 | 区间 (min..max) | 吞吐 |
|---|---|---|---|
| 不透明帧（每行扫 alpha 后 memcpy） | **0.383 ms/帧** | 0.377..0.467 | 9624 MB/s |
| 含 alpha 帧（逐像素混合） | **2.681 ms/帧** | 2.617..2.694 | 1375 MB/s |
| 纯 memcpy 参照（同字节量） | 0.271 ms/帧 | 0.250..0.339 | 13582 MB/s |
| 去掉扫描的对照实现（逐行 memcpy） | 0.245 ms/帧 | — | — |

**alpha 扫描的净成本（交错 A/B）：0.123 ~ 0.172 ms/帧，占不透明帧 32%~46%。**

三次独立运行（同一二进制，间隔数秒）：

| 运行 | 不透明帧 | alpha 扫描净成本 | 占比 |
|---|---|---|---|
| 1 | 0.380 ms | 0.123 ms | 32.3% |
| 2 | 0.402 ms | 0.133 ms | 33.1% |
| 3 | 0.373 ms | 0.172 ms | 46.2% |

> 测量方法说明：最初用「blit 耗时 − 纯 memcpy 耗时」反推扫描成本，实测会
> 算出**负值**——宿主 memcpy 带宽本身在 7.9~17.6 GB/s 之间漂移，减法的噪声
> 比被测量还大。改成**交错 A/B**（同一轮里先测真实 blit、再测"去掉扫描"的
> 对照实现，逐轮取差再求中位数），漂移对两边影响近似相等，结果才稳定。
> 这个坑值得记住：**在共享机器上，任何跨时间块的减法都不可信。**

### 3.3 PNG 解码吞吐

输入：`tests/host/gen_gui_png.py` 用宿主 Python `zlib` 生成的 540x360 真彩
PNG（160128 B 压缩 → 583200 B RGB 输出）。zlib 是独立实现，所以这同时是
"能解开"和"解多快"。

| 指标 | 数值 |
|---|---|
| 单次解码 | **2.81 ~ 2.90 ms** |
| 输出吞吐 | **201 ~ 208 MB/s** |
| 输入吞吐 | **55 ~ 57 MB/s** |

三次运行 2.866 / 2.799 / 2.897 ms，抖动 < 4%，可重复。

---

## 4. syscall 热点统计

### 4.1 新增能力：内核按 nr 聚合（`setenv sysprof 1`）

对应 `docs/jindu.md` P3 第 10 条原话："建议加 `--vmodule` 级别的 syscall
计数统计（按 nr 聚合次数 + 累计耗时），而不是现在这种逐条打印"。

实现要点：

- **默认关闭**，关闭时每次调用只多一次 `if (!g_linux_prof_on)`，热路径不受影响。
- 打开后每条调用两次 `rdtsc`，累加到 `(count, ticks)` 槽位。
- 计时用 `rdtsc` 而非 PIT tick：PIT 是 100 Hz，单条调用几乎总是 0 tick，
  累加全是 0。rdtsc 的**绝对频率随宿主而变**，所以输出里给的是
  "该 nr 占本机总耗时的百分比" + 原始 tick；**跨机器比百分比，同机前后比 tick**。
- 表大小 512 项；`nr >= 512` 合并成一个桶（不合并的话这些调用会从统计里
  消失，总占比就对不上）。
- `sysprof` 命令打印累计耗时 top 15，含次数、tick、占比（两位小数——
  单个 nr 常常只有零点几个百分点，整数会全抹成 0）。

用法（串口会话里）：

```
setenv sysprof 1        # 打开并清零
<跑要测的负载>
sysprof                 # 出 top 15 表
sysprof reset           # 只清零，继续采
setenv sysprof 0        # 关掉（回到零开销）
```

### 4.2 从历史日志聚合（不需要 QEMU）

`scripts/syscall_profile.py` 把已有的 `--- last linux syscalls ---` 环形
缓冲 dump 聚合成排行。

**关键陷阱**：dump 是**快照**，一次崩溃会 dump 好几份，相邻快照重叠极多。
直接把全文件行求和 = 把同一批调用重复计 N 次（`build/tmp/*.log` 里
28955 条 dump 行，实际是 30+ 份快照）。所以脚本按 dump 头切段，**默认只
统计最大的那一段**。

### 4.3 真实 top 15（Chromium 场景）

数据来源：`build/tmp/r32-chromium2.log`（2026-10-31 20:02，4 段 dump，
取最大段 747 条调用）。复现：

```bash
make sysprof-log LOG=build/tmp/r32-chromium2.log
```

```
#   nr  syscall              count     pct  pids
     9  mmap                   209  27.98%  2,3,8,9
    10  mprotect               150  20.08%  2,3,8,9
   257  openat                  61   8.17%  2,3,8,9
     0  read                    55   7.36%  2,8,9
     5  fstat                   50   6.69%  2,3,8,9
     3  close                   49   6.56%  2,3,8,9
   186  gettid                  47   6.29%  2,3,8
    13  rt_sigaction            22   2.95%  2
   157  nr=157                  13   1.74%  2
   228  clock_gettime           12   1.61%  2
    14  rt_sigprocmask          10   1.34%  2,3
   318  getrandom                9   1.20%  2
    83  mkdir                    7   0.94%  2
   262  newfstatat               7   0.94%  2
    89  readlink                 5   0.67%  2
```

**重要限定**：这是 **dump 快照，只有次数，没有耗时**。所以它回答的是
"调用得最多的是什么"，**不能**回答"最耗 CPU 的是什么"——一个 `read` 阻塞
10ms 和一次 `mmap` 花 2us，在次数榜上看起来一样。真正的耗时排行需要
§4.1 的 `sysprof` 表，而那需要跑 QEMU（见 §6）。

### 4.4 交叉验证：不同日志是否给出同样的结论

| 日志 | 样本 | top 1 | top 2 | top 3 |
|---|---|---|---|---|
| `r32-chromium2.log` | 747 | mmap 27.98% | mprotect 20.08% | openat 8.17% |
| `chr10.log` | 512 | mprotect 28.71% | mmap 20.12% | gettid 8.79% |
| `chr3.log` | 512 | mprotect 28.71% | mmap 19.92% | gettid 8.98% |

三份独立的 Chromium 日志里，**mmap + mprotect 稳定占 48%~49%**，排名
只是二者互换。这不是偶然，是 Chromium 的 loader/分配器行为在 HBOS 上的
稳定表现。

进一步按 pid 拆（`r32-chromium2.log` 最大段）：

```
pid 分布: {2: 520, 3: 19, 9: 109, 8: 99}
  pid=2 top: mprotect 146, mmap 106, gettid 41, openat 36
  pid=8 top: mmap 47, read 13, openat 11, fstat 10
  pid=9 top: mmap 55, openat 13, read 13, fstat 13
```

pid=2（主进程）一个人占了 520/747 = **70%** 的调用，且集中在
`mprotect`+`mmap`。另两个子进程（8/9）是 mmap 为主的映射密集型。

还验证了 mmap 的分布形态：747 条里 209 条 mmap，分成 **52 段连续爆发**
（最大一段 12 条），从段首一直散布到段尾。说明它是**周期性**行为（每轮
分配都走一遍 mmap/mprotect），不是一次性的启动开销——这类热点优化一次
就能持续受益。

### 4.5 一个反例：Wayland 测试场景的热点是 `read`

```
perf-work.log (512 条):  read 507 = 99.02%
wl-proto-test.log (512 条): read 508 = 99.22%
```

这两份日志的 top 1 是压倒性的 `read`。看 `perf-work.log` 的尾部可以发现
大量 `nr=0 ret=-11`（EAGAIN）——那是**非阻塞轮询**，不是真在读数据。

这条数据的价值是提醒：**"次数多"不等于"值得优化"**。EAGAIN 轮询每次
开销极小，但次数巨大；真要优化它，方向是"减少轮询次数/加阻塞等待"，
而不是"把 read 实现写快一点"。所以 §5 的建议里，我没有把 read 排在前面。

---

## 5. 优化建议清单（按数据支撑排序）

### 5.1 值得做

**① `gui_blit_argb8888` 的每行 alpha 扫描 → 缓存"该 buffer 是否曾出现透明像素"**

- 数据：扫描净成本 **0.123~0.172 ms/帧**，占不透明帧 **32%~46%**（交错 A/B，
  3 次独立运行一致）。不透明帧 0.383 ms/帧，其中约三分之一到一半纯粹用于
  "扫一遍发现全是不透明、然后还是得 memcpy"。
- 为什么值得：这是**每帧每 surface** 都走的路径（`wl_composite_surface`
  每帧调一次 `gui_blit_argb8888`）。浏览器/终端窗口的常态就是全不透明。
- 怎么改（不改语义）：在 `wl_shm_buffer_t` 上加一个三态标记
  （未知/全不透明/曾出现透明）。全不透明时跳过整行扫描直接 memcpy；
  首次仍扫一遍以确定状态。**注意**：客户端可以复用同一块 buffer 写不同
  内容，所以标记必须在 buffer 内容变化（commit）时重置为"未知"——
  这是正确性关键，不是性能细节。
- 收益上限：省掉 0.12~0.17 ms/帧，即不透明帧从 0.383 → 约 0.22~0.26 ms。

**② Chromium 场景下的串口输出本身（稳定后彻底关掉）**

- 数据：本次统计的 28955 条 dump 行、以及 `[LINUX_SYSCALL]`/`[T]`/`[FDERR]`
  等逐条打印，全部经 `console_putchar` → 串口逐字符模拟。`src/syscall.h`
  里 `LINUX_DEBUG_TRACE_RESUME` 的注释已经踩过这个坑：
  "串口日志在 QEMU 里是逐字符模拟的，热路径上白打日志足以把 zygote 的
  5 秒握手拖超时"。
- 这是**间接证据**（定性明确、没有精确数字）：本轮没有量化"关掉串口能省
  多少秒"，因为那需要跑 QEMU 的 A/B。**列入待测**（§6）。
- 值得做的理由：它不是"优化某个函数"，而是**消除测量本身对被测系统的
  干扰**——不解决它，后面所有 QEMU 内的性能数字都不可信。

**③ 用 `sysprof` 拿到真正的耗时排行，再决定要不要动 `mmap`/`mprotect`**

- 数据：mmap+mprotect 占 **48%** 的调用次数（三份日志一致），且 70% 集中在
  主进程 pid=2，呈 52 段周期性爆发。
- **但次数占比 ≠ 耗时占比**。mmap 可能每次只是几十微秒的页表操作，也可能
  因为物理页分配/清零而很贵。**在拿到 `sysprof` 的 tsc 排行之前，我不建议
  动它**——这正是上一轮"凭感觉改热路径"要避免的。
- 这一条本身不是"优化"，是**下一步优化的前置条件**。

### 5.2 已验证：不值得做 / 已经做了

**④ `wl_buffer_read` 整块快速路径 —— 已做，不要再动**

`src/gui/wayland/iface/wl_iface_state.c:95` 里已经有快速路径（整宽 +
stride 一致时一次 `memfd_read_at`）。合成器实际就是这么调的
（`wl_backend_hive.c:100` 传 `x=0, w=width, dst_stride=width*4`），所以
每帧都命中。`docs/jindu.md` 把它列为"已做"是对的。

**⑤ PNG 解码 —— 目前不是瓶颈**

- 数据：2.81~2.90 ms/次（540x360），输出 201~208 MB/s。
- 一个 540x360 的图解码约 2.8ms。对比：一次 blit 1280x720 是 0.38ms。
  也就是说解一张这样的图 ≈ 7 帧 blit 的时间。
- 但网页上图片是**一次解码、多帧复用**的，不在每帧热路径上。除非有证据
  表明"打开页面时卡在解码"，否则不值得优化。**结论：不动。**

**⑥ `read` 的 EAGAIN 轮询（99% 次数占比）—— 不建议按"写快 read"的方向做**

见 §4.5。次数巨大但单次成本极低，方向应是减少轮询次数，而不是优化实现。

### 5.3 一句话结论

> 当前最值得优化的 3 个点：
> 1. **`gui_blit_argb8888` 的每行 alpha 扫描** —— 0.123~0.172 ms/帧、
>    占不透明帧 32%~46%，且每帧每 surface 都走。
> 2. **Chromium 场景的串口输出** —— 定性明确（逐字符模拟，足以拖超时），
>    但具体秒数待 QEMU A/B 量化。
> 3. **（前置条件，非优化）用 `sysprof` 把 mmap/mprotect 的 48% 次数占比
>    换算成耗时占比** —— 在拿到 tsc 数据前不要动它们。

---

## 6. 还没做的：需要 QEMU 的部分

QEMU 由调度者独占（本机无 KVM，只能单实例串行）。以下**代码已完成并编译
通过，但运行时验证未做**：

| # | 要验证什么 | 跑几次 | 每次多久 | 说明 |
|---|---|---|---|---|
| Q1 | `sysprof` 端到端可用 + 拿到**耗时** top15 | 1 | ~5 min | 打开统计、跑 Chromium 到稳定、`sysprof` 出表 |
| Q2 | `perf_baseline.py` 修复后的真实数字 | 3 | ~5 min | `--runs 3`，同时验证新 JSON 结构 |
| Q3 | 串口输出对 Chromium 启动的影响（建议②） | 2 | ~5 min | A/B：开/关串口日志，比启动到稳定耗时 |
| Q4 | `--with-qemu` 的 CLI 无样本断言 | 1 | ~1 min | 用假 ISO 跑，验证退出码非 0 |

合计 **7 次 QEMU，约 31 分钟**（串行）。

已完成的静态验证（不需要 QEMU）：

- `make build/hbos-bios.iso` 全量编译通过（`syscall.o`/`app.o` 无新增警告）
- `nm build/syscall.o` 确认 `linux_syscall_prof_{set,reset,dump,enabled}` 已链接
- `nm build/tools/app.o` 确认 `cmd_sysprof` 存在
- `make test`（宿主测试套件）全过

---

## 7. 怎么复现（完整清单）

```bash
# ── 不需要 QEMU ──────────────────────────────────────────
make perf-selftest          # 统计口径回归（4 组断言）
make gui-bench              # GUI 宿主基准（blit + PNG）
make gui-bench FRAMES=500   # 更多帧，更稳

# syscall 热点：从已有日志聚合
make sysprof-log LOG=build/tmp/r32-chromium2.log TOP=15
# 或直接
python3 scripts/syscall_profile.py build/tmp/*.log --top 15
python3 scripts/syscall_profile.py build/tmp/r32-chromium2.log --sections all

# 生成确定性 PNG 输入（gui-bench 内部会调，也可单独跑）
python3 tests/host/gen_gui_png.py build/tmp/bench-gui/in.png 540 360

# ── 需要 QEMU（先申请）──────────────────────────────────
# 基线（boot + 负载），3 次取中位数
python3 scripts/perf_baseline.py --runs 3 --json build/tmp/perf.json

# syscall 耗时排行（交互式，串口）
TMPDIR=$PWD/build/tmp python3 scripts/qemu_session.py build/hbos-bios.iso \
    --mem 2048 --timeout 300 --disk build/c3-chromium.img -- \
    "setenv sysprof 1" \
    "run /opt/chromium/usr/lib/chromium/chromium --ozone-platform=wayland \
     --no-sandbox --no-zygote --single-process --disable-gpu about:blank" \
    "sysprof"
```

---

## 8. 文件清单

新增：

| 文件 | 作用 |
|---|---|
| `scripts/syscall_profile.py` | 从串口日志聚合 syscall 排行（含快照去重） |
| `scripts/bench_gui.sh` | GUI 宿主基准的构建+运行入口 |
| `tests/host/bench_gui.c` | blit 帧成本（交错 A/B）+ PNG 解码吞吐 |
| `tests/host/gen_gui_png.py` | 确定性 PNG 输入生成（宿主 zlib） |
| `tests/host/test_perf_baseline.py` | `perf_baseline.py` 统计口径回归 |
| `docs/PERF-BASELINE-R32.md` | 本文 |

修改：

| 文件 | 改动 |
|---|---|
| `scripts/perf_baseline.py` | 失败返回 `None`、计时段收窄、结果含 `n`/`spread`、`--min-samples` |
| `scripts/qemu_session.py` | 新增 `--timings`（默认关闭，输出不变） |
| `src/syscall.c` | 按 nr 聚合的计数+tsc 统计（默认关闭） |
| `src/syscall.h` | `linux_syscall_prof_*` 声明 |
| `src/tools/app.c` | `setenv sysprof 0\|1` 开关 + `sysprof [reset]` 命令 |
| `Makefile` | `gui-bench` / `perf-selftest` / `sysprof-log` 三个目标 |
