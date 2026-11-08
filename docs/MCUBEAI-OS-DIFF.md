# MCubeAI-OS 对标分析（HBOS vs MCubeAIOS）

> 外部对标文档。对象是 GitHub 上的开源 x86_64 实验操作系统 **MCubeAIOS**
> （`MCubeHub/MCubeAI-OS`），自我定位为「从底层兼容 AI 的操作系统」。
> 本文的目标不是介绍它，而是回答一个问题：**它的「AI 原生」做法里，
> 有没有能让 HBOS 少走弯路的东西？**
>
> 分析日期：2026-11-08 · 对应 HBOS `v0.2.1-pre1`（`version.mk`）
>
> 阅读约定：**【原文】** = 我实际读到/抓到的文本，附 URL 或 `文件:行号`；
> **【推断】** = 我基于原文的推理，未经运行验证；**【外部报告】** = 第三方
> 分析结论，我未复现，仅作为线索引用。

---

## 结论摘要

1. **它不是「AI 原生内核」，而是「用户态 AI 守护进程 + 内核管道」**。与 AI 相关的
   内核代码只有 **226 行**（`kernel/ai/ai_chan.c` 205 行 + `kernel/ai/ai_core.c`
   21 行），内核只当管道中介和审计记账员，**不做任何推理**。「AI 原生」是定位
   口径，不是内核架构事实 【原文】`kernel/ai/ai_core.c:1-11`。

2. **值得 HBOS 抄的不是推理，是三处接口设计**：fd 对会话通道、内核审计环、
   `CONFIRM` 权限确认。这三样本质上都在回答「AI 动了系统，凭什么信它」，
   而 HBOS 目前**零 AI 代码**（`grep -rniE 'llm|inference|gguf|aiosd|ai_agent' src/`
   无命中），属于纯增量、不冲突。

3. **它的模型是玩具**：256 维字节级 bigram，embedding 是单位阵，无 attention、
   无 FFN，语料是硬编码字符串 【原文】`tools/gen_tiny_model.py:101-106,142-147`。
   所以它的工具路由被迫用**规则匹配**（`ai_route`），因为玩具模型发不出可靠的
   function call 【原文】`ai/README.md`。这恰好证明：**没有真模型时，工具层
   必须自己写规则**。

4. **它与 HBOS 不构成竞争，处在一条路的另一端**。它用自定义 `LGY` 容器
   （内核「never parses ELF at exec time」）、自研 freestanding libc、FAT32 8.3 名；
   HBOS 有 PT_INTERP 动态链接（`src/elf.c:326,340`）、ext2、131072 项文件表
   （`src/fs.h:24`）、真 Wayland 服务端。**HBOS 在「跑 Debian Chromium」这条
   路上领先它一个数量级**，反过来它在 AI 这一格从 0 到 1 走过一遍。

5. **对当前卡点零帮助**。HBOS 卡在 R49：`getcwd()` 在 Chromium 里失败
   （`docs/jindu.md` R49 节）。MCubeAI-OS 与这条线毫无交集。**它的正确用法是
   「将来做 AI 时的避坑清单」，不是「现在该抄的代码」。** 唯一建议现在做的
   是零代码成本的验收矩阵（见 C1）。

---

## 0. 抓取方法与元信息

| 项 | 值 |
|---|---|
| 仓库 | https://github.com/MCubeHub/MCubeAI-OS |
| 许可 | **MIT**（`LICENSE`，API `license.spdx_id = MIT`） |
| 创建 / 最后推送 | **2026-10-23T14:45:21Z** / **2026-11-04T16:07:57Z** |
| 所属组织 | `MCubeHub`，创建于 **2026-10-07**，公有仓库 2 个（另一个是 C# 的 `MCubeFlash`「MCube装机」） |
| 热度 | **2 star / 0 fork / 2 watcher / 1 open issue** |
| 体积 | 549 KB（API `size`）；实际 190 个树条目 / 161 个 blob / 1,785,899 字节 |
| 语言 | C **91.3%**（1,566,540 B）、Python 42,743、Assembly 40,250、Shell 19,416、Makefile 17,636、Batchfile 14,401、PowerShell 13,247、Linker Script 1,626 |
| 默认分支 | **`master`**（不是 `main`；按 `main` 请求 tree 会 404） |
| Release / Tag | **无**（`/releases` 与 `/tags` 均返回 `[]`） |
| 抓取方式 | `api.github.com` REST + `raw.githubusercontent.com` **本机直连可用**，无需镜像/代理 |

来源：`https://api.github.com/repos/MCubeHub/MCubeAI-OS`、
`/languages`、`/orgs/MCubeHub`、`/git/trees/master?recursive=1`、`/releases`、`/tags`。

**必须先说的一条元信息发现：它的提交历史不是增量开发史。**

抓到的最近 15 条提交中，里程碑提交 M2→M9 全部集中在 **2026-10-23 15:18 →
2026-10-24 14:27**（约 23 小时），其后 6 条全是 `auto: 同步 <时间戳>` 形式的
自动同步提交 【原文】`https://api.github.com/repos/MCubeHub/MCubeAI-OS/commits`。
配合仓库里同时存在 `build.bat`（Windows 侧驱动）、`tools/autosync.ps1`、
`tools/install-autosync.ps1`，以及 README 里「本项目位于 SMB 挂载的
`Z:\MCubeAIOS`」这句话 【原文】`README.md`，可以判断：

