# DeepSeekOS 对标分析（附：DeepSeek Harness 作为真实对手）

> 分析日期：2026-10-31
> HBOS 基线：`v0.2.1-pre1`（`version.mk`：MAJOR=0 / MINOR=2 / BETA=1 / PRE=1，`HIVE_GUI_REV=1`）
> 分析对象：①"DeepSeekOS"（用户假设的 DeepSeek 官方操作系统）；②社区同名 OS 项目；③**DeepSeek 官方实际交付的桌面形态产品 DeepSeek Harness（dsh）**
> 核心结论前置：**以我检索到的一手信息，无法确认 DeepSeek 官方在做操作系统。**

## 阅读约定

- **【实测】**：我在本机对下载到的真实文件做的分析，或对一手页面原文的直接引用。
- **【报道】**：来自公开网页/媒体，未经独立验证；**与【实测】冲突时以【实测】为准**。
- **【推断】**：基于事实的推理，可能错。

**本文最重要的是 §0 的核查结论**。如果团队里有人因为看到"DeepSeek 发布操作系统"的标题而想调整 HBOS 的路线，请先读完 §0 再决策。这个任务的第一要务不是找差距，而是**阻止一个基于不存在竞品的战略调整**。

---

## 结论摘要

1. **DeepSeek 官方没有操作系统项目**。`github.com/deepseek-ai/DeepSeek-OS` 返回 404；官方组织 46 个公开仓库逐条核查，**无任何仓库名含 os / kernel / linux / system**。【实测】社区同名项目 `SodiumLauncher/DeepseekOS` 的 README 全文只有两行，是一个 Python 小脚本，0 star、0 fork、1 commit、无 release，**它不是操作系统**。【实测】媒体标题里的"真OS""AI 操作系统"是**对软件产品定位的比喻**，不是内核。【报道】**判定：DeepSeekOS 不存在，不构成对 HBOS 的威胁，也不应作为 A 战略的参照。**

2. **但 DeepSeek 确实交付了一个以桌面应用形态落地的产品：DeepSeek Harness（dsh）**。官方原文："`DeepSeek Harness (dsh) is an open-source agent harness developed by DeepSeek AI`"、"`It is built on an everything-is-a-plugin architecture and powered by Cordis`"，MIT 许可，Electron 桌面壳 + Node Host + Web UI。【实测】这才是 HBOS 该对标的真实对手。

3. **DSH 的桌面端是一个纯 Electron 应用，对 HBOS 的依赖面与 Chromium 高度重合，而 HBOS 已有的大部分能力正好覆盖它**。【实测】`docs/architecture.md` 原文："`The Electron desktop application ships its exact dsh production runtime in signed resources`"、"`Electron starts the private Desktop Host in Electron Node mode`"、"`Desktop listens on an OS-assigned port by default`"。【推断】这意味着 **HBOS 只要跑通 Chromium，就大概率能跑通 DSH**——不需要为它做任何专有适配。

4. **DSH 引入了一个 HBOS 当前完全没有的新需求面：Node.js 运行时语义**。Node 比 Chromium 更依赖 `/proc` 细节、`epoll` 边角、`signalfd`、`inotify`、`statx`、`futex` 超时精度。【实测】HBOS 的 `/proc` 只实现了 `uptime/meminfo/cpuinfo/sys/fs/inotify/max_user_watches/<pid>/stat|<pid>/status|<pid>/task/<tid>`（`src/devfs.c:251-329`），Node 需要的 `self/maps`、`self/fd/`、`self/environ` 等尚未确认。【推断】这是**值得提前验证**的方向，但不是当前卡点的解药。

5. **对 HBOS 当前卡点（Chromium `CHECK(!channel_)`）零帮助**。DSH 完全跑在 Chromium 之上，Chromium 不通 DSH 必不通。**判定：本文对当下没有可执行的提速动作，唯一价值是"确认不需要为它分心"以及"提前知道 Chromium 通了之后下一步是什么"。**

---

## 0. 核查结论（本文最重要的一节）

### 0.1 假设一：DeepSeek 官方推出的操作系统 —— **不存在**

