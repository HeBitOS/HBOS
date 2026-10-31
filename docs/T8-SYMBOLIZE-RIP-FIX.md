# T8:串口日志符号化工具修复报告(别再给假符号)

任务:`scripts/symbolize_rip.py` 符号化结果全部不可信 → 做成可靠且能自证可靠。
日期:2026-10-31。工作目录 `/media/data/hbosv2`。纯宿主机工具修复,未跑 QEMU。

---

## 0. 一句话结论

**基址 `0x1100000000` 本来是对的**;任务书里的“实际映射在 `0x11067CD000`”是把
内核 `same-perm span=` 的 **±16MB 截断走查结果**当成了映射基址。
真正的事故根因是 **符号表与二进制不是同一个 build**:

| 日志 | 实际运行的二进制 (build-id) | 当时拿去符号化的表 | 结果 |
|---|---|---|---|
| `hs-dom5..14.log` | `chromium-headless-shell 156.0.8078.73-1` (`0de17064…`) | `chromium` 桌面版 `156.0.8078.73-1` (`8f201a24…`) | **全假** |
| `r32-chromium2.log` / `c3-*.log` / `run-*.log` / `hs-dom2,3,4` | `chromium 150.0.7871.100-1~deb12u1` (`203b7252…`) | 同左(v150 表) | 当时是对的 |

也就是说:老脚本对 **v150 日志**给出的符号其实一直是对的;凡是 **headless-shell 的
地址** 被套上 chromium 桌面版符号表,得到的全是毫不相干的假函数名——
这正是“GPU 崩溃被误判成 `SignedExchangeSignatureVerifier`”这类结论的来源。

---

## 1. 三个关键事实(用证据纠正任务书里的前提)

### 1.1 基址:`same-perm span=` 不是映射基址

`src/core/gdt_idt.c:206-235` 的实现:从 rip 所在页**向上下各走查最多 4096 页
(16MB)**,遇到“未映射或权限不同”就停,把两端打印成 `same-perm span=`。
所以当 span 长度恰好是 `0x2001000`(= 2 × 16MB + 4KB)时,**两端都撞到了走查上限**,
span 起点根本不是任何映射的起点。`hs-dom5..14.log` 的 6 条 span 全是 `0x2001000`。

### 1.2 加载器把 PIE 装到 `pie_base - image_min`

`src/elf.c:595`:`const uint64_t pie_base = 0x0000001100000000ULL;`
紧接其后 `load_bias = pie_base - image_min`(`image_min` = 所有 PT_LOAD 的
`p_vaddr & PAGE_MASK` 最小值)。这些 chromium/headless-shell 的第一个 PT_LOAD
`p_vaddr = 0` → `image_min = 0` → **bias = 0x1100000000**,`VA = 0x1100000000 + vaddr`。

### 1.3 用陷阱字节反证(最硬的证据)

Breakpoint 陷阱的 rip 处必须是陷阱指令。chromium 的 `IMMEDIATE_CRASH()` 在这里编译成
**`ud2`(`0F 0B`)**,于是可以直接去真实二进制里核对字节:

```
v150-era 14 个 Breakpoint rip  vs build/tmp/chromium.elf          → 14/14 是 0f 0b
hs-dom5..14 的 rip             vs chromium-headless-shell         → 8/8  是 0f 0b
同样这些地址 vs 错基址 0x11067CD000                                → 0 命中,全是随机字节
```

`0x11067CD000` 这个“基址”在 `hs-dom10.log` 的 trap 上甚至落在只读段里 —— 用它的
话连“这是指令地址”都不成立。

---

## 2. 交付物