> 这是一个**在 Windows + WSL2 上离线开发、靠自动同步脚本批量推送**的项目
> 【推断】。所以「commit 数 / 日更」不能用来衡量它的工程成熟度，
> 反过来也别用「一天提交完 M2–M9」质疑它没写过代码——代码量是真的。

---

## 1. 对方项目定位与架构摘要

### 1.1 一句话定位

**单体内核 + 用户态 AI 守护进程的 x86_64 实验 OS**，按 M0–M9 里程碑推进，
当前自称 **M9 基线完成（AI 原生层）**。它**不是**微内核、**不是** ChromeOS/AOSP
派生、**不是** Linux 发行版，也不是所谓「Agent 运行时」——它是一个从零写的
内核，外加一个叫 `aiosd` 的用户态守护进程 【原文】`README.md`。

```
                 ┌─────────────────────────────────────────────┐
  用户态         │ coreutils · shell · GUI · 桌面应用           │
                 │ aiosd（推理/上下文/记忆/工具/访问控制）       │
                 ├─────────────────────────────────────────────┤
  系统服务       │ VFS · FAT32/tmpfs · AHCI · TTY · 网络协议栈   │
                 ├─────────────────────────────────────────────┤
  内核           │ 调度 · 虚拟内存 · 中断/APIC · 同步 · 系统调用 │
                 ├─────────────────────────────────────────────┤
  引导           │ UEFI loader · Boot Info Block · 早期控制台    │
                 └─────────────────────────────────────────────┘
```

上图逐字取自 `README.md`。注意它自己把 `aiosd` 画在**用户态**那一栏——
这一点比它的自我宣传诚实，也是本文最重要的一条对照依据。

### 1.2 它不是 Linux：LGY 容器 + 自研 libc

这是与 HBOS 最根本的分歧点，必须先钉死：

> `A .lgy file is a self-describing image: a fixed header followed by a
> segment table and the segment payload. The kernel maps the segments
> directly; **it never parses ELF at exec time**.`
> 【原文】`include/mcube/lgy.h:2-6`

`kernel/user/process.c` 里对应的是 `user_load_lgy()`（`lgy_has_magic()` 校验
魔数 `LGY\x1A` 后按段表映射），而 `user/README.md` 明写「内核启动后 PID 1 从
内嵌 LGY 运行」【原文】`kernel/user/process.c:154-169`、`include/mcube/lgy.h:22-26`。

它有 Linux 的**系统调用号**（`read/write/open/...`、`fork(57)/execve(59)/
wait4(61)/pipe(22)/dup2(33)/chdir(80)/getcwd(79)/brk(12)` 均「与 Linux x86_64
对齐」）【原文】`docs/architecture.md` §5 —— 但那只服务于**自己的** freestanding
libc。它没有 glibc、没有 `ld.so`、没有动态链接、文件系统是 FAT32 且只解析
**8.3 短名**（「the FAT32 driver does not parse LFN entries yet」）
【原文】`ai/README.md`。

> **对照 HBOS**：HBOS 的全部战略价值就在 ELF + `PT_INTERP` 动态链接 +
> ext2 + 真 glibc 二进制。**这一格 MCubeAI-OS 是负分项，不是可借鉴项。**

### 1.3 AI 分层：内核只有 226 行

`kernel/ai/` 目录只有两个文件，共 226 行：

| 文件 | 行数 | 职责 |
|---|---|---|
| `kernel/ai/ai_chan.c` | 205 | 连接撮合（两条匿名管道）、守护进程注册、64 项审计环 |
| `kernel/ai/ai_core.c` | 21 | 只在启动时打印一行「syscall 段 0x400..0x417 已保留」 |

`ai_core.c` 的原注释把边界讲得非常清楚：

> `The inference engine, context/memory managers and agent scheduler run in
> the "aiosd" user-space daemon (M6). The kernel side only owns: the reserved
> ai_* syscall range, a shared-memory arena descriptor used for token
> streaming, audit hooks for tool invocations.`
> 【原文】`kernel/ai/ai_core.c:1-13`

顺带一个可验证的**文档漂移**：这段注释说的「shared-memory arena descriptor
used for token streaming」在 `ai_chan.c` 里并不存在——实际是两条匿名管道 +
`ai_msg_t` 定长消息 【原文】`kernel/ai/ai_chan.c:1-8`。注释比代码更"先进"。

用户态那一侧才是主体，合计 **2,227 行**：

| 文件 | 行数 | 职责 |
|---|---|---|
| `ai/ai_engine.c` | 720 | GGUF v3 解析、llama 风格前向、采样 |
| `ai/ai_agent.c` | 571 | 规则意图路由、9 个内置工具、会话记忆 |
| `user/programs/aiosd.c` | 358 | 守护进程：模型加载、请求循环、确认、流式 |
| `user/programs/ai.c` | 255 | CLI：单轮 / chat / exec / status / tools / audit / memory |
| `include/mcube/ai.h` | 97 | 通道协议与审计结构体的共享 ABI |

### 1.4 规模与覆盖面

| 指标 | 值 | 说明 |
|---|---|---|
| 树条目 / blob | 190 / 161 | `git/trees/master?recursive=1`，未截断 |
| C/H/asm 文件 | 125 个 / **1,606,790 字节** | 含 2 个生成字体 |
| 剔除生成字体后 | 123 个 / **1,246,685 字节** | `kernel/font/ui_font.c` 332,862 B、`font8x16.c` 27,243 B 是生成的 |
| 文档 | **4 篇 / 49,026 字节** | `architecture` / `ai-native` / `roadmap` / `acceptance` |
| 最大单文件 | `user/programs/desktop.c` **173,926 B** | 自绘合成器 + 窗口管理 + 7 个应用入口 |