| 核查项 | 方法 | 结果 |
|---|---|---|
| `github.com/deepseek-ai/DeepSeek-OS` | 直接访问 | **HTTP 404**，页面显示 "This is not the web page you are looking for"。【实测】 |
| 官方组织仓库列表 | 访问 `github.com/orgs/deepseek-ai/repositories` | **46 个公开仓库**，逐条查看，**无任何名字含 os / kernel / linux / system / distro**。【实测】 |
| 官方站点 | `deepseek.com` | 无操作系统产品线。【实测】 |
| 组织内最大项目 | 同上 | `deepseek-harness`（TypeScript，约 272–274K star / 33–34K fork，MIT）。【实测】 |

组织内 46 个仓库的构成（按主题归类，【实测】）：

- **模型/训练**：DeepSeek-V3、DeepSeek-V3-2-Exp、DeepSeek-Coder、DeepSeek-Coder-V2、DeepSeek-OCR、DeepSeek-OCR-2、Engram、DeepSeek-Reasoner 系列
- **推理/算子**：FlashMLA、DeepGEMM、DeepEP、DeepEP-Ascend、DualPipe、TileKernels、DeepJIT、DeepSelect
- **基础设施**：3FS（分布式文件系统）、deepseek-recipe
- **Agent 运行时**：**deepseek-harness**、dsh-superagent-prototype、dsh-node-addon-require-builtin、dsh-libreoffice-kit
- **文档/生态**：awesome-* 若干

**没有任何一项在操作系统层面工作。** 3FS 是面向数据中心的用户态分布式文件系统，与 HBOS 的 VFS/ext2 不构成竞争。

### 0.2 假设二：社区同名 OS 项目 —— 存在但**不是操作系统**

`github.com/SodiumLauncher/DeepseekOS`【实测】：

| 项 | 值 |
|---|---|
| README 全文 | `# DeepseekOS` / `一个Deepseek用Python编写的，简易的程序。A simple program written in Python using Deepseek.` |
| Star / Fork | 0 / 0 |
| Commit 数 | 1 |
| Release | 无 |
| 代码 | Python 脚本，调用 DeepSeek API |

**它不是操作系统。** 我未穷举全部 GitHub 同名项目（搜索结果噪声过大），但已确认 star 最高的那个是玩具项目；即使存在其他同名项目，也未形成任何社区影响力（无 star、无 issue、无发布）。

### 0.3 媒体噪音的定性与处理

【报道】以下标题在 2026-11 前后广泛传播，**必须被标注为不可作为决策依据**：

- 163 / 新智元 2026-11-06《DeepSeek砸出「真OS」》
- 今日头条《DeepSeek打造的是AI操作系统》

这类文章的"OS"指的是**"AI 时代的操作系统级入口/平台"这一商业比喻**（类比"微信是操作系统"），**不是内核、不是发行版、不是能装机的系统**。把这类标题理解为"DeepSeek 在做 OS 竞品"是**把比喻当事实**。

**给 HBOS 团队的一句话：如果有人在会上说"DeepSeek 都做 OS 了，HBOS 要调整方向"，请要求他出示 DeepSeek 的 OS 仓库或装机镜像。拿不出来，这条战略输入就是假的。**

### 0.4 唯一真实的对手：DeepSeek Harness（dsh）

既然用户要求对标，我把真实存在的东西查清楚了。事实以下全部来自官方一手（README / architecture.md / 官网）【实测】：

| 项 | 内容 |
|---|---|
| 名称 | DeepSeek Harness，命令 `dsh` |
| 定性（官方原文） | "`an open-source agent harness developed by DeepSeek AI`" |
| 架构（官方原文） | "`built on an everything-is-a-plugin architecture and powered by Cordis`" |
| 许可 | MIT |
| 状态 | **developer preview**，官方明写 "`THERE WILL BE COMPATIBILITY-BREAKING CHANGES`" |
| 两种形态 | `npx @deepseek-ai/dsh web`（Web UI，`http://127.0.0.1:3080`）／**Electron 桌面应用** |
| 桌面端启动（官方原文） | "`The Electron desktop application ships its exact dsh production runtime in signed resources and owns the reserved $DSH_HOME/profiles/desktop`" |
| 桌面端进程（官方原文） | "`Electron starts the private Desktop Host in Electron Node mode`"；"`Node IPC carries boot injections, readiness, fatal errors, and shutdown`" |
| 端口（官方原文） | "`Desktop listens on an OS-assigned port by default`" |
| 插件执行（官方原文） | "`Shell-owned UI runs plugin transactions through bundled pnpm with normal user and profile configuration`" |
| 核心包 | `core/session`（SessionEvent 日志）、`core/system-prompt`、`core/tools`（scoped registry + guarded execution）、`core/agent`、`core/agent-loop`、`core/scope`、`llm/llm`（adapter seam） |
| 事件域 | Session / Agent / Capability 三域（`fs/*`、`tools/*`、`telemetry/*`） |

