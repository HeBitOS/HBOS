# I18N-PLAN：系统级 UTF-8、多语言渲染与 locale 契约

> 状态：2026-11-14 第一轮落地（task-5）。本文是**结论 + 契约 + 里程碑**，
> 不是设计草稿。后续改动若与本文冲突，先改本文并说明理由。
> 关联：`docs/wiki/CODE_STYLE.md`、`docs/jindu.md`（R54/R55 的
> Wayland 现状）、`docs/wiki/LINUX_KDE_COMPAT.md`。

## 1. 结论摘要

| 项 | 结论 | 本轮状态 |
|---|---|---|
| (a) 统一 UTF-8 库 | 一份实现（`src/i18n/utf8.c`），严格 + 容错两套语义 | **已落地**，宿主单测覆盖 |
| (b) 宽字符/组合符/字素簇 | wcwidth 表统一（Unicode 16.0），退格按簇、组合符零宽、Emoji 双宽 | **已落地**（tty / flanterm） |
| (c) 字体回退 | 按脚本的静态回退链 + 码点缓存 | **数据结构与查表已落地**；`fb.c` 接线受限（见 §5.3） |
| (d) locale 契约 | LC_ALL > LC_CTYPE > LANG；UTF-8 名字即接受；无 locale 数据 | **已落地**（`src/i18n/locale.c`） |
| (e) 输入法 | fcitx5 / ibus **本轮不做**；前置缺口是 Wayland text-input + keymap + 会话总线 | 结论 + 里程碑（§6） |

**一句话**：HBOS 现在对"字符串是 UTF-8"这件事有唯一事实来源；能否**显示**
某语言取决于字体面，能否**输入**取决于输入法协议，后者本轮明确不做。

## 2. UTF-8 契约（全链路）

### 2.1 唯一实现与两套语义

`src/i18n/utf8.{h,c}` 是唯一实现，`src/lib/utf8.{h,c}` 退化为转发头/空翻译
单元（删掉实现是为了不再分叉）。两套解码语义按调用场景选：

- **严格** `utf8_decode()`：非法序列返回 0，零消费。用于校验、解析
  （文件名、协议字段、`.desktop` 文件）。判据：最短形式、非代理区、
  ≤ U+10FFFF、续字节完整。
- **容错** `utf8_decode_fffd()`：永远消费 ≥1 字节，非法输出 U+FFFD。
  用于渲染/行编辑——输入流不能因为一个坏字节停滞。

替换行为是**确定**的（按 Unicode maximal subpart）：

| 输入（十六进制） | 消费 | 输出 |
|---|---|---|
| `E4 B8`（截断的 3 字节） | 2 | 一个 U+FFFD |
| `80 80`（两个孤立续字节） | 1 / 1 | 两个 U+FFFD |
| `E0 80 80`（超长编码） | 1 / 1 / 1 | 三个 U+FFFD（第二字节越界，maximal subpart 只有首字节） |
| `F4 90 80 80`（> U+10FFFF） | 4 | 一个 U+FFFD |

这些行由 `tests/host/test_i18n.c` 逐条钉死，改行为必须同步改测试与本节。

### 2.2 各通路现状

| 通路 | 现状 | 契约 |
|---|---|---|
| tty 输入 | 行规程按字节收，编辑缓冲存原始 UTF-8；退格按**字素簇**删 | 不转码、不校验（Linux 语义）；只保证不把字符切一半 |
| tty 输出 | 字节透传；CR/LF 由 OPOST/ONLCR 处理 | 输出侧不做编码转换；宽度由渲染层算 |
| shell/工具输出 | 明文，printf 家族不感知编码 | 输出必须是合法 UTF-8；非法字节由渲染层变 U+FFFD |
| 文件名/VFS | 字节串，内核不校验 | 与 Linux 一致：合法 UTF-8 是**应用约定**，不是 VFS 约束 |
| GUI 文本 | 现有 `graphics.c` 自带 UTF-8 累积器（本轮未改，见 §2.3） | 迁移到 `i18n/utf8.h`，行为不变 |
| flanterm/VGA 终端 | 已改为严格校验 + 确定替换（`src/flanterm.c`） | 每个坏序列恰好一个占位、光标只前进一格 |

### 2.3 已知缺口（明确登记，不假装完成）