覆盖面确实宽：ACPI + **AML 解释器**（`kernel/acpi/aml.c` 29,731 B）、USB xHCI/EHCI/UHCI
三套主机控制器（合计 106 KB）、AHCI/ATA、e1000 + ARP/IPv4/ICMP/UDP/DNS、
fat32 + vfs + tmpfs/devfs、pipe、SMP/percpu、slab/pmm/vmm。

**但要区分「宽」和「深」**：它的 USB 栈 4 个文件合计约 104 KB，而 HBOS 的 `src/linux_compat.c`
单文件就是 105,977 字节；它的**整个内核系统调用分发** `kernel/arch/x86_64/syscall.c`
是 38,328 字节，而 HBOS 的 `src/syscall.c` 是 244,133 字节。二者的复杂度
不在同一量级，**但方向不同：它横向铺硬件，HBOS 纵向抠 ABI 兼容**。

### 1.5 工程侧：这块是它真正值得学的地方

比代码更值得注意的是它的**验收纪律**。`docs/acceptance.md`（10,195 B）是一张
逐条带状态的矩阵，A-01 到 A-22 每条都诚实标注 `通过 / 部分 / 待实现 / 手工`：

| ID | 项目 | 通过条件（节选） | 状态 |
|---|---|---|---|
| A-01 | 模型加载 | GGUF Q4/Q5/Q8 加载成功，损坏文件被拒绝 | **部分**：只支持 F32 / 256 字节词表 |
| A-03 | 性能 | Qwen2.5-1.5B Q4_K_M 在 4 vCPU QEMU > 3 tok/s | **待实现**：尚无量化大模型 |
| A-04 | 流式输出 | 首 token < 2 s，逐 token 渲染 | **通过** |
| A-11 | 工具调用 | 正确选择工具、校验参数、回填并继续推理 | **通过** |
| A-13 | 权限确认 | 弹确认，拒绝后不执行，审计记录完整 | **通过（手工）** |
| A-16 | 审计 | 每次调用都有 uid/工具/参数摘要/结果/耗时 | **部分**：64 项环形，只有 kind/pid/text |
| A-17 | `?` 命令 | 自然语言转系统操作并经确认执行 | **待实现** |

【原文】`docs/acceptance.md` 第 76–105 行（第六节起于 106 行）

> 这条对 HBOS 的价值很高：HBOS 的 `docs/jindu.md` 是 1,147 行**叙述式日志**
> （R1–R49），信息密度高但**不可判定**——没有一个地方能一眼看出「哪些能力
> 已验证、哪些是猜的、哪些回滚了」。MCubeAI-OS 用一张表解决了这件事。

---

## 2. AI 到底是怎么落进操作系统的（核心章）

这是它和 HBOS/OpenCubeOS 最不一样的地方，也是本文的主要挖掘对象。
**结论先说：它的 AI 没有被「落进操作系统」，而是被「挂在操作系统旁边」**，
挂法有三处设计值得看。

### 2.1 会话通道：两条匿名管道 + 定长消息

> `a connection is two anonymous pipes (request: client -> daemon, response:
> daemon -> client), so the existing read/write/poll/close paths work unchanged.`
> 【原文】`include/mcube/ai.h:2-7`

```c
typedef struct PACKED ai_msg {
    u32 type;      /* 请求 1-9 / 事件 64-70 */
    u32 session;
    u32 flags;
    u32 len;
    char data[AI_MSG_DATA];   /* 512 */
} ai_msg_t;                   /* 共 528 字节 */
```
【原文】`include/mcube/ai.h:48-54`（`AI_MSG_DATA` = 512，见 `:27`）

内核实现 `ai_chan_connect()` 做四件事：建两条管道 → 塞进 16 项 pending 队列 →
分配自增 session 号 → 把四个端交给调用者 【原文】`kernel/ai/ai_chan.c:1-8,53-96`。
客户端拿到的是**固定的 fd 3（请求写）/ fd 4（响应读）**。

**这套设计有三个明显优点，HBOS 可以直接抄：**

- **零新协议栈**：内核不解析 prompt、不解析 JSON、不解析模型文件。管道语义
  已经存在，`poll`/`close` 无需改动，代价极低。
- **天然背压与流式**：一个 token 一条 `AI_EVT_TOKEN` 消息，客户端逐条 read，
  不需要额外缓冲管理。
- **与 GUI 通道同构**：它的桌面协议也是「fd 3 命令 / fd 4 事件」
  【原文】`user/README.md`「`gui.c` 桌面协议客户端（fd 3 命令 / fd 4 事件）」。
  同一个 OS 里 UI 与 AI 用同一种通道形状，**心智负担为零**。

> **但 fd 3/4 这个具体选择 HBOS 不能照抄**【推断】：HBOS 的目标负载是
> Chromium，一个进程会开成百上千个 fd，3/4 随时被占。正确做法是让
> `SYS_AI_CONNECT` **返回两个动态分配的 fd**（语义等同 `socketpair`），
> 把「哪两个 fd」交给调用者，而不是硬编码。

### 2.2 内核审计环：它唯一有架构价值的部件

这是我认为**整个项目里最值得 HBOS 抄的一处**。`kernel/ai/ai_chan.c` 里：