1. **重写 `scripts/symbolize_rip.py`**(唯一入口,兼容老的 `地址…` 用法):
   - 基址**不硬编码**:从 `src/elf.c` 读 `pie_base`、从 ELF 程序头读 `image_min`,
     打印 `bias = pie_base - image_min` 的推导过程;支持 `--base` 覆盖。
   - **profile 化**:`build/tmp/symbol-profiles.json` 登记
     `build-id → {真实二进制, dbgsym, nm 符号表, 镜像内路径}`。
   - **build-id 守卫**:符号表/调试文件与二进制 build-id 不一致 → **直接拒绝出符号**。
   - **地址分类拒绝**:可执行段 → 给符号;非可执行段 / 不在任何 PT_LOAD /
     `0x1000_0000_0000` 共享库与 ld.so 区间 / 用户栈 → **拒绝**,并说明原因。
     明确写出“假符号比没有符号更危险”。
   - **自证**:`--selfcheck`(8 项,含“用错基址必须被检出”的负向对照),
     以及 `--log` 模式里的 ud2 指纹校验 + 独立 DWARF 读取器(`eu-addr2line`)交叉校验。
   - `--log FILE`:按崩溃块输出,自动识别是哪个二进制,并把每个块**前置的 FATAL 行**
     打出来当作“语义对不对”的锚点。
   - `--mksyms dbgsym -o out.syms`:生成符号表 + 记录来源 build-id 的侧车文件。
2. **新增符号表** `build/tmp/headless-shell156.syms`(662,789 条,
   来源 `chromium-headless-shell-dbgsym_156.0.8078.73-1`,build-id `0de17064…`)。
3. **profile 登记**(4 条,见 §5)+ 本地 commit。
   自检做成脚本模式而非 Makefile target:Makefile 是与调度者共用的在建文件,
   不为了一个宿主机工具去动它(`python3 scripts/symbolize_rip.py --selfcheck` 一行即可,
   退出码 0/1,需要时再挂 target)。

生成的 4 个 profile:

```
chromium-150.0.7871.100-1~deb12u1        203b72529218969b6f6cd29601092ee7fba44624  syms=build/tmp/chromium.syms
chromium-156.0.8078.73-1                 8f201a244f07b6213001f23b2a2700932d4dd3e1  syms=build/tmp/chromium156.syms
chromium-156.0.8078.73-1~deb12u1         d2ca4df74b4b2be8bca3dd57df999d3ebbe37b80  syms=-(公开源无此 dbgsym)
chromium-headless-shell-156.0.8078.73-1  0de17064b8ccb4589cf27429acdd7e5efe352495  syms=build/tmp/headless-shell156.syms
```

---

## 3. 自检(命令 + 输出)

```
$ python3 scripts/symbolize_rip.py --selfcheck
=== symbolize_rip 自检 ===
[PASS] 1 基址推导(不硬编码)
        bias=0x1100000000;内核 PIE 常量 src/elf.c:595: pie_base=0x1100000000; ELF 程序头最小 PT_LOAD p_vaddr(页对齐 image_min)=0x0
[PASS] 2 span 不是基址(截断证据)
        span 长度 0x2001000 == 2*16MB+4K,两端都撞上限;  可执行段 [0x110212e000,0x110d4af740): rip在段内; 向下还剩 0x57c2be9 才会撞到段首(>=16MB 才能解释下端被截断: 一致)
[PASS] 3 ud2 指纹校验
        2/2 个 Breakpoint rip 处字节是 0f 0b(ud2=IMMEDIATE_CRASH)
[PASS] 4 负向对照:错基址必须被检出
        base=0x11067cd000 时 ud2 命中 0/2、分类=['nonexec', 'nonexec']、且不产出函数名 -> 错误基址被检出
[PASS] 5 build-id 守卫
        拿 chromium-150.0.7871.100-1~deb12u1(203b72529218) 的表配 0de17064b8cc 的二进制 -> 拒绝:符号表与二进制不同源
[PASS] 6 共享库地址拒绝
        0x1000015A4137 -> [shlib] 共享库 / ld.so / 用户堆区间(>= 0x1000_0000_0000),本符号表不覆盖 —— 需要该库自己的 dbgsym,拒绝套主程序符号
[PASS] 7 已知答案语义校验
        5 个已知地址全部命中
[PASS] 8 eu-addr2line 交叉校验
        两个独立 DWARF 读取器结论一致
--- 8/8 PASS ---     (退出码 0)
```

自检的**检出能力**由第 4/5/6 项保证:故意把基址换成日志里的 `0x11067CD000`
(第 4 项)、故意把别的 build 的符号表配上去(第 5 项)、故意喂共享库地址(第 6 项),
工具都必须拒绝;任何一项拒绝失效 → FAIL(退出码 1)。

单独使用时的拒绝示例:

```
$ python3 scripts/symbolize_rip.py --elf build/tmp/headless-check/chromium-headless-shell \
      --syms build/tmp/chromium156.syms 0x11077CD4B7
拒绝:符号表与二进制不同源,给不出可信符号 ->
  符号表来源 dbgsym build-id=8f201a244f07b621(…/201a244f07b6213001f23b2a2700932d4dd3e1.debug),二进制 build-id=0de17064b8ccb458
对策:找到 build-id 匹配的 dbgsym(debuginfod / debian-debug 对应的 -dbgsym 包),用 --mksyms 重新生成符号表。

$ python3 scripts/symbolize_rip.py --profile 0de17064 0x1000015A4137 0x6FFFFFFFCE70
  拒绝 0x1000015a4137  共享库 / ld.so / 用户堆区间(>= 0x1000_0000_0000),本符号表不覆盖 —— 需要该库自己的 dbgsym,拒绝套主程序符号
  拒绝 0x6fffffffce70  用户栈区间(>= 0x600000000000),拒绝猜测
```

---

## 4. 修复前后对比(同一批地址)

对 `hs-dom10.log`(实际是 **chromium-headless-shell 156.0.8078.73-1**,
前置 FATAL `gin/v8_initializer.cc:640 Error loading V8 startup snapshot file`)。
“旧”= 老脚本(v150 符号表 + base 0x1100000000),“新”= `--log` 自动识别后的结果。
`+0x…` 是相对符号起点的偏移。

| 地址 | 旧(错) | 新(对) |
|---|---|---|
| `0x11077CD4B7` (ud2 崩点) | `ProfileNetworkContextService::UpdateSplitAuthCacheByNetworkIsolationKey()` | **`logging::LogMessage::Flush()+0xb57`** ← FATAL 打印路径里的 `IMMEDIATE_CRASH` |
| `0x11077CD6FD` (bt[1]) | `ProfileNetworkContextService::~ProfileNetworkContextService()` | **`logging::LogMessageFatal::~LogMessageFatal()+0xd`** |
| `0x11099565FA` (bt[2]) | `x11::PickBestVisuals(x11::Connection*, x11::VisualId&, x11::VisualId&)` | **`gin::V8Initializer::LoadV8SnapshotFromFile(...)+0x38a`** ← 与那条 FATAL 完全对应 |
| `0x1109956153` (bt[3]) | `x11::VisualManager::GetVisualInfo(...)` | **`gin::V8Initializer::LoadV8Snapshot(...)+0x203`** |
| `0x11071A4D4E` (bt[4]) | `toolbar_ui_api::…OmniboxActionTextInput_Data::Validate(...)` | **`content::ContentMainRunnerImpl::Initialize(...)+0x83e`** |
| `0x1102D11D59` | `std::__1::__hash_table<…net::AddressFamily…>::__emplace_unique_key_args<…>`(巨型模板) | **`headless::(anonymous namespace)::HeadlessChildMain(...)+0xe9`** |
| `0x110212E021` | `kSecurePaymentConfirmationHeaderCustomPath`(数据符号!地址在 `.text`) | **`_start+0x21`** |
| `0x1102CD113C` | `simdutf::westmere::implementation::convert_valid_utf8_to_utf16be(...)` | **`partition_alloc::internal::SchedulerLoopQuarantineBranch<…>::PurgeInternal(...)+0xa4c`** |
| `0x11061C995B` | `perfetto::internal::TrackEvent<…>::CallIfCategoryEnabled<…>` | **`content::SchedulerLoopQuarantineTaskObserver::DidProcessTask(...)+0x1b`** |
| `0x1000015A4137` (bt[2]) | “非 Chromium PIE 地址(共享库),跳过”(无说明、无后续线索) | 明确拒绝:**共享库/ld.so 区间,需要该库自己的 dbgsym** |

对照组(v150 日志 `c3-exe.log`,老脚本本来就对):

| 地址 | 旧 | 新 |
|---|---|---|
| `0x1108B3080F` | `logging::LogMessage::HandleFatal(...)+0x2df` | 同(并标注 `[ud2]`) |
| `0x110AB5F6B2` | `gin::V8Initializer::LoadV8SnapshotFromFile(...)+0x1b2` | 同 |
| `0x1107CC660F` | `content::ContentMainRunnerImpl::Initialize(...)+0x5bf` | 同 |