【报道】环球网 2026-11-06 给官方口径的运营数据：DSH v0.1 于 8 月 13 日上线、9 月 29 日推桌面端、截至 11 月 4 日 DAU 154 万、MAU 548 万、日耗 token 约 92 万亿、第三方插件约 3.49 万个。**这类数字我无法验证，仅作规模参考。**

---

## 1. 对方定位与架构摘要

### 1.1 定位

DSH 不是操作系统，是**跑在现有桌面操作系统之上的 Agent 运行时**。它的商业意图是把"Agent 能力"做成可插拔的平台层，而不是替换 OS。

### 1.2 架构（据官方 architecture.md【实测】）

```
┌──────────────────────────────────────────────────────────────┐
│ Electron 桌面壳                                               │
│   ├─ 窗口立即加载打包好的 Web 资源                             │
│   ├─ Node IPC: boot injections / readiness / fatal / shutdown │
│   └─ OS 分配的随机端口上起本地 HTTP 服务                       │
├──────────────────────────────────────────────────────────────┤
│ Desktop Host（Electron Node mode 下的私有运行时）              │
│   ├─ 调用共享 CLI profile runner                              │
│   └─ 承载完整 Web 应用                                        │
├──────────────────────────────────────────────────────────────┤
│ dsh 运行时（Cordis 插件树）                                    │
│   ├─ core/session   SessionEvent 日志                         │
│   ├─ core/tools     scoped registry + guarded execution       │
│   ├─ core/agent     + core/agent-loop                         │
│   ├─ llm/llm        adapter seam                              │
│   └─ 3.49 万第三方插件（报道）                                 │
├──────────────────────────────────────────────────────────────┤
│ 需宿主 OS 提供：Node.js / Electron / Chromium / 子进程 / pnpm   │
└──────────────────────────────────────────────────────────────┘
```

### 1.3 关键判断：DSH 对 OS 的需求 ≈ Chromium 对 OS 的需求 + Node 对 OS 的需求

【实测】官方 README 只要求 "Install `Node.js`"，不要求任何 OS 特定组件。桌面端是 Electron。【推断】因此 DSH 在 HBOS 上的可行性分解为两个已有/待建的能力：

- **Chromium 面**：与当前 `docs/jindu.md` R54 之后的阻塞**完全同一组**（`CHECK(!channel_)`、`/dev/shm`、`/tmp` ENOSPC、多进程）。
- **Node 面**：HBOS **尚未针对性验证**。Node 对 `/proc/self/maps`、`/proc/self/fd/*`、`epoll` 边角、`signalfd`、`inotify` 的组合压力比 Chromium 更细碎。

---

## 2. 逐项对标表