```c
/* Only the daemon that has accepted a connection may append audit events, so
 * an ordinary client cannot forge tool decisions in the kernel ring. */
int ai_chan_audit(u32 kind, const char *text) {
    task_t *t = current_task();
    if (!t || g_daemon_pid == 0 || g_daemon_pid != t->pid) return -EPERM;
    ai_audit_log(kind, text);
    return 0;
}
```
【原文】`kernel/ai/ai_chan.c:156-163`

设计意图非常清楚：**AI 工具的每一次调用/拒绝，都写进内核的环形缓冲，
普通客户端无权写，因此不能伪造"我已获授权"的记录**。审计条目
`{ts, pid, kind, text[64]}` 通过 `SYS_AI_AUDIT` 只读回放
（`AI_AUDIT_RING = 64`）【原文】`include/mcube/ai.h:65-70` 与 `:72`。

**为什么这对 HBOS 有意义**：HBOS 迟早要让某个东西（AI、自动化、脚本）去
调用高权限 syscall。一旦如此，"谁在什么授权下做了什么" 必须有一个**内核侧
不可绕过**的记账点。把这个锚点放在内核（而不是守护进程自己的日志文件）是正确的
架构直觉——因为守护进程可以被替换，内核不能。

**但这个实现本身是坏的，不能整体照抄**：见 §2.5 与 §8。

### 2.3 工具层与权限确认

9 个内置工具，全部在 `aiosd` 进程内以**自身完整权限**执行，没有子进程、没有沙箱：

> `sys.info`, `sys.time`, `sys.disk`, `proc.list`, `fs.list`, `fs.read`,
> `fs.write` (confirmation required), `fs.search`, `calc.eval`. The router is
> **rule based** (`ai_route`) because **the tiny model cannot emit reliable
> function calls**. Tool output is capped at 2 KiB per call.
> 【原文】`ai/README.md`

确认流程：`aiosd` 发现 `tool->needs_confirm`，向**同一个客户端**发
`AI_EVT_CONFIRM`，再从**同一个客户端的请求管道**读 `AI_REQ_CONFIRM`，
只看 `flags & 1` 【原文】`ai_agent.c` / `aiosd.c` 交互，及第三方报告 §4.2 的
行号引用 `aiosd.c:186-201`。

> **这才是「AI 落进操作系统」的真问题所在**：不是推理跑在哪，而是
> **权限边界画在哪**。它画在了「同一个进程自问自答」上，等于没有边界。

### 2.4 推理核心的真相：一个玩具

`tools/gen_tiny_model.py` 生成的 `build/tiny.gguf` 是**合法 GGUF v3**，但内容是：

```python
vocab   = 256      # 字节级词表：token id == 字节值
n_embd  = 256
n_layer = 1
n_head  = 4
n_ff    = 256
# token_embd.weight  = 单位阵（token i -> one-hot e_i）
# output_norm.weight = 全 1
# output.weight      = log P(next byte | current byte)   ← 二元语法
```
【原文】`tools/gen_tiny_model.py:101-106,142-147`

即：**没有 attention、没有 FFN、没有多层**，实际执行路径是
`embedding → RMSNorm → lm_head → sampler`，等价于一个**查表式的字符二元模型**，
语料是脚本里硬编码的一小段英文 【原文】`ai/README.md`、
`tools/gen_tiny_model.py:5-21,49`。

这一点非常重要，它把很多"看起来是 AI 能力"的东西还原成了工程事实：

| 看起来 | 实际是 |
|---|---|
| 「本地推理」 | 一次 256×256 矩阵乘 + 采样，几微秒 |
| 「流式输出」 | `AI_EVT_TOKEN` 逐条写管道 |
| 「9 个工具」 | 字符串关键词匹配（`ai_route`） |
| 「上下文」 | 内存里最近 8 轮原文滑窗（`AI_SESSION_TURNS 8`） |
| 「记忆」 | 同上，无落盘、无向量、无 LRU-K |

而它的目标设计文档写的是 Qwen2.5-1.5B + Q4_K_M + KV cache + 向量检索——
**这份落差见下一节。**

### 2.5 目标设计 vs 实现：一份可直接复用的规格书

`docs/ai-native.md`（12,201 字节）描述了一套相当完整的 AI 子系统，
而它自己在开头就承认了落差：

> `M9 基线说明：本文描述目标架构。当前实现以两条匿名管道代替
> /run/aiosd.sock …… 推理为非驻留线程的同步单请求循环；GGUF 仅支持 F32、
> 字节级词表与 top-k/温度采样；工具为规则路由而非模型 function call；
> 记忆是内存内 8 轮滑窗。`
> 【原文】`docs/ai-native.md:6-10`

逐项对照（左列为设计文档章节，右列为 `docs/acceptance.md` 自评状态）：