1. `src/graphics/graphics.c` 的 `console_putchar` 仍用自带的 `utf8_feed`：
   它接受 `C0/C1` 超长编码、把 `F5..F7` 当四字节首字节。**不在本轮 scope**
   （`src/graphics/**` 被任务边界禁止），建议下一轮替换为
   `utf8_decode_fffd()`；行为差异只在非法输入上。
2. `console_putchar` 对非 ASCII 的宽度判断是"Box Drawing/Block 1 列，其余
   2 列"的硬编码，Emoji 与组合符没接 `utf8_wcwidth()`。同属 graphics.c。
3. `fb.c` / `flanterm.c` 的网格模型一码点一格，组合附加符无法**叠画**在
   前一个字上：本轮做到"不占新格、不画豆腐块"，但没有合成字形。真正
   组合需要渲染层保留簇并做合成（里程碑 M3）。

## 3. 宽度与字素簇（wcwidth 语义）

`src/i18n/wcwidth.c` 的表由宿主 Python `unicodedata`（Unicode 16.0.0）
离线生成后内联，**不在启动时建表**：

- 零宽表 335 段：`Mn/Me/Mc/Cf`（减去 U+00AD 软连字符）+ Hangul Jamo
  元音/终声 `U+1160..U+11FF` + Emoji 肤色修饰符 `U+1F3FB..U+1F3FF` +
  Tag 字符 `U+E0020..U+E007F`。
- 双宽表 130 段：`East_Asian_Width ∈ {W,F}` 且已分配。
- 单码点 API 的返回：控制字符 -1、零宽 0、普通 1、东亚宽/全角/Emoji 2。
- **VS16 升级**：`U+2764`（❤）单码点是 1；`U+2764 U+FE0F` 组成的簇是 2。
  单码点 API 看不到这个上下文，`utf8_cluster_width()` 才做升级。
- **字素簇**：`utf8_cluster_next/prev` 遵循 UAX #29 的常用子集：
  Extend/ZWJ/VS16/Emoji 修饰符、区域指示符对（国旗）、CRLF、Hangul Jamo。
  不变量：`cluster_next(cluster_prev(off)) == off`，退格按这个边界删。

**终端限制（写进契约，避免下一个人以为是 bug）**：
`\b` 退格只移动一列，flanterm 不知道前一格是宽字符。tty 的 ECHOE 已经按
簇宽发多次 `"\b \b"`；应用自己写 `"\b"` 擦 CJK 仍然会偏一列——这是 VT 语义
的固有限制，KDE 的 Konsole 也一样，除非应用用 `\r`/重绘整行。

## 4. locale 契约（HBOS 侧该给 KDE/Qt 什么）

### 4.1 语义

`src/i18n/locale.c` 实现最小 `setlocale()`：

- 优先级：`LC_ALL` > `LC_<CATEGORY>` > `LANG` > `"C"`。
- **接受任何 UTF-8 名字**（`C.UTF-8`、`zh_CN.UTF-8`、`utf8`…），语义等价于
  C.UTF-8：字节级排序、C 数值格式、UTF-8 文本。**没有**翻译/月份名/排序表。
- `C`/`POSIX` 是 ASCII；`*.ISO-8859-*` 等一律拒绝（返回 NULL，状态不变）。
- `setlocale(LC_ALL, "")` 按环境变量初始化；`(NULL)` 查询；不支持的名字
  返回 NULL 而不是静默降级。
- 非线程安全；约定引导期/单线程调用。

### 4.2 给 KDE/Qt 的契约

| 必须 UTF-8 | 可以为 C.UTF-8 |
|---|---|
| `LANG` / `LC_CTYPE` / `LC_ALL` 的值 | `LC_NUMERIC`（Qt 用 `QLocale`/C 格式） |
| argv/envp 字节（内核不转码，原样透传） | `LC_TIME`（同上） |
| 文件路径（`QFile::encodeName`、fontconfig 模式） | `LC_COLLATE`（没有 collate 表，退字节序） |
| 剪贴板 `text/plain;charset=utf-8` | `LC_MONETARY` |
| Wayland `wl_data_offer` 的 MIME 文本 | `LC_MESSAGES`（没有 .mo 就英文） |
| D-Bus 字符串、KConfig、`.desktop` 文件 | |

具体到镜像/兼容层：