| # | 能力维度 | 对方做法 | HBOS 现状（文件:行号） | 差距等级 | 对 A 战略的价值 |
|---|---|---|---|---|---|
| 1 | "是操作系统" | **不是**，是 Electron 桌面应用 + Agent 运行时。【实测】 | — | 无差距 | **确认不需要把它当 OS 竞品对标** |
| 2 | 图形栈 | Electron → Chromium Ozone | HIVE Wayland 合成器（`src/gui/wayland/core/wl_server.c`，7 全局） | 必须补（进度项） | 与 Chromium 验收同一缺口 |
| 3 | 动态链接 | Node/Electron 依赖 glibc、libstdc++、libnss3 等 | `src/elf.c` 支持 PT_INTERP + 符号版本 | 无差距（待实测） | A 战略覆盖面够 |
| 4 | 多进程 | Electron 主进程 + renderer + utility + Host | R36："单进程路线走不通，必须多进程"（`docs/jindu.md`） | 必须补（进度项） | 硬门槛，与微信内嵌 Chromium 同构 |
| 5 | 子进程 | `core/tools` 的 guarded execution 起子进程；pnpm 跑插件事务 | `src/syscall.c` `case 59`（`:2678`）execve；`case 57`（`:2675`）fork/vfork 路径 | 无差距（基础具备） | A 战略核心能力已成型 |
| 6 | IPC：Node IPC | Electron/Node 用 socketpair + JSON over pipe | AF_UNIX（`src/linux_compat.c:986/1025`）、pipe2（`linux_compat.c:1465`） | 无差距 | 已有 |
| 7 | IPC：mojo | Chromium 多进程用 mojo over AF_UNIX + memfd | `/dev/shm` memfd slot 后备（`src/linux_compat.c:2373-2402`） | 必须补（进度项） | 与 Chromium 同坑 |
| 8 | 事件循环 | Node libuv 依赖 epoll + eventfd + timerfd | epoll_create1 `src/syscall.c:2920`、epoll_wait `:2791/:2896`、eventfd2 `:2919`、timerfd `:2956(332)` 区段 | 无差距（已实现） | **libuv 的核心需求 HBOS 已覆盖，是 DSH 可行性的最大利好** |
| 9 | 线程同步 | Node worker_threads + futex | `case 202` futex（`src/syscall.c:2776`） | 无差距（已实现） | — |
| 10 | 信号 | Node 用 signalfd + 自管道 | signalfd（`src/syscall.c:3777`）、`case 290` 区段 | 无差距 | — |
| 11 | 文件监视 | Node `fs.watch` → inotify；dsh HMR 依赖文件监视 | inotify（`src/syscall.c:2815` 区段 `:2928`） | 无差距 | **HMR 依赖 inotify，HBOS 已实现** |
| 12 | `statx` | Node 20+ 的 `fs.stat` 走 `statx` | `case 332`（`src/syscall.c:2956`） | 无差距（需确认字段完整度） | — |
| 13 | **`/proc/self/maps`** | Node/V8 读它做堆布局与 `--max-old-space-size` 判断 | `/proc` 仅实现 uptime/meminfo/cpuinfo/sys/fs/inotify/max_user_watches/`<pid>/stat|status|task/<tid>`（`src/devfs.c:251-329`），**无 `self/maps`** | **必须补（新发现）** | **DSH 引入的第一个 Chromium 不需要、Node 需要的缺口** |
| 14 | `/proc/self/fd/*` | Node 的 `process.stdin`、`fs.readdir('/proc/self/fd')` | 同上，未见实现 | 值得抄 | 中等影响 |
| 15 | `/proc/self/environ`、`/proc/self/cmdline` | Node 的 `process.env` 初始化、`process.argv` 显示 | 同上，未见 | 值得抄 | 低-中 |
| 16 | 文件描述符上限 | Node 频繁开 socket/fd | `src/fs.h:FS_MAX_FILES 131072` | 无差距 | — |
| 17 | 文件系统 | 需要 POSIX yield 的目录遍历、rename、stat | ext2 `src/ext2.c`（含 htree）、`src/vfs.c` | 无差距 | A 战略基底 |
| 18 | `/tmp` 与缓存目录 | Electron 写 `~/.config`、`/tmp`、`$DSH_HOME/profiles/desktop` | `/tmp` 内存覆盖层已落地（`docs/jindu.md` R54） | 无差距（已有） | **DSH 会复用 R54 的成果** |
| 19 | `/dev/shm` | Chromium 共享内存 | 内存覆盖层 + memfd（`docs/jindu.md` R54） | 无差距 | 已有 |
| 20 | D-Bus | Electron 用于单实例、通知、托盘 | 仅 `src/selftest.c` 有字符串，无实现 | 值得抄 | Electron 缺 D-Bus 会刷错但可启动【推断】 |
| 21 | fontconfig | Electron 渲染文本 | 无 fontconfig 移植 | 必须补（中文显示） | 同微信 |
| 22 | 网络 | dsh 需访问 LLM API（HTTPS） | TCP 栈存在；TLS 需用户态（libssl） | 可忽略（用户态） | HBOS 不需实现 TLS |
| 23 | 沙箱 | dsh 有 "sandbox and approval policy"（`docs/architecture.md` 原文 `dsh-base` 层含 sandbox） | 无 namespace/seccomp 完整实现 | 可忽略（Chromium 已可 `--no-sandbox`） | 不影响验收 |
| 24 | pnpm 执行 | "`Shell-owned UI runs plugin transactions through bundled pnpm`" → 需 `execve` node + 大量 fork | `execve` + `clone` 已实现；fork 语义需确认 | 可忽略（后续） | 插件开发场景，非首屏 |
| 25 | GPU | Electron 默认硬件加速，可 `--disable-gpu` | 无 `/dev/dri`（`src/devfs.c:561-639`） | 必须补（进度项，可用 `--disable-gpu` 绕过） | 与 Chromium 同 |
| 26 | 音频 | dsh 桌面端可能无音频需求 | 无 `/dev/snd` | **可忽略** | 这是 DSH 相对微信的优势：**不需要音频** |
| 27 | 输入法 | Web UI 输入框 | 无 keymap、无 text-input | 必须补 | 与微信 S1/S4 同 |
| 28 | 窗口装饰 | Electron 用 CSD | 无 `zxdg_decoration_manager_v1` | 值得抄 | Electron 通常走 CSD，影响小 |
| 29 | AI 能力交付形态 | **本地 Electron 壳 + 云端模型 + 本地 Node Host** | — | 无差距 | **这是本文对 A 战略最有价值的信息：AI 时代的桌面负载形态是"Electron 壳 + 本地 Node"，HBOS 押 Chromium 是对的** |