| 设计项（`ai-native.md`） | 实现现状 | 状态来源 |
|---|---|---|
| §2.1 量化 Q4_0/Q4_K/Q5_K/Q8_0 | 仅 F32 | A-01「部分」 |
| §2.1 mmap 权重、按层换出 | 未实现 | A-01 |
| §2.2 完整 llama 前向（RoPE/GQA/SwiGLU） | 模型无 attn/ffn 张量，走退化路径 | A-02「部分」 |
| §2.2 `struct ai_backend_ops` GPU 后端 | **只有结构体草图，零实现** | 未见代码 |
| §2.3 BPE tokenizer | 字节级词表 | A-01 |
| §2.3 top-p / repetition penalty | 仅 temperature / top-k / max_tokens | A-05「部分」 |
| §2.4 推理任务队列、抢占、并发上限 | 同步单请求循环 | A-03「待实现」 |
| §3 上下文管理、KV 快照 | 8 轮滑窗 | A-06「部分」/ A-07「待实现」 |
| §4 记忆 LRU-K + TTL | 内存 8 轮 | A-08「待实现」 |
| §5 向量库（`/var/lib/aios`） | 无 | A-09「待实现」 |
| §6 JSON Schema function calling | 规则路由 | A-11「通过」（但机制不同） |
| §6 单次 8 次调用 / 64 KiB 上限 | 单次 2 KiB，次数无上限 | A-12「部分」 |
| §7 capability 位 + 路径白名单 + `ai-sandbox` | **完全没有** | A-14「部分」 |
| §7 提示注入隔离 `<untrusted>` | 无 | A-15「待实现」 |
| §8 8 个 `ai_*` 系统调用（0x400–0x407） | 实际 0x400–0x417 保留，落地 5 个 | `ai_core.c:18-19` |
| §12 验收：1.5B 模型 > 3 tok/s | 无量化大模型 | A-03「待实现」 |
| §10 桌面 AI 客户端、§9 Shell `?` | 无 | A-17/A-20「待实现」 |

> **这张表就是它最有价值的产出**：它把「做 AI 原生 OS 需要哪些模块」拆成了
> 一份可勾选清单，并且诚实地标了勾。HBOS 若将来要做 AI，拿这张表当规格书
> 比读任何博客都省时间——**但要知道右列的真实状态，别把左列当已交付**。

---

## 3. 能力矩阵对比

| 维度 | HBOS（v0.2.1-pre1） | MCubeAI-OS（M9 基线） |
|---|---|---|
| 定位 | Linux ABI 兼容层 + 自研 Wayland 合成器（HIVE），跑 Debian Chromium/Electron | 从零 x86_64 教学 OS，AI 作为用户态一等服务 |
| 可执行格式 | ELF64 + **PT_INTERP 动态链接**（`src/elf.c:326,340`） | 自定义 `LGY` 容器（`include/mcube/lgy.h`）+ 自有 ELF 装载 |
| 动态链接 / `.so` | 有（自制加载器，`src/elf.c` 43,669 B） | **无**（freestanding libc，静态） |
| libc | 真 glibc（跑 Debian 二进制） | 自研 freestanding（`user/lib/`） |
| 文件系统 | ext2 **只读**挂载 + `FS_MAX_FILES 131072` 平铺表（`src/fs.h:24,46`） | FAT32（8.3 短名、双 FAT）+ tmpfs/devfs |
| 系统调用 | ~244 KB 分发，Linux 号段覆盖 0–446（`src/syscall.c:2333`） | 38 KB 分发，Linux 号段子集 + `0x400–0x417` AI 段 |
| 图形栈 | **真 Wayland 服务端**（`src/gui/wayland/`，9 个 `.c`，含 wl_compositor/wl_shm/xdg_wm_base…） | 自绘合成器 + 自有 fd3/fd4 管道协议（`desktop.c` 173 KB） |
| 网络 | （未在本次范围核实） | e1000 + ARP/IPv4/ICMP/UDP/DNS |
| USB | （未核） | xHCI/EHCI/UHCI 全栈约 104 KB |
| ACPI | （未核） | ACPI + **AML 解释器** 29 KB |
| **AI 子系统** | **零**（`grep` 无命中） | 用户态守护进程 2,227 行 + 内核 226 行 |
| AI 模型 | — | 256 维字节 bigram（玩具） |
| AI 工具沙箱 | — | **无**（工具以守护进程自身权限运行） |
| AI 审计 | — | 内核 64 项环，仅 kind/pid/text |
| 验收矩阵 | `CHROMIUM_COMPAT_BASELINE.md` §4 有 Chromium 能力矩阵 | `acceptance.md` 全系统 A-01..A-22 带状态 |
| 可运行镜像 | 需自建（`make`/limine），无 release | 需自建（`make image/iso/disk`），**无 release/tag** |
| 社区 | — | 2 star / 0 fork / 1 open issue；**昨日有外部安全分析**（issue #1，2026-11-07） |

---

## 4. HBOS 相对它的优势（别被「AI 原生」这个词吓到）

1. **ABI 兼容是硬壁垒，AI 是软功能**。HBOS 的 `src/elf.c` 有
   `elf_interpreter_t` / `prepare_interpreter()`，能解析 `PT_INTERP` 并在切 CR3 前
   解析依赖 【原文】`src/elf.c:326-340`。MCubeAI-OS 的内核「exec 时不解析 ELF」，
   意味着它**永远跑不了任何 unmodified Linux 二进制**。前者是护城河，
   后者是一个可以后加的模块。

2. **真实负载驱动的复杂度**。HBOS 的 `src/syscall.c` 244 KB、
   `src/linux_compat.c` 106 KB（socket/memfd/epoll/eventfd/signalfd/timerfd/
   inotify/SHM），是被 glibc + Chromium 的真实调用打出来的；MCubeAI-OS 的
   syscall 只有 38 KB，服务对象是自家 20 个 coreutils。**复杂度不是缺点，
   是这个领域唯一的护城河。**