1. **HBOS 默认设 `LANG=C.UTF-8`**（`src/tools/app.c` 已如此），不设 `LC_ALL`
   ——`LC_ALL` 会覆盖用户选择，KDE 自己会按需设置。
2. 用户 `export LANG=zh_CN.UTF-8` 必须能覆盖默认值。本轮修了 `app.c` 的
   环境组装：同名变量现在**覆盖**默认槽位，而不是追加后被 `getenv` 忽略。
3. glibc 的 `setlocale` 在**镜像里的 libc** 里，不在 HBOS 内核里。若镜像
   没有对应 locale 数据，`setlocale(LC_ALL,"")` 会失败——这是镜像
   `locale-gen` 的责任，HBOS 不应假装成功。`C.UTF-8` 在 Debian glibc 中
   内置，作为默认是安全的。
4. **需要 linux_compat 侧确认/改动（只在汇报中提出，本轮未改）**：
   - syscall 层没有 locale 相关调用（Linux 本身没有），`execve` 的 envp
     必须逐字节透传、不去重、不排序（本轮已确认 `app.c` 侧只是组装）。
   - `/usr/lib/locale/locale-archive`、`/usr/share/locale/*/LC_MESSAGES`
     的访问必须走正常 VFS；只读 ext2 上缺文件要返回 ENOENT，不能返回 EPERM。
   - `nl_langinfo(CODESET)` 由 guest glibc 实现，兼容层无需干预。
   - 不要把 `LC_ALL` 注入到所有子进程；只在用户显式设置时透传。

## 5. 字体回退

### 5.1 数据结构（`src/i18n/font_fallback.c`）

三张静态表，**无分配**：

1. `script_ranges[]`（70 段，按 first 升序）：码点 → 脚本，二分查找。
2. `g_faces[]`（上限 8）：每个字体面声明自己覆盖的**脚本**，不逐字登记
   码点（CJK 面上千字，逐字表会吃掉启动内存）。
3. `g_cache[]`（64 项直接映射）：码点 → 能画出它的面；注册表变化时整体
   失效（字体面只允许引导期装配）。

回退顺序：同脚本面按注册顺序 → 主拉丁面兜底（CJK 字体缺生僻字、Emoji
面缺符号都靠它）。

### 5.2 查表成本

| 操作 | 成本 |
|---|---|
| `i18n_script_of` | ~7 次区间比较（70 段二分） |
| `i18n_font_chain` | 脚本命中后 O(面数) ≤ 8，结果 ≤ 4 |
| `i18n_font_face_for`（未命中缓存） | script_of + chain + 每候选面一次 lookup（CJK 面内是码点数组二分） |
| `i18n_font_face_for`（命中缓存） | O(1)，不增加 `i18n_font_lookup_probes()` |

单测用假字体面验证了：CJK 面优先、缺字回退拉丁面、缓存命中不重复探测。

### 5.3 接线状态与下一刀

- `fb.c` 的 `plot_char` 现在对 `grid[].c >= 256` 的单元直接返回，注释写着
  "CJK cell: pixels already correct"——像素由 `graphics.c` 的
  `cjk_render_at_cursor` 预先画好，grid 里存的是空格。**没有任何码点
  进入 fb.c 的绘制路径**，所以本轮无法在 fb.c 里接一条有价值的回退渲染，
  强行加钩子只会是死代码。
- 正确的接线点是 `graphics.c:console_putchar`：把"非 ASCII 一律
  `cjk_render_at_cursor(cp, !narrow)`"改成
  `face = i18n_font_face_for(cp); 用 face 的 glyph 渲染`。这需要改
  `src/graphics/**`（本轮边界禁止）。**提议给 Lead**：下一轮由
  graphics 的 owner 接，接口已就绪，调用点只有一处。
- 阿拉伯/希伯来本轮只做**字体选择**，不做 bidi 重排与 Arabic shaping：
  逻辑顺序存字节，渲染仍是 LTR。不要把它当"RTL 已支持"。

## 6. 输入法（fcitx5 / ibus）评估

### 6.1 两者共同的前置条件（HBOS 当前都缺）