### 2.1 一条重要的正交发现

【实测】`docs/architecture.md` 原文把 `dsh-base` 描述为包含 "`model adapters, tools, persistence, sandbox and approval policy, settings, credentials, telemetry`"，并把 **sandbox 与 approval policy 放在基础层**。

【推断】这说明 DSH 的设计假设是：**它自己管权限，宿主 OS 只提供进程与文件原语**。这对 HBOS 是**利好**——HBOS 不需要为 Agent 类应用实现专有的"AI 安全层"，只需要把 POSIX 原语做扎实。任何"HBOS 应该为 AI 应用加 OS 级 Agent 支持"的想法，在 DSH 这个真实样本上**找不到支撑**。

---

## 3. 可执行建议

### D1【现在做】把本文档的 §0 结论写进团队可见位置 —— 代价 0.2 人日

- **做什么**：在本文件之外，于 `docs/ROADMAP-2026-10-23.md` 增加一条"战略输入审查"小节，记录：DeepSeekOS 经一手核查不存在，不作为路线依据。
- **为什么**：用户明确要求"不能让 HBOS 团队基于不存在的竞品做战略调整"。文档写了但没人读到等于没写，需要放进团队会看的路线文件。
- **代价**：0.2 人日。
- **风险**：无。唯一风险是过度解读——不要写成"DeepSeek 没有威胁"，要写成"DeepSeekOS 这个 OS 不存在；DSH 是真实产品但不构成竞品"。

### D2【现在做】**不要**为 DSH 做任何前瞻性投入 —— 代价 0

- **做什么**：什么都不做。
- **为什么**：DSH 完全跑在 Chromium 之上，**Chromium 卡住则 DSH 必卡**。当前 `docs/jindu.md` R54 后仍有 3 条阻塞（`crashpad` ENOSPC/EPERM、`[TRAP]` rip=0x1109A7D7B7、`[WL] client` 未出现）。在 Chromium 出窗口之前，任何 DSH 相关工作都是**把瓶颈往后挪**而非解决。
- **代价**：0。
- **风险**：无。这条建议的价值恰恰在于"明确不做"。

### D3【将来做】`/proc/self/maps` —— 代价 1 人日