3. **图形栈选对了协议**。HBOS 实现的是**真 Wayland 服务端**
   （`src/gui/wayland/{backend,core,iface}`），MCubeAI-OS 是自有管道协议。
   后者意味着它的每个 GUI 应用都得用它的私有 libc 重写；前者意味着
   Chromium/Electron 可以直接接上。这是**协议选择层面的胜负**，不是工作量差异。

4. **有 R1–R49 的真实踩坑账本**。`docs/jindu.md` 1,147 行记录了从
   131072 项文件表（`src/fs.h:24`）到 `getcwd()` 在 `base/path_service.cc:264`
   失效的每一步。这种账本 MCubeAI-OS 完全没有——它的文档是**设计**，
   HBOS 的文档是**病历**。做系统内核，病历比设计值钱。

---

## 5. HBOS 相对它的劣势（三条真差距）

1. **AI 侧从 0 到 1 一次都没走过**（`grep` 确认零代码）。它的价值不是"我们有
   它没有"，而是"它替我们试错了哪些坑"。等 HBOS 真要接 AI 时，§2.5 那张表
   能省掉一轮架构返工。

2. **验收不可判定**。`docs/jindu.md` 是时间序叙事，没有一个地方能回答
   「当前一定能跑什么」。它需要的人肉阅读成本，正是 MCubeAI-OS 用
   `acceptance.md` 一张表解决掉的。

3. **没有「AI 动了系统之后凭什么信」的位置**。HBOS 的 `src/syscall.c` 里
   所有权限判定都服务于「进程是不是进程」，没有为「某个代理是否被授权做
   这次调用」留位。这不是现在要做的功能，而是**一旦定下 AI 通道形态就难改**
   的结构性问题（见 C2）。

---

## 6. 值得做（C1–C6）

### C1 · 全系统验收矩阵（P0，零代码）

- **做什么**：把 `docs/jindu.md` 的 R1–R49 提炼成一张
  `docs/ACCEPTANCE.md`，每条带 `通过 / 部分 / 待实现 / 已回滚` 和**验证命令**。
  格式直接照 `docs/acceptance.md`（A-01..A-22 那张表）。
- **落点**：新建 `docs/ACCEPTANCE.md`；从 `docs/jindu.md` 抽取。
- **代价**：**0 行代码**，约半天。不碰任何 `src/`。
- **收益**：把 1,147 行叙事压成可判定状态表；R49 的 `getcwd` 卡点会立刻
  从"某段文字"变成"一条红色用例"，下一步该做什么不用再想。
- **为什么是 P0**：这是唯一一项**与 R49 卡点正交、且立刻产生决策价值**的改动。

### C2 · AI 通道形态提前定死（P1，只写设计不写码）

- **做什么**：写下两条不可逆决定的结论并进 `docs/`：
  1. AI 会话通道**返回动态 fd 对**（不照抄 fd 3/4），语义等同 `socketpair`；
  2. 新增 `SYS_AI_*` 号段放 **`0x400` 起**（已验证 HBOS 侧空闲：
   Linux 分发 switch 最大到 446、`default` 在 `src/syscall.c:2942`；
   自有 enum 在 `src/user/libc/syscall.h:78-217`，都够不着 1024）。
- **落点**：`docs/`，以及将来的 `src/syscall.c` 分发点（`src/syscall.c:2333`
  Linux 段 / `:3313` 自有段）。
- **代价**：写文档，0 代码。
- **收益**：避免将来接 AI 时因为 fd 号冲突（Chromium 占满 3/4）而返工。

### C3 · 内核审计环（P1，约 250 行，但必须改掉它的 5 个缺陷）

- **做什么**：抄「内核侧不可伪造的记账点」这个**思路**，不抄它的实现。
  最小形态：一个 64–256 项环 + 一个 `SYS_AUDIT_READ`，**写权限绑定到
  可执行文件路径校验**（而不是照抄 MCubeAI 的"谁先到谁是守护进程"）。
- **落点**：新建 `src/audit.c` + 头；注册进 `src/syscall.c` 分发。
- **代价**：约 250 行 + 少量 BSS。低风险，不触碰现有路径。
- **收益**：一旦有 AI/自动化调用高权限 syscall，「谁批准的」有内核侧证据。
- **前置**：必须同时修 §8 列的 MC-03 / MC-09 / MC-10 / MC-20。

### C4 · 一次性的验收证据格式（P1，低成本）

- **做什么**：规定所有验证结论写成 `{命令, 退出码, 关键输出行}` 三元组，
  禁止"看起来正常"。
- **落点**：`docs/`（可并入 C1）。
- **代价**：0 代码。
- **收益**：直接对症 R49 那类问题——"栈上残留字符串 `FATAL: base/path_service.cc:264`"
  这种证据之所以珍贵，是因为它可复现；把这种习惯制度化。

### C5 · 工具层规则路由起步（P2，约 600 行用户态）

- **做什么**：如果真要给 HBOS 加 AI 助手，**先写规则路由**，不要先接模型。
  MCubeAI-OS 的 `ai_route` 证明了这条路能跑通 demo。
- **落点**：用户态程序（不入 `src/kernel.c`）。
- **代价**：中。
- **收益**：在模型还不存在时就有可演示的端到端链路。
- **警告**：这是**被玩具模型逼出来的过渡方案**，不要当成长期架构（见 §8）。

### C6 · 可写审计落盘（P2，被 ext2 只读阻塞）

- **做什么**：审计事件落到持久存储。
- **落点**：受限于 HBOS ext2 **只读**（`src/ext2.c`），只能先落 `/tmp` ramfs
  或内存环。