新结果的**内部一致性**本身就是证据:崩点 `LogMessage::Flush` → `~LogMessageFatal`
→ `V8Initializer::LoadV8SnapshotFromFile` → `LoadV8Snapshot` → `ContentMainRunnerImpl::Initialize`,
与日志里那条 FATAL 的调用路径严丝合缝;另一个块(bt[1..14])是
`SchedulerLoopQuarantineTaskObserver::DidProcessTask` → `TaskQueueImpl::NotifyDidProcessTask`
→ … → `WorkerThread::ThreadMain` 的调度线程栈,也是自洽的。

---

## 5. 之前哪些结论是错的

* **错的**:对 `hs-dom5..14.log`(headless-shell)给出的任何 chromium 函数名,
  包括 `SignedExchangeSignatureVerifier` 一类。它们来自 chromium 桌面版符号表,
  与 headless-shell 的地址空间无关。正确的例:`0x11077CD4B7` 是
  `logging::LogMessage::Flush` 里的 `ud2`(FATAL 崩溃点),
  `0x1102D11D59` 在 `headless::HeadlessChildMain`。
* **错的**:“主程序映射/基址是 `0x11067CD000`”。那是 ±16MB 截断走查的端点。
* **对的(之前被否定)**:`PIE_BASE = 0x1100000000` 本身正确;`0x1000_0000_0000`
  区间的地址确实不该套主程序符号表 —— 老脚本“跳过”的方向对,但只是跳过,
  没有说清楚“这里需要另一个 dbgsym”,现在明确了。

---

## 6. 用法(日常)

```bash
# 扫日志:自动认二进制 + 打印基址依据 + 每个崩溃块的前置 FATAL
python3 scripts/symbolize_rip.py --log build/tmp/hs-dom10.log

# 单个地址(profile 名或 build-id 前缀)
python3 scripts/symbolize_rip.py --profile 0de17064 0x11077CD4B7

# 自检(PASS/FAIL,退出码可用)
python3 scripts/symbolize_rip.py --selfcheck

# 新版本到来时:拿到 build-id 匹配的 dbgsym → 生成符号表 → 登记 profile
python3 scripts/symbolize_rip.py --mksyms /path/xx.debug -o build/tmp/xx.syms
python3 scripts/symbolize_rip.py --register --name xx --elf /path/binary --syms build/tmp/xx.syms \
        --debug /path/xx.debug --guest-path /opt/.../xx
```

**怎么找匹配的 dbgsym**:`readelf -nW <binary>` 取 build-id,
然后去 Debian debug 源挑同名但 revision 一致的 `-dbgsym` 包
(`deb.debian.org/debian-debug/pool/main/c/chromium/`)。
本次就是这么定位到 `chromium-headless-shell-dbgsym_156.0.8078.73-1` 的
(桌面版 `-1` 的 dbgsym 也在同目录)。

---

## 7. 遗留与建议

1. **桌面 chromium 装的是 `156.0.8078.73-1~deb12u1`**(build-id `d2ca4df7…`),
   而公开源里只有 `-1` 的 dbgsym(`8f201a24…`),两者**不是同一次构建**,
   段布局都不一样(`.text` 起点 `0x2d92000` vs `0x2da7000`)。
   → 要么把镜像里的桌面 chromium 换成 `-1`(其符号表 `build/tmp/chromium156.syms`
   已就绪),要么接受“桌面版 156 无法符号化”(工具会明确拒绝,不会再编)。
   当前的 headless-shell 工作不受影响:它跑的是 `-1`,符号表已配好。
2. **`0x1000_0000_0000` 区间的崩溃**(`r32-chromium2.log` 的
   `SIGSEGV rip=0x1000017F95A5` 就落在 ld.so/libc)需要各自的 dbgsym
   (`libc6-dbgsym` / `libc6` 对应的 `ld-linux`),现在工具会明确说明,不再假装知道。
3. 未改动 `src/`、未重写 `build/tmp/chromium156.syms`、未跑 QEMU、未 `git push`。
   `build/` 在 `.gitignore` 里,大符号表不会进 git。