- **做什么**：在 `src/devfs.c` 的 `/proc/<pid>/` 生成逻辑（`:115` 起的 `proc_node_lookup` 一带）中，为 `self/maps`（及 `<pid>/maps`）生成标准格式输出：`start-end perms offset dev inode pathname`。数据源用页表遍历 + VMA 链表（`src/syscall.c` 的 mmap 记录）。
- **为什么**：**这是本文唯一一条"DSH 需要而 Chromium 不需要"的缺口**。【实测】`src/devfs.c:251-329` 当前只覆盖 uptime/meminfo/cpuinfo/`<pid>/stat`/`<pid>/status`/`<pid>/task/<tid>`。Node/V8 在启动时读 maps 判断地址空间布局。
- **代价**：1 人日（格式简单，难在拿 VMA 链表）。
- **风险**：中。若 VMA 记录不完整，Node 可能读到不连续区间而误判 64 位可用空间。建议先输出"只含已映射区间、其余留空"的保守版本。

### D4【将来做】`/proc/self/fd/`、`fdinfo/`、`environ`、`cmdline` —— 代价 1 人日

- **做什么**：与 D3 同一处扩展。`fd/` 目录项为该进程每个已打开 fd 提供一个符号链接（或至少一个可读的伪文件），`environ`/`cmdline` 从进程的 arg/env 区读原始字节并在 NUL 处换 NUL（保留原格式）。
- **为什么**：Node 的 `process.env`、`process.argv`、`fs.readdir('/proc/self/fd')` 会碰到。Electron 的 crash reporter 也会用。
- **代价**：1 人日。
- **风险**：低。`fd/` 用符号链接时要求 VFS 支持 `readlink`，需确认 `src/vfs.c` 已有。

### D5【将来做，且先评估】Node 运行时的最小冒烟测试 —— 代价 0.5 人日（评估）

- **做什么**：在 Chromium 出窗口之后，用 Debian 的 `node` 二进制在 HBOS 上跑 `node -e 'console.log(process.pid)'`，再跑 `npx @deepseek-ai/dsh web --no-open`。
- **为什么**：**Node 的 syscall 面与 Chromium 不同**（libuv 的 epoll 边角、`signalfd`、`timerfd` 精度）。HBOS 现在没验证过 Node。【推断】这是一个**独立于 Chromium 的小型验收里程碑**，成本低。
- **代价**：0.5 人日（评估阶段）。
- **风险**：低。Node 是单一进程（相对 Chromium），失败原因会更容易定位。

### D6【明确不做】任何"为 AI Agent 做 OS 级支持"的设计 —— 代价 0

- **做什么**：不新增"Agent 专用系统调用""AI 沙箱""模型缓存层"等任何专有机制。
- **为什么**：DSH 的架构（`docs/architecture.md` 原文：sandbox 与 approval policy 在 `dsh-base` 插件层）证明**真实产品把安全与权限放在用户态管**。OS 只需提供 POSIX 原语。加专有机制违反 HBOS 架构信条"不为跑某 App 加特例分支"。
- **代价**：0。
- **风险**：无。

---

## 4. 明确不值得抄的部分

| 项 | 原因 |
|---|---|
| Cordis 的 "everything is a plugin" | 是应用层架构，与 OS 无关 |
| `dsh-hmr` 热重载 | 依赖 pnpm + 文件监视，属应用层 |
| `core/session` 的 SessionEvent 日志 | 应用层数据模型 |
| 3.49 万第三方插件生态 | 与 OS 层无关 |
| "AI 操作系统"的概念包装 | **是比喻，不是技术实体。抄概念会误导路线** |
| Electron 自带 Chromium 的打包方式 | DSH 反而不该像微信那样内嵌整包（它靠 npm 分发）；对 HBOS 无启示 |

---

## 5. 执行顺序建议

```
D1 写进路线文件 (0.2d)  ← 唯一"现在做"的，且是战略防混淆
D2 明确不为 DSH 投入 (0)

  ─────── Chromium 出窗口（当前卡点，本文无关）───────

D5 Node 冒烟 (0.5d) → 若失败，失败点指向 D3/D4
D3 /proc/self/maps (1d)
D4 /proc/self/fd 等 (1d)
```

**注意：D3/D4 在 Chromium 出窗口之前不要动。** 它们对当前卡点零帮助，且 `docs/jindu.md` R54 已经用掉大量调试预算在 `/tmp` ENOSPC 类问题上，再分散注意力风险高于收益。

---

## 6. 我未能验证的部分