- **代价**：等待 ext2 可写，或接受易失。
- **收益**：跨重启可追溯。
- **结论**：**现在别做**，记为阻塞项。

---

## 7. 可以不做（明确不做也不会落后）

| 项 | 为什么可以不做 |
|---|---|
| KV cache 快照 / 上下文恢复（`ai-native.md` §3） | HBOS 当前连 Chromium 都起不来，没有 AI 会话可恢复 |
| 向量记忆 / HNSW（§4 §5） | 需要可写持久存储 + embedding 模型，两样 HBOS 都没有 |
| 多模型并发调度 / 抢占（§2.4） | 单模型都还没有 |
| GPU/NPU 抽象（`struct ai_backend_ops`） | **它的设计里也只有结构体草图，零实现**；抄一个空接口是负收益 |
| BPE / SentencePiece tokenizer（§2.3） | 先用字节级就够，且 HBOS 的瓶颈不在这 |
| mmap 权重 / 按层换出（§2.1） | 依赖可写存储与页缓存，HBOS 的 VFS 明确"dentry/page cache 与 LRU 留待后续"（同类设计） |
| `ai-sandbox` 独立 UID 进程（§6） | HBOS 的权限模型本身还没到需要 AI 沙箱的阶段；且它自己也没做 |
| AML 解释器 / USB 全栈 | 与 AI 无关；HBOS 若需要再单独评估（另见 OpenCubeOS 对标文档 C13） |

---

## 8. 明确不该学

### 8.1 结构性错误：不要照抄它的权限模型

第三方安全分析（仓库 issue #1 附件，作者 `duahciM`，2026-11-07）对本项目
AI 部分给出的结论，与我从代码里独立读到的一致：

> **MC-09（高）守护进程角色可被任意进程抢占，审计可伪造**
> `ai_chan_accept()`：`if (g_daemon_pid == 0) g_daemon_pid = me->pid;`
> 第一个调用 `SYS_AI_ACCEPT` 的进程就成为守护进程，不校验可执行文件路径、
> 不校验 pid 来源 …… 该进程同时获得写审计权限。
> 【外部报告】`sec.md` §4.3（对应 `kernel/ai/ai_chan.c:105-141`）

我独立复核了这段代码，确认逻辑就是「先到先得」：`ai_chan_accept()` 里
`if (g_daemon_pid == 0) { g_daemon_pid = me->pid; }` 即登记，之后仅比对 pid
【原文】`kernel/ai/ai_chan.c:112-115`。
**因此「内核审计环不可伪造」这个卖点在自己的实现里是破的**——
注册权本身就是无认证的。这是 HBOS 若要抄这条设计时**必须设计掉**的第一件事。

同一份报告另外四条（我未逐条复核，仅作线索）：

| ID | 级别 | 问题 | 对 HBOS 的含义 |
|---|---|---|---|
| MC-03 | 严重 | 无路径沙箱（`tool_fs_write` 只要求路径以 `/` 开头）+ **确认可自签**（请求方与被请求方是同一进程） | 确认机制必须绑一次性 nonce + 独立授权方 |
| MC-10 | 高 | 会话键由客户端提供，可遍历 1..N 读/清别人的会话 | 会话号必须服务端分配并绑定连接 |
| MC-20 | 中 | 审计只有 `{ts,pid,kind,text[64]}`，缺 uid/session/参数摘要/结果/耗时 | 审计条目字段要一次设计够，改结构成本高 |
| MC-11 / MC-05 | 高 / 严重 | 单线程阻塞模型易被 DoS；`aiosd` 死后 AI 服务不可恢复 | 守护进程要能自愈重启，且通道要能重连 |

### 8.2 五条「不要抄」清单

1. **不要抄 `LGY` 自定义容器**。HBOS 的全部价值在 ELF/`PT_INTERP`；换容器等于自杀。
2. **不要抄「先到先得当守护进程」**（MC-09）。用 ELF 路径校验或能力位。
3. **不要抄「客户端自己确认自己」**（MC-03）。确认必须来自独立授权方 + 一次性 token。
4. **不要抄「客户端提供 session key」**（MC-10）。服务端分配、绑定连接。
5. **不要抄「内核只保留号段 + 打印一行日志」**的占位实现。
   `ai_core.c` 21 行里 19 行是注释，唯一动作是 `kprintf` 一行
   【原文】`kernel/ai/ai_core.c:18-21`。这是**文档占位符**，不是子系统。

### 8.3 定位层面不要学

- **不要学「AI 原生操作系统」这个定位话术**。它的内核 AI 代码 226 行、
  模型是 bigram；拿着这个词对外讲，会在第一次被问「跑什么模型」时露底。
  HBOS 现在的定位（能跑 Chromium 的 Linux ABI 兼容内核）**更硬、更可验证**。
- **不要把规则路由当长期方案**。它是被玩具模型逼出来的
  （「the tiny model cannot emit reliable function calls」）【原文】`ai/README.md`。
- **不要学 8.3 短名限制**（「the FAT32 driver does not parse LFN entries yet」）。
  HBOS 用 ext2，没有这个问题，别退回去。

---

## 9. 对 HBOS 的行动项