| 前置 | fcitx5 | ibus | HBOS 现状 |
|---|---|---|---|
| Wayland `text-input` / `input-method` 协议 | 必需（或 X11） | 必需 | HIVE `wl_server` **没有** text-input 接口（`wl_protocol.h` 只有 data_device_manager 等 MVP） |
| `wl_keyboard.keymap`（xkb keymap） | 必需 | 必需 | 发的是 `NO_KEYMAP`（jindu R55 记录） |
| 会话 D-Bus | 配置/面板/引擎通信 | **硬依赖**（GDBus） | 没有 dbus-daemon、没有 session bus |
| 共享内存 + eventfd + poll/epoll | 有 | 有 | 已有（`/dev/shm` 内存覆盖层、epoll） |
| fork/exec、unix socket | 有 | 有 | 已有 |
| 图形面板 | cairo/pango | GTK | HIVE 没有 GTK/cairo 运行时 |

依赖体量：fcitx5 core 需要 `libfmt`、`libintl`、`libxkbcommon`、
`wayland-client/protocols`，面板要 `cairo/pango`；D-Bus 依赖在部分构建
配置下可削减（需实测，不写死）。ibus 的 `glib/gobject/gio` 是硬依赖，
依赖面明显更大。KDE 生态默认 fcitx5，选型上 fcitx5 更对齐。

### 6.2 结论

**本轮不做，也不建议现在做。** 理由不是"跑不起来"，而是**前置链路缺三块**：
text-input 协议、xkb keymap、会话总线。先做输入法会得到一个能启动但
无法把预编辑串送进 Chromium 的进程，属于无效投入。

替代路径（短期可用）：**剪贴板粘贴**。Chromium 的 CJK 输入在无 IME 时
唯一现实路径是 `Ctrl+V`，需要 HIVE 把 `wl_data_device` 的 selection
做成真的（当前是 MVP 桩），且验证 `text/plain;charset=utf-8` 往返。

### 6.3 里程碑（建议顺序）

| 里程碑 | 内容 | 验收 |
|---|---|---|
| M1 | `wl_keyboard.keymap` 发真实 xkb keymap（xkbcommon 数据） | Chromium 里 Shift/组合键正确 |
| M2 | `text-input-v3` 服务端：enable/commit/预编辑串 | 一个测试客户端能收预编辑并提交 UTF-8 |
| M3 | 字体回退接入 `graphics.c` + 组合字符合成 | CJK/Emoji/带调字符不再豆腐块 |
| M4 | 会话 D-Bus（最小 dbus-daemon 或兼容实现） | `dbus-send --session` 可用 |
| M5 | fcitx5 移植（先无面板，命令行切换引擎） | 在 Chromium 里输入中文 |
| M6 | 面板/主题（可选） | 托盘图标 |

## 7. 本轮改动清单与验证

| 文件 | 改动 |
|---|---|
| `src/i18n/utf8.{h,c}` | 新增：严格/容错解码、编码、校验、字素簇前进/后退 |
| `src/i18n/wcwidth.{h,c}` | 新增：Unicode 16.0 零宽/双宽表、簇宽、整串宽度 |
| `src/i18n/font_fallback.{h,c}` | 新增：脚本表、回退链、码点缓存、探测计数 |
| `src/i18n/locale.{h,c}` | 新增：环境优先级、最小 setlocale、UTF-8 判定 |
| `src/lib/utf8.{h,c}` | 实现迁走；头文件转发到 `i18n/utf8.h`（不保留第二份解码器） |
| `src/flanterm.c` | 严格校验（超长/代理区/越界→U+FFFD）、Emoji/组合符宽度走统一表；`mk_wcwidth` 保留为薄包装 |
| `src/tty.c` | 退格/^U 按字素簇删，ECHOE 按簇宽发 `"\b \b"` |
| `src/tools/app.c` | 用户 export 的同名环境变量覆盖默认值（LANG 可被用户选择） |
| `Makefile` | `src/i18n/*.c` 构建规则；新增 `i18n-test`；`utf8-test`/`exfat-test` 链接新实现 |
| `tests/host/test_i18n.c` | 新增宿主单测（a/b/c/d 全覆盖） |

验证命令与证据见本轮汇报（`make BUILD_DIR=build-i18n -j2` 零 error/
零 warning；`make BUILD_DIR=build-i18n i18n-test utf8-test exfat-test` 全绿；
`make BUILD_DIR=build-i18n check-headers` 全绿）。

**未验证**：QEMU 里的实际渲染效果（按团队规则只有 Lead 能跑 QEMU）；
`fb.c`/graphics.c 的回退接线；阿拉伯文 shaping；真实 xkb keymap。