1. **DSH 桌面端在无 D-Bus、无 GPU 环境下的实际行为**。我没有实机运行（DSH 需要 Node + Electron，HBOS 尚不具备）。所有关于"能启动""会刷错"的判断都是【推断】。
2. **DSH 桌面端 Electron 的具体版本与 Chromium 内核版本**。`docs/architecture.md` 与 README 均未给出。这不影响结论（任何 Electron 都跑在 Chromium 上），但影响"复用哪份 Chromium 测试基线"。
3. **Node 对 `/proc` 的具体依赖清单**。我只从 Node 常见行为推断，未读 Node 源码或 strace 实测。
4. **是否还有其他社区 "DeepSeekOS" 项目**。GitHub 搜索噪声过大（大量把 DeepSeek 当推理后端的小工具），我确认了 star 最高的那个，未穷举。
5. **媒体所称"DeepSeek 官方 OS 计划"是否存在未公开的内部项目**。我只能确认**公开信息中不存在**。内部计划无法证伪。

---

## 7. 附录：查询与命令记录

### 7.1 网络查询

| 查询 | 结果 |
|---|---|
| `https://github.com/deepseek-ai/DeepSeek-OS` | **404** |
| `https://github.com/orgs/deepseek-ai/repositories` | 成功，46 个仓库，无 OS 类 |
| `https://www.deepseek.com/harness/` | 成功，官方产品页，中英双语 |
| `https://raw.githubusercontent.com/deepseek-ai/deepseek-harness/master/README.md` | 成功，全文引用见 §0.4 |
| `https://raw.githubusercontent.com/deepseek-ai/deepseek-harness/master/docs/architecture.md` | 成功，Electron 启动链路原文 |
| `https://github.com/SodiumLauncher/DeepseekOS` | WebFetch 报 `fetch failed` |
| `https://raw.githubusercontent.com/SodiumLauncher/DeepseekOS/main/README.md` | 成功，两行 README |
| WebSearch `DeepSeek 操作系统 OS 2026` | 返回大量媒体文章，标题含"真OS"，内容为比喻 |

### 7.2 抓取踩坑记录（供后续复用）

- `api.github.com` 与 `cdn.jsdelivr.net` 在本机被 TLS 拦截：`curl: (35) TLS connect error: error:0A000126:SSL routines::unexpected eof while reading`，重试 3 次均失败。**与 `docs/OPENCUBEOS-DIFF.md` 记录的现象一致。**
- 绕行方案：改用 `WebFetch` 抓 GitHub 页面与 `raw.githubusercontent.com` 原文，均成功。
- `/tmp` 是 10 MB tmpfs，`curl -o /tmp/xxx` 会静默失败。缓存改放 `build/tmp/diff2611/`。

### 7.3 HBOS 侧核实命令

```bash
grep -n "case 202" src/syscall.c                 # futex :2776
grep -n "linux_compat_signalfd\|epoll_create1" src/syscall.c  # :3777 / :5271
grep -n "case 332" src/syscall.c                 # statx :2956
grep -n "case 290\|case 291" src/syscall.c       # eventfd2/epoll_create1 :2919/:2920
grep -n "/proc" src/devfs.c                      # :251-329 已实现节点
grep -n "/proc/self" src/devfs.c                 # 无命中
grep -n "FS_MAX_FILES" src/fs.h                  # 131072
grep -n "LINUX_AF_UNIX" src/linux_compat.c       # :54
grep -n "dev_shm\|memfd" src/linux_compat.c      # :2373-2402
grep -n '"/dev/' src/devfs.c                     # 8 节点，无 dri/snd
wc -l src/syscall.c src/ext2.c src/elf.c         # 5886 / 1072 / 1062
grep -c "case " src/syscall.c                    # 374
```

### 7.4 与本文档无关但需要一并告知的事实

用户在任务描述中给出的 HBOS 基线"Wayland 九全局"与仓库实测不符：**实测为 7 个全局**（`src/gui/wayland/core/wl_server.c:654-667`，compositor/shm/seat/xdg_wm_base/output/data_device_manager/subcompositor），代码注释亦明写 7 个。`wl_protocol.h` 的 `WL_MAX_GLOBALS` 为 8。差异项为 `linux_dmabuf` 与 `presentation`（均未注册）。此差异已按实测写入第 1 份文档 §1.3 与 §2 表。