| # | 行动 | 落点文件 | 代价 | 收益 | 优先级 |
|---|---|---|---|---|---|
| 1 | 把 R1–R49 提炼成带状态的验收矩阵 | 新建 `docs/ACCEPTANCE.md`（素材 `docs/jindu.md`） | **0 代码**，约半天 | 卡点从叙事变成可判定红项；R49 立刻可执行 | **P0** |
| 2 | 写死 AI 通道两条不可逆决定：动态 fd 对、`0x400` 号段 | `docs/`（将来落到 `src/syscall.c:2333` Linux 段 / `:3313` 自有段） | 0 代码 | 避免 Chromium 占满 fd 3/4 后的返工 | **P1** |
| 3 | 内核审计环（抄思路，修掉 5 个缺陷） | 新建 `src/audit.c`，注册进 `src/syscall.c` | 约 250 行 + BSS | 高权限调用有内核侧不可伪造证据 | **P1** |
| 4 | 验证结论统一成 `{命令, 退出码, 关键输出}` 三元组 | `docs/`（并入第 1 项） | 0 代码 | 消灭"据推测"式结论；R49 那类坐标可复现 | **P1** |
| 5 | 规则路由式工具层起步（真要加 AI 助手时） | 用户态新程序（不入 `src/kernel.c`） | 约 600 行 | 无模型也能跑通端到端 demo | **P2** |
| 6 | 审计事件持久化 | 受 `src/ext2.c` 只读阻塞 | 等 ext2 可写 | 跨重启可追溯 | **P3（阻塞）** |
| 7 | AI 会话通道实现（fd 对 + 定长消息） | 复用 `src/linux_compat.c:1465`（`linux_compat_pipe2`）、`src/fd.h:56`（`entries[POSIX_MAX_FDS]`，上限 128 见 `src/fd.h:15`） | 约 120 行 | 任何 HBOS 进程（含将来的 chromium）可当客户端 | **P2** |

> 上表每条落点均已在本工作区核对存在：
> `src/syscall.c`（244,133 B，分发 switch 在 `:2333`/`:3313`）、
> `src/linux_compat.c`（105,977 B，`linux_compat_pipe2` 在 `:1465`）、
> `src/fd.h`（`POSIX_MAX_FDS 128` 在 `:15`，`entries[]` 在 `:56`）、
> `src/ext2.c`（22,347 B）、`src/elf.c`（43,669 B，`elf_interpreter_t` 在 `:326`）、
> `src/fs.h`（`FS_MAX_FILES 131072` 在 `:24`）、`src/user/libc/syscall.h`（enum `:78-217`）、
> `docs/jindu.md`（1,147 行）。

**与 R49 的关系**：以上 7 项**没有一项**能推进 `getcwd()` 那个
`FATAL: base/path_service.cc:264`。这不是遗漏，是结论——
**MCubeAI-OS 帮不上 HBOS 当前的忙**。

---

## 10. 我未能验证的部分（明确声明）

以下事项我**没有**验证，本文任何地方都不应被读作对它们的断言：

1. **能否编译通过**。受任务边界约束（禁止 `make` / 编译 / QEMU），
   我没有执行任何构建。`Makefile` 存在 `image/iso/disk/test/run-gui` 等 23 个目标
   【原文】`Makefile:111-449`，但**目标存在 ≠ 能跑通**。
2. **是否有可运行镜像**。`/releases` 与 `/tags` 均为空，因此**没有预编译产物**；
   必须自建。我没构建，所以**无法确认自建后能否启动到桌面**。
3. **ELF 装载与 LGY 的关系**。`kernel/user/process.c` 有 `user_load_lgy()`
   （`:154-169`），而 `kernel/arch/x86_64/syscall.c` 的 exec 注释写
   「Load an ELF and build a fresh address space」（`:577-579`）。二者并存，
   我**没有读完 exec 的完整分派**，不确定是走 LGY、ELF、还是双支持。
   因此本文只断言「它不用 Linux 的 `.so`/`ld.so` 动态链接」，**不断言它不能加载 ELF**。
4. **`aiosd` 的实际端到端行为**。没有运行，所有行为描述来自源码与 `acceptance.md`
   的自评。`A-13 权限确认` 标的是「通过（手工）」，我也未复现。
5. **第三方安全报告的结论**。§8.1 的 MC-* 五条中，只有 **MC-09** 我回到源码
   独立复核过；其余四条我未见原始证据，按「外部线索」处理。
6. **它的网络栈 / USB / ACPI 质量**。我只看了文件大小与文档描述，未读实现，
   能力矩阵里这三行标为「未核」。
7. **社区影响与外部评价**。除仓库自身指标（2 star / 0 fork / 1 issue）外，
   我用 `web_search` 检索「MCubeHub / MCubeAI-OS / AI 原生操作系统」等关键词
   **没有找到任何第三方报道、论文或引用**。因此无法判断它在更大范围内是否有
   认知度；org 创建于 2026-10-07，仓库创建于 2026-10-23，都极新。
8. **HBOS 侧 `src/kernel.c` 等模块的内部细节**。本文引用的 HBOS 事实限于
   「文件存在 / 大小 / 行号 / 特定符号」这类可机械核对的项，未评估其实现质量。

---

## 11. 一句话结论

**MCubeAI-OS 是一个诚实但小的实验：它用 226 行内核代码和 2,227 行用户态代码，
证明了「AI 不必进内核」这件事——而这恰好是 HBOS 最该先知道的一条。**
它的模型是玩具、权限模型是破的、跑不了任何 Linux 二进制；但它那份
`acceptance.md`（每条标状态）和「内核审计环」的思路，是 HBOS 现在就能拿走的
两样东西——前者半天、零代码，后者约 250 行。其余全部可以等
`getcwd()` 修好、Chromium 起来之后再说。
