# XJ380 API 与 HBOS API 对标分析

> 参考规范：`XJ380_API_Spec_CPP_1_4.pdf`（XINGJI Interactive Software，2026/11/5，VERSION 1.4，171 页）
> 对标对象：HBOS 应用 SDK（`app/include/`）、用户态 libc（`src/user/libc/`）、HIVE 运行时（`HIVE/`）
> 本文性质：差异分析 + 补全方案。不含实现承诺；所有"补全方式"均为接口草案。

---

## 0. 结论摘要

| 维度 | XJ380 | HBOS | 判定 |
| --- | --- | --- | --- |
| 分层 | 5 层（编译层 / POSIX 版 / XAPI-TUI / XAPI-GUI / XNet）+ 2 个空章节 | 3 层（HAX syscall ABI / hax_widgets 工具包 / HIVE 运行时） | 层数不同但切面可对齐 |
| API 规模 | 234 个函数 + 78 个公开类型 | `hax.h` 43 个 `static inline` + `hax_widgets.h` 73 个 + `hax_anim.h` 10 个（非 inline，链接期符号）+ `hive.h` 1 个别名层 | HBOS 接口数约为其 54% |
| 覆盖 | — | 有 ≈29%（68/234）、部分 ≈21%（48/234）、缺 ≈50%（118/234） | 缺口中约 1/3 属"不适用" |
| 最大空缺 | — | 音频流（27）、IME（16）、剪贴板（9）、文本度量（11）、XNet 配置管理（12） | 集中在"跨应用系统服务" |
| 最大优势 | — | 控件树（18 类控件 + 布局 + 主题 + 焦点/命中/派发）、多窗口 v2 ABI、POSIX syscall 面 | XJ380 的控件是"窗口拥有的过程式 CRL"，HBOS 是应用侧 retain 树 |

**一句话结论**：XJ380 强在*系统服务*（音频/IME/剪贴板/网络配置），HBOS 强在*GUI 控件与窗口 ABI*。
补全顺序应是「先补纯用户态、零内核改动的缺口（文本度量、类型转换、图像解码），再补需要服务契约的跨应用设施（剪贴板 → 动画帧驱动 → 音频流）」。

---

## 1. 数据来源与方法

```sh
pdftotext -layout XJ380_API_Spec_CPP_1_4.pdf build/tmp/xj380.txt   # 8118 行 / 172 页分页
# 函数标题形如 "3-1-1 xapi_Output();"（正文）与 "3.1.1 xapi_Output();"（目录）
# 类型标题形如 "4-7-1 XAnimCurve 类型"
```

- **页码约定**：本文所有 `pNN` 均为 PDF 物理页序（`pdfinfo` 报告 171 页，`pdftotext` 输出 172 段，末段为空页），不是手册页脚编号。
- 函数清单由正文标题正则提取（去重后 234 个，与目录条目逐条核对一致）。
- 类型清单由正文 `NN-N-M Xxxx 类型` 标题提取，共 78 个。
- HBOS 侧全部结论均带 `文件:行号`，均可在当前工作树复核。

### 1.1 一个必须先说的结构事实

规范标题写作 "C\C++ 版"，但**内容没有任何 class / 继承 / 模板 / 命名空间**：

- 全部 API 是 C 风格自由函数 + POD 结构体；
- 唯一的"对象"是**不透明句柄**，且每个子系统各自定义了一套 64 位句柄类型（`HDLE`、`XAudioHandle`、`XAudioPlayer`、`XMp3Handle`、`XAnimHandle`、`XNetHandle`、`XTlsHandle`、`XHttpHandle`）；
- 每个子系统还各自定义了一套 32/64 位整数别名（`XAudioU32`、`XAnimI32`、`XClipU64`、`XImeU32`、`XNetResult`…），语义完全相同、仅前缀不同（p15–p17 的 `typedef` 块）。

对 HBOS 的意义：**不存在"C++ 面向对象 API 需要抽象基类"这类移植问题**。对标是纯粹的函数族 + 结构体布局对标，这也是 HBOS 能用纯 C 头文件（`hax.h` 全部是 `static inline`）承接的原因。

---

## 2. XJ380 API 分层摘要

### 2.1 层总表

| 层 | 名称 | 页范围 | 函数 | 类型 | 说明 |
| --- | --- | --- | --- | --- | --- |
| L0 | 编译/环境/专有类型（Ch1） | p13–p19 | 0 | 30 别名 | 头文件选择表、编译参数、30 个基础别名、5 用户 7 权限模型、`__XJ380_OS__`、`/config` 规范、`settings.json` 键表 |
| L1 | POSIX 版（Ch2） | p20 | 0 | 0 | **全章仅一句话**："遵守且兼容一部分 POSIX 标准"。实质内容 = 系统自带 libc |
| L2 | XAPI 无 GUI（Ch3） | p21–p64 | 90 | 19 | 10 节：文本 I/O、文件、类型转换、进程、当前信息、运行、内存、音频、权限、标记文本解析 |
| L3 | XAPI GUI（Ch4） | p65–p130 | 89 | 36 | 11 节：窗口、绘图、图片、消息、framebuffer、控件、动画、剪贴板、IME、截屏、通知 |
| L4 | XNet 网络（Ch5） | p131–p169 | 55 | 23 | 4 节：socket、DNS/配置、TLS、HTTP |
| L5 | BridgeEngine BAPI（Ch6）/ Stardust UI（Ch7） | p170 / p171 | 0 | 0 | **两个空章节**，仅"简介"二字，无任何可对标 API |

合计：234 函数 / 78 类型。

### 2.2 L2 XAPI 无 GUI（Ch3，90 函数 / 19 类型）

| 节 | 模块 | 页 | 函数 | 类型 | 要点 |
| --- | --- | --- | --- | --- | --- |
| 3.1 | 文本输入输出 | p21–p23 | 8 | — | `Output/Input/Getline/Getch/EndLine/PrintLine/Printf/OutputSerial`；`xapi_Input` 要求 ≥1024 字节可写缓冲区 |
| 3.2 | 文件操作 | p23–p27 | 12 | `XFILE`(p23)、`DirNode`(p24) | 返回 `UINT64`（0 成功 / 非 0 错误），非 errno 约定；`xapi_FileDialog`(p27) 是 GUI 能力却放在 TUI 层 |
| 3.3 | 类型转换 | p28–p29 | 5 | — | `xcr_char2int/int2char/hex2char`、`toRGB/toRGBA`；手册自述"严格意义上不算 API" |
| 3.4 | 进程与线程 | p30–p32 | 5 | `XapiTaskInfo`(p30) | `Fork/Execve/Exit/GetTaskList/KillProcess`；任务信息含 `task_level/thread_count/window_count/cpu_runtime_ticks` |
| 3.5 | 当前信息 | p33–p34 | 7 | `UserInfo`(p32)、`TimeType`(p32) | 系统版本、秒级与纳秒时间、当前用户、CPU 型号、内存量 |
| 3.6 | 运行 | p35–p37 | 8 | `XapiSystemStats`(p35) | `Broken`（崩溃上报）、`Sleep(ms)`、`Run/RunArgs/SpawnInherited`（**启动即降为 Visitor 权限**）、`PowerAction` |
| 3.7 | 内存 | p37–p38 | 3 | — | `AllocateMemory/FreeMemory/MapMemory`；MapMemory 带 `flags` |
| 3.8 | 音频 | p39–p54 | **27** | 6 | 三个子族：PCM 流 13（`AudioOpen/Write/Read/Control/Query/Wait/SetVolume/SetMute/Close` 等）、播放器 8、MP3 解码 6；全部 `size/version` 双字段版本化 |
| 3.9 | 权限 | p55–p58 | 6 | `UserPermisson`(p55) | 7 个 bool：R/W/CS/NV/SV/SR/SW；`LiftPermissions(UserType)` 需密码 |
| 3.10 | 标记文本解析 | p58–p64 | 9 | `XXml*`(3)、`XJson*`(3) | XML：DOM（node/attribute/firstChild/nextSibling）；JSON：value/object/array 访问器 |

### 2.3 L3 XAPI GUI（Ch4，89 函数 / 36 类型）

| 节 | 模块 | 页 | 函数 | 类型 | 要点 |
| --- | --- | --- | --- | --- | --- |
| 4.1 | 创建窗口 | p65–p67 | 5 | `XWINDOW`(p65) | 窗口类型宏：`XWIN_NORMAL/FRAME_OFF/FULL_SCR/TYPE_MASK 0x0f/SETTABLE_RESIZEABLE 0x80`（p65–66）；**窗口被用户关闭后对应线程结束** |
| 4.2 | 绘图 | p67–p79 | 20 | `XPaintControl`(p69)、`XTextStyle`(p71)、`XTextMetrics`(p72)、`XTextRect`(p72) | 3 直接绘制（点/线/矩形）+ **4 统一绘制事务**（`BeginPaint/EndPaint/CancelPaint/PaintControls`）+ **11 文本** + SVG + FontAwesome |
| 4.3 | 插入图片 | p80–p82 | 5 | `XCOLORA`(p80) | BMP/PNG/JPEG/GIF/HDR；`LoadPicture` 直接解到 `XCOLORA*` 缓冲 |
| 4.4 | 消息处理 | p83–p90 | 0（+1 非 `xapi_` 前缀：`SetMsgPrcor`） | — | 21 个消息号（`MSG_CHAR`…`MSG_EDIT`）+ 33 个 `XKEY_*`；回调签名 `void (*MsgPrcor)(UINT64 Type, UINT64 hData, UINT64 lData)` |
| 4.5 | framebuffer | p91–p93 | 6 | `XCOLOR`(p91) | 读/写缓冲（含 alpha 变体）、整窗刷新、局部刷新 |
| 4.6 | 控件 | p94–p99 | 15 | `RightMenuItem`(p95) | 按钮/右键菜单/文本输入框/单模开关/滚动条；**过程式**：`xapi_Button(wnd, CRLid, x, y, text)`，控件归属窗口、由 `CRLid` 标识 |
| 4.7 | 动画 | p100–p104 | 4 | `XAnimCurve/XAnimDesc/XAnimSnapshot`(p100–101) | 描述式：`target`/`window`/`tag`/`duration_ms`/`delay_ms`/`curve`/`from[]/to[]`；快照回传 `progress/values[]`，通过 `MSG_ANIMATION` 通知 |
| 4.8 | 剪切板与文本选择 | p104–p112 | 12 | 8 个 `XClip*`/`XTextSelection` | 文本/通用数据/图片/文件/路径 五类格式；异步粘贴：`RequestPaste`→`MSG_PASTE`→`GetPasteInfo`→`ReadPaste`→`FinishPaste` |
| 4.9 | 输入法 | p112–p126 | **16** | 16 个 `XIme*` | 双代并存（V1 + V2）；注册/枚举/状态/激活/挂接/事件/回复/候选窗 UI/模式/上下文/用户数据 |
| 4.10 | 截屏 | p127–p128 | 2 | — | `CaptureScreen`（带容量探测）+ `SetSecret`（防截屏） |
| 4.11 | 通知和重点 | p128–p130 | 4 | `XApiNotificationRequest`(p128) | 应用消息、通知回调、系统通知（含标题/正文/图标/两个动作按钮）、重点栏高亮 |

### 2.4 L4 XNet（Ch5，55 函数 / 23 类型）

| 节 | 模块 | 页 | 函数 | 要点 |
| --- | --- | --- | --- | --- |
| 5.1 | 原生网络 | p131–p146 | 23 | 句柄式 socket；`XNetIoOptions{timeout_ms, cancel}` 统一注入超时与取消；`NetWaitMany` 多对象等待 |
| 5.2 | DNS 与配置 | p147–p156 | 14 | 异步解析（Start/GetResult）、网卡枚举、接口/地址/DNS 配置读写、`SaveConfig/ReloadConfig/GetConfigStatus`、接口变更 watch |
| 5.3 | TLS | p157–p162 | 8 | 句柄式 `TlsConnectStart/TlsWait/Send/Receive/Shutdown/Cancel/Close`；`XTlsPeerInfo` 回传协议/密码套件/验证标志 |
| 5.4 | HTTP | p163–p169 | 10 | 请求描述 `XHttpRequestDesc{url, method, headers[], body, max_redirects, total_timeout_ms, flags}`；流式 `HttpRead`；另有便捷 `xapi_httpGet` |

### 2.5 交叉割面（按命名空间前缀）

| 前缀 | 函数数 | 所属层 | 特征 |
| --- | --- | --- | --- |
| `xapi_`（无子系统前缀） | 156 | L2+L3 | 核心 TUI + GUI |
| `xapi_Audio*` / `xapi_Mp3*` | 27 | L2 | 独立 `XAudioResult` 错误域，`size/version` 版本化结构 |
| `xapi_Animation*` | 4 | L3 | 描述式 + 快照查询 |
| `xapi_Clipboard*` / `xapi_ReadPaste` / `xapi_TextInput*` | 12 | L3 | 异步粘贴事务 |
| `xapi_Ime*` | 16 | L3 | 双代 V1/V2 |
| `xapi_Net*` | 29 | L4 | 句柄 + IoOptions |
| `xapi_Tls*` | 8 | L4 | 句柄 |
| `xapi_Http*` | 9 | L4 | 句柄 + 流式 |

---

## 3. HBOS 侧现状基线

### 3.1 应用 SDK 三个层次

| 文件 | 行数 | 内容 | 定位 |
| --- | --- | --- | --- |
| `app/include/hax.h` | 581 | 类型别名 `HI8..HU64`/`HCOLOR`（47–55）、TUI 便捷层（96–124）、文件/HTTPS 便捷（129–198）、单窗口 canvas `hax_gui_*`（222–312）、`HAX_EV_*` 事件（337–344）、单窗口 `hax_win_*`（347–390）、**多窗口 v2 ABI**（395–573：能力位、`HAX_WIN2_*` 十个操作、`HAX_DRAW_*` 四种绘制命令、`hax_window_{query,create,get_state,set_title,set_geometry,set_state,draw,present,poll,close}`） | 系统调用 ABI + 便捷封装 |
| `app/include/hax_widgets.h` | 1514 | 18 类控件（49–68）、控件标志（72–76）、`hax_widget_t`/`hax_ui_t`/`hax_ui_theme_t`（95–162）、布局（164–214）、深浅主题（216–250）、增删改（252–730）、焦点与 UTF-8（741–814）、文本框选区（816–876）、键盘（899）、命中与激活派发（987–1072） | 纯用户态 retain 控件树 |
| `app/include/hax_anim.h` | 143 | 7 可动画属性（46–54）、8 条整数缓动曲线（62–76）、`hax_anim_t`（83–94）、`declare/set/get/to/to_many/tick/running/finish`（80–121）、`hax_ease_apply`/`hax_now_ms`（127–130） | 纯用户态属性过渡 |
| `app/include/hive.h` | 144 | HIVE 品牌层：版本（13–19）、`hive_*` 类型/控件/事件别名（25–66） | 公开首选头 |
| `src/user/libc/` | 50+ 文件 | `stdio/stdlib/string/unistd/fcntl/dirent/sys/stat/sys/mman/sys/wait/socket/poll/pthread/dlfcn/netdb/getopt/termios?/time/errno/limits/setjmp/math` | POSIX 子集 |
| `src/user/libc/syscall.h` | — | 约 130 个 `HBOS_SYS_*` 编号（77–末尾），含 `FORK/EXECVE/WAITPID/KILL/MMAP/MUNMAP/MPROTECT/SOCKET…/EPOLL_*/POLL/WIN2/HTTPS_GET*/SHUTDOWN/REBOOT` | 与 Linux 兼容层共用 |

### 3.2 HIVE 运行时（**应用不可直接包含**）

`HIVE/src/gui/{compositor,wm,winsrv,effects,service,gui_dirty}.c`、`HIVE/src/graphics/gui_font.h`。
`docs/REPOSITORY_SPLIT_BOUNDARIES.md:33–50` 强制依赖方向：应用 → HAX SDK / Window ABI；GUI → Kernel UAPI / `gui_service` ABI；**内核不依赖 GUI 或应用**。
`docs/HIVE_DESKTOP_API.md:193` 明确：应用不得包含 `gui_state.h`、`gui_draw.h`、`gui_app.h`、`wm.h`、`winsrv.h`，不得访问 framebuffer。

> 这条边界直接决定了本文所有"补全方式"：能放 `app/include/` 的（纯头文件/内联）即时可做；需要 `gui_service` 或 syscall 的必须走主代理域。

### 3.3 已登记的自认缺口（与本文重合项）

- `docs/HIVE_DESKTOP_API.md:85`：「当前键盘事件仍只直接输入 ASCII，完整输入法与系统剪贴板协议后续增加」→ 对应本文 4.8 / 4.9。
- `docs/LINUX_KDE_COMPAT.md:256–306`「当前明确缺口」：vDSO、clone3 进程形态、AF_INET `getpeername`、inotify 覆盖、符号链接、D-Bus/Wayland/DRM → 影响本文 5.x 判定，**不重复登记**。

---

## 4. 映射表

状态：✅ 有等价能力　◐ 部分（语义/形态不同，需适配）　❌ 缺

### 4.1 L0 基础设施（Ch1，p13–p19）

| XJ380 概念 | 页 | HBOS 对应物 | 状态 | 差距 |
| --- | --- | --- | --- | --- |
| `INT8..UINT64` / `WSTR` / `HDLE` | p15–17 | `hax.h:47–55` `HI8..HU64`、`HCOLOR` | ◐ | 命名不同（前缀 H vs 无）；无 `WSTR`/`HDLE` 别名，XJ380 源码无法直接编译 |
| 回调 `MsgPrcor(Type,hData,lData)` | p17 | `hax.h:387` `hax_win_poll(int ev4[4])` / `hax.h:560` `hax_window_poll` | ◐ | XJ380 是注册式回调；HBOS 是拉取式事件队列（`HAX_EV_KEY/MOUSE/CLOSE/RESIZE/MOVE/FOCUS/STATE`） |
| 5 用户 / 7 权限 | p18 | 无（`HBOS_SYS_GETUID` 恒返回固定值，无 `/users/`） | ❌ | HBOS 无多用户模型；见 §6「不适用」 |
| `__XJ380_OS__` 环境识别宏 | p18 | 构建期 `.hax` manifest（`HAX_META_MAGIC`，`hax.h:67`） | ◐ | 判据不同但可用（编译期/打包期识别） |
| `/config/<组件>/` 规范 | p18 | `src/lib/json.h:67–104`（内核侧主题/种子配置）+ `/etc` | ◐ | 无统一全局配置目录约定；JSON 解析器仅内核侧、仅扁平对象 |
| `settings.json` 键表 | p19 | `src/ui/theme.c` 主题种子 | ❌ | 无等价"系统设置单一真相源"；键表 16 项无对应 |

### 4.2 L1 POSIX 版（Ch2，p20）

| XJ380 概念 | 页 | HBOS 对应物 | 状态 | 差距 |
| --- | --- | --- | --- | --- |
| 「兼容一部分 POSIX」 | p20 | `src/user/libc/`（50+ 文件）+ `src/linux_compat.c`（约 130 syscall） | ✅ | HBOS 的 POSIX 面**远大于** XJ380 该章（该章 0 函数、0 内容） |

> Ch2 没有可比对象。反而 HBOS 的 `docs/LINUX_KDE_COMPAT.md` 已是该层更完整的实现记录。

### 4.3 L2 TUI 层（Ch3，90 函数）

| XJ380 函数族 | 页 | HBOS 对应物 | 状态 | 差距 |
| --- | --- | --- | --- | --- |
| `xapi_Output/PrintLine/Printf/EndLine` | p21–23 | `stdio.h:48` `printf`、`:59` `puts`、`:41` `fputc` | ✅ | — |
| `xapi_Input/Getline/Getch` | p21–22 | `stdio.h:43` `fgets`、`:57` `getchar`、`hax.h:105/113` | ✅ | `xapi_Input` 要求 ≥1024B 可写缓冲，`fgets` 需显式长度（语义更安全） |
| `xapi_OutputSerial` | p23 | 无 | ❌ | 无用户态串口通道（调试走控制台） |
| `xapi_OpenFile/CloseFile` | p24 | `open/close`（`syscall.h` `HBOS_SYS_OPEN/CLOSE`） | ◐ | XJ380 返回带 `length/buffer` 的 `XFILE*`（**整文件入内存语义**）；HBOS 是 fd |
| `xapi_ReadFile/WriteFile` | p26–27 | `pread/pwrite`、`hax.h:129/143` | ✅ | 参数序为 `(name,buf,size,offset)`，可内联适配 |
| `xapi_SearchFile/SearchFile_freem` | p24–25 | `dirent.h:44–53` `opendir/readdir/closedir` | ✅ | XJ380 一次返回 `DirNode[]`+count 并需显式 `_freem`；HBOS 是游标式 |
| `xapi_Mkdir/Rmdir/CreateFile/DeleteFile/RenameFile` | p25–27 | `HBOS_SYS_MKDIR/RMDIR/UNLINK/RENAME`（`unistd.h:25/34/47`） | ✅ | — |
| `xapi_FileDialog` | p27 | 无（`HIVE/app/files.c` 是独立应用，未服务化） | ❌ | 无"文件选择器服务"，跨应用不可调用 |
| `xcr_char2int/int2char/hex2char` | p28 | 无（`atoi`/`snprintf` 可替代） | ❌ | 纯语法糖，5 行可内联补齐 |
| `toRGB/toRGBA` | p28–29 | `HCOLOR` = 0xRRGGBB（`hax.h:55`） | ◐ | 需 `0xAARRGGBB` ↔ `XCOLORA{u8 r,g,b,a}` 互转 |
| `xapi_Fork/Execve/Exit` | p30–31 | `HBOS_SYS_FORK/EXECVE/EXIT`（`unistd.h:39`、`hax.h:121`） | ✅ | — |
| `xapi_GetTaskList` | p31 | 无（`HIVE/app/taskmgr.c` 只能按 PID 发信号，不能枚举） | ❌ | 无用户态任务枚举 syscall |
| `xapi_KillProcess` | p32 | `kill`（`HBOS_SYS_KILL`）、`hive_process_signal` | ✅ | — |
| `xapi_GetSystemVersion` | p33 | `uname`（`HBOS_SYS_UNAME`） | ✅ | — |
| `xapi_GetTime/GetTimeNano/GetTimeX` | p33–34 | `time`、`clock_gettime`、`localtime_r`（`time.h`） | ✅ | XJ380 自造 `TimeType`（同 `struct tm`），可直映射 |
| `xapi_GetCurrentUser` | p33 | 无 | ❌ | 无用户模型（见 §6） |
| `xapi_GetCpuModel` | p34 | 无公开接口 | ❌ | 内核可读 CPUID，无导出 |
| `xapi_GetMemorySize` | p34 | 无公开接口（无 `/proc/meminfo`） | ❌ | 同上 |
| `xapi_Broken` | p35 | 内核故障路径（`src/core/**`，应用不可调用） | ❌ | 无"应用崩溃上报"契约 |
| `xapi_Sleep(ms)` | p35 | `usleep`（`unistd.h:21`，微秒） | ◐ | 单位不同（ms vs µs） |
| `xapi_Run/RunArgs/SpawnInherited` | p36–37 | `execve/execvp`（`unistd.h:39–40`）、fork+execve | ◐ | XJ380 会**降权为 Visitor**并保留父身份；HBOS 无权限层级，等权 |
| `xapi_PowerAction` | p37 | `HBOS_SYS_SHUTDOWN/REBOOT`（`syscall.h` 枚举内） | ◐ | 无统一"电源动作"入口与权限校验 |
| `xapi_AllocateMemory/FreeMemory` | p37–38 | `malloc/free`（`stdlib.h`） | ✅ | — |
| `xapi_MapMemory(ptr,size,flags)` | p38 | `mmap`（`sys/mman.h:26`） | ◐ | XJ380 flags 语义未见详述；HBOS 需 POSIX `prot/flags` |
| `xapi_Audio*`（PCM 流 13） | p42–47 | 仅内核 `src/ac97.c` + `src/wav.c` + `src/tools/audio.c`（`playwav` 内核工具） | ❌ | **无用户态音频 API**：无设备枚举、无流句柄、无音量/静音、无查询 |
| `xapi_AudioPlayer*`（8） | p48–p51 | 无 | ❌ | 无播放器对象 |
| `xapi_Mp3*`（6） | p51–p54 | 无（只有 `src/wav.c`） | ❌ | 无 MP3 解码器 |
| `xapi_QueryFileAccess` 等权限 6 项 | p55–58 | 无 | ❌ | 无文件访问权限模型（见 §6） |
| `xapi_parseXml/freeXmlNode/freeXmlTree` | p59–60 | 无 | ❌ | 全树无 XML 解析器 |
| `xapi_parseJson/…/jsonArrayAt`（6） | p62–64 | `src/lib/json.h:85–97` | ◐ | 仅内核侧；仅扁平对象（`parse_object`），**无数组、无泛型 `XJsonValue`、无 SDK 暴露** |

**Ch3 小计：✅31 / ◐10 / ❌49**

### 4.4 L3 GUI 层（Ch4，89 函数）

| XJ380 函数族 | 页 | HBOS 对应物 | 状态 | 差距 |
| --- | --- | --- | --- | --- |
| `xapi_CreateWindow/CloseWindow/GetWindowSize` | p65–67 | `hax_window_create/close/get_state`（`hax.h:505/570/517`）；单窗口 `hax_win_open/close/info`（`hax.h:347/388/352`） | ✅ | XJ380 用 `HDLE*` 出参 + `XWINDOW{sets}` 位域；HBOS v2 用 `struct_size`+ABI 版本 + 能力协商（更严格） |
| `xapi_SetWindowTitle` | p66 | `hax_window_set_title`（`hax.h:527`） | ✅ | v1 单窗口无此能力 |
| `xapi_SetIcon(path)` | p66 | 构建期 `.haxicon` 段（`hax.h:99–103`、`hax_app_icon_at`） | ◐ | 只能构建期指定，运行期不能换图标 |
| `xapi_DrawPoint` | p67 | `hax_gui_pixel`（`hax.h:259`） | ◐ | v2 窗口绘制命令只有 CLEAR/FILL/TEXT/ARGB（`hax.h:426–431`），**无单像素 op** |
| `xapi_DrawLine` | p68 | `hax_gui_line`（`hax.h:273`） | ✅ | 同上，v2 需用 FILL 自行栅格化 |
| `xapi_DrawRect(color,fill)` | p68 | `hax_gui_rect`（`hax.h:232`）、`hax_gui_frame`（:264）、`HAX_DRAW_FILL` | ✅ | — |
| `xapi_BeginPaint/EndPaint` | p69–70 | `HAX_WIN2_DRAW_BATCH`（`hax.h:416`）+ `hax_window_draw`（:541）/`hax_window_present`（:555） | ◐ | HBOS 是"命令数组提交"，非"临时表面句柄"；`EndPaint` 返回 `-EAGAIN`（窗口尺寸变化）的约定无对应 |
| `xapi_CancelPaint` | p70 | 无（不提交即等价） | ✅* | 语义可由"丢弃命令数组"达成，无需独立 API |
| `xapi_PaintControls` | p70 | 无 | ❌ | HBOS 控件在应用侧 retain（`hax_ui_t`），服务端不持有控件布局 → **该能力属架构差异，不建议补** |
| `xapi_DrawText/DrawSWText` | p73–74 | `hax_gui_text`（`hax.h:237`）、`HAX_DRAW_TEXT` | ◐ | HBOS 只有位图字体的整数 `scale`，无字号/字体/锚点 |
| `xapi_CalcTextWidth` | p74 | 无 | ❌ | 控件居中/折行无法精确排版 |
| `xapi_GetTextMetrics`（ascent/descent/line_gap/x_height/ch_width…） | p74–75 | 无 | ❌ | `XTextMetrics` 12 个度量字段零对应 |
| `xapi_GetTextPosition`（字节偏移→x） | p75 | 无 | ❌ | 文本框光标定位现在靠等宽假设 |
| `xapi_DrawTextEx/DrawTextBaseline/DrawTextTop/DrawMonoTextTop/DrawTextInRect`（带裁剪与基线） | p76–78 | 无（`hax_gui_text` 无裁剪矩形） | ❌ | 5 个变体全是"基线/裁剪/等宽"组合，属同一缺口的 5 个切面 |
| `xapi_DrawTextl`（回传宽度） | p73 | 无 | ❌ | 同上 |
| `xapi_DrawSvg` | p79 | 仅浏览器后端（`HIVE/src/gui/browser_backend.c`），应用 SDK 无 | ❌ | 无 SVG 光栅化对外接口 |
| `xapi_DrawFA`（FontAwesome 图标字体） | p79 | 无 | ❌ | HIVE 用 PNG 图标瓦片，非图标字体 |
| `xapi_DrawBMP/PNG/Picture` | p80–81 | `HAX_WIDGET_IMAGE` 收裸像素（`hax_widgets.h:431`）；BMP 解码仅 `HIVE/app/imgview.c:24`（未压缩 24/32 位） | ◐ | **SDK 无任何图像解码器**；PNG/JPEG/GIF/HDR 全缺 |
| `xapi_GetPicSize/LoadPicture` | p82 | 无 | ❌ | 无法不绘制就读尺寸/解码到内存 |
| `xapi_WriteBuffer/WriteBufferA` | p92–93 | `HAX_DRAW_ARGB`（`hax.h:430`）、`hax_win_blit`（:372） | ✅ | — |
| `xapi_ReadBuffer/ReadBufferA` | p91–92 | 无 | ❌ | **无 framebuffer 回读** |
| `xapi_RefreshWindow/RefreshPartWindow` | p93 | `hax_window_present` / `hive_window_present_rect`（脏矩形） | ✅ | HBOS 更细（`HAX_WIN_CAP_DIRTY_RECT`） |
| `xapi_Button/ButtonEmp/DeleteButton` | p94–95 | `hax_ui_add_button`（`hax_widgets.h:324`）、`hax_ui_remove_widget`（:623） | ✅ | **形态不同但能力更全**：HBOS 是应用侧 retain 控件树 |
| `xapi_RegisterRightButtonMenu/DeleteRightButtonMenu` | p95–96 | `hax_ui_add_menu`（`hax_widgets.h:423`） | ◐ | HBOS 是普通菜单控件，无"窗口级右键上下文菜单绑定" |
| `xapi_PutTextInputBox/GetTextInputBox/DeleteTextInputBox` | p96–97 | `hax_ui_add_textbox`（:331）+ `buffer/cap/cursor` 字段（:126–132） | ✅ | HBOS 缓冲由应用提供，XJ380 由系统持有并返回 `UINT64 id` |
| `xapi_PutSwitch/SetSwitch/DeleteSwitch` | p97–98 | `hax_ui_add_toggle`（:470）/`add_checkbox`（:348）、`hax_ui_set_value`（:701） | ✅ | — |
| `xapi_Put{V,H}ScrollBar/DeleteScrollBar/SetScrollBarPosition` | p98–99 | `hax_ui_add_scrollbar`（:410）、`hax_ui_set_value`、`hax_ui_scrollbar_value_at`（:1015） | ✅ | — |
| `xapi_AnimationStart` | p102 | `hax_anim_declare`（`hax_anim.h:80`）+ `hax_anim_to`（:99） | ◐ | **无窗口/句柄绑定**：`XAnimDesc` 带 `window`/`tag`，HBOS 动画是应用内纯数学对象 |
| `xapi_AnimationControl` | p103 | `hax_anim_finish`（:121） | ◐ | 无 pause/resume 操作码 |
| `xapi_AnimationQuery` | p103 | `hax_anim_get`（:89）、`hax_anim_running`（:118） | ◐ | 无 `XAnimSnapshot{elapsed_ms,progress,sequence}` 聚合快照 |
| `xapi_AnimationRelease` | p103 | 无（栈对象，无句柄） | ✅* | 生命周期由调用方持有，无需 release |
| `xapi_ClipboardSetText/SetData` | p107–108 | 无系统剪贴板 | ❌ | 只有应用内选区（`hax_widgets.h:816–876`） |
| `xapi_ClipboardQuery/Attach` | p109 | 无 | ❌ | 无所有者/变更修订号（`XClipInfo.revision`） |
| `xapi_RequestPaste/GetPasteInfo/ReadPaste/FinishPaste` | p110–111 | 无 | ❌ | 无异步粘贴事务，无 `MSG_PASTE` |
| `xapi_ClipboardClaimMove` | p111 | 无 | ❌ | 无"移动/剪切"所有权转移 |
| `xapi_TextInputGetSelection/SetSelection/SetFlags` | p111–112 | `hax_ui_textbox_selection`（:816）、`hax_ui_textbox_select`（:833）、`hax_ui_textbox_select_all`（:843） | ◐ | 选区端点语义一致（UTF-8 字节偏移 + 码点钳制），但无 `flags`（密码/只读等集中位） |
| `xapi_Ime*`（16 项） | p112–126 | 无 | ❌ | 键盘事件目前只有 ASCII（`docs/HIVE_DESKTOP_API.md:85`） |
| `xapi_CaptureScreen` | p127 | 无对外接口 | ❌ | 无截屏 syscall |
| `xapi_SetSecret` | p128 | 无 | ❌ | 无内容保护/防截屏层 |
| `xapi_SendAppMessage` | p129 | 无 | ❌ | 无应用间消息投递 |
| `xapi_SetNotifyPrcor/SendNotification` | p129 | 无（无系统托盘/通知中心） | ❌ | 无通知 UI 与回调 |
| `xapi_HighlightNotice` | p130 | 无 | ❌ | 无"重点栏" |

**Ch4 小计：✅22 / ◐18 / ❌49**

### 4.5 L4 XNet 层（Ch5，55 函数）

| XJ380 函数族 | 页 | HBOS 对应物 | 状态 | 差距 |
| --- | --- | --- | --- | --- |
| `NetOpen/Bind/Listen/Accept/Connect/Send/Receive/SendTo/ReceiveFrom/Shutdown/Close` | p138–146 | `socket/bind/listen/accept/connect/send/recv/sendto/recvfrom/shutdown/close`（`socket.h`、`HBOS_SYS_*`） | ✅ | HBOS 是 BSD socket；XJ380 是句柄 + `XNetIoOptions` |
| `NetSetTcpOptions/GetTcpOptions` | p144 | `setsockopt/getsockopt`（`HBOS_SYS_SETSOCKOPT/GETSOCKOPT`） | ◐ | `XNetTcpOptions` 把 no_delay/keepalive×4 打包进一个结构；BSD 需逐项 setsockopt |
| `NetSetUdpOptions/GetUdpOptions` | p145 | `setsockopt(SO_BROADCAST)` | ◐ | 同上 |
| `NetWaitMany` | p146 | `poll`、`epoll_*`（`HBOS_SYS_POLL/EPOLL_*`、`poll.h`、`sys/epoll.h`） | ◐ | `XNetWaitItem{object,events,ready,tag}` 可映射到 `epoll_event.data.u64` |
| `NetQuery` | p143 | `getsockname/getpeername` | ◐ | 见 `docs/LINUX_KDE_COMPAT.md`：AF_INET 的 `getpeername` 仍缺 |
| `NetSendAll` | p142 | 无（应用自行循环 `send`） | ◐ | 易补（内联循环） |
| `NetParseAddress/FormatAddress` | p137 | `inet_pton/inet_ntop`（若在 `netdb.h`） | ◐ | 语义等价，命名不同 |
| `NetGetCapabilities` | p131 | 无 | ❌ | 无后端能力探测（`max_objects/max_transfer_bytes/features`） |
| `NetCreateCancel/SignalCancel` | p146 | 无 | ❌ | 无跨对象取消令牌（HBOS 用 `O_NONBLOCK`+`poll`） |
| `NetResolveStart/GetResult` | p151 | `getaddrinfo/gethostbyname`（`netdb.h`） | ◐ | 只有阻塞式，无异步句柄 |
| `NetEnumInterfaces/GetInterfaceInfo/GetInterfaceAddresses` | p152–153 | 无 | ❌ | lwIP 单网卡，无枚举接口 |
| `NetGetDnsConfig/ApplyDnsConfig` | p153–154 | 无（`src/net/` 静态配置） | ❌ | — |
| `NetApplyInterfaceConfig/GetSavedConfig/SaveConfig/ReloadConfig/GetConfigStatus` | p154–155 | 无 | ❌ | 无网络配置持久化 |
| `NetWatchCreate/NetWatchRead` | p156 | 无 | ❌ | 无接口变更订阅 |
| `TlsConnectStart/Wait/GetPeerInfo/Send/Receive/Shutdown/Cancel/Close` | p158–162 | `third_party/mbedtls` + `src/net/mbedtls_port.c` + `src/secure_https.c`（**内核侧**） | ◐ | 有 TLS 实现但**无公开句柄 API**；应用只能用一次性 HTTPS GET |
| `HttpRequestStart/GetResponseInfo` | p165–166 | `hax_https_get_v2`（`hax.h:183`，`HBOS_SYS_HTTPS_GET_V2`） | ◐ | 只能 GET，无方法/头/体/重定向控制 |
| `HttpRead` | p168 | 同上（一次性取回全文） | ◐ | 无流式读取 |
| `HttpGetHeader/GetTlsInfo/HttpWait/HttpCancel/HttpClose/HttpCloseIdleConnections` | p166–169 | 无 | ❌ | 无报文级 HTTP 事务 |
| `xapi_httpGet` | p169 | `hax_https_get`（`hax.h:164`） | ✅ | 最接近的一项（便捷 GET） |

**Ch5 小计：✅15 / ◐20 / ❌20**

### 4.6 总统计

| 层 | 函数 | ✅ 有 | ◐ 部分 | ❌ 缺 | 有率 |
| --- | --- | --- | --- | --- | --- |
| L0 基础设施 | 0（类型/宏） | — | — | — | — |
| L2 TUI（Ch3） | 90 | 31 | 10 | 49 | 34% |
| L3 GUI（Ch4） | 89 | 22 | 18 | 49 | 25% |
| L4 XNet（Ch5） | 55 | 15 | 20 | 20 | 27% |
| **合计** | **234** | **68** | **48** | **118** | **29%** |

粗估：**有 ≈29%，部分 ≈21%，缺 ≈50%**；缺口中约 34 项（audio 27 + mp3 6 + SetSecret 1）与权限 6 项属"不适用/低优先"，实际"应补"约 **78 项**。

---

## 5. 可补全项清单

优先级：**P0** 立即做（零内核改动、收益广）→ **P1** 本季度（需要服务契约）→ **P2** 视需求 → **P3** 明确不做。
"落位"一栏区分：`SDK头`（我可做）/ `HIVE`（GUI 域）/ `内核`（主代理域，本文只给契约）。

### P0-1　文本度量与裁剪绘制族（11 函数）

| 项 | 内容 |
| --- | --- |
| XJ380 来源 | 4.2.5.7–4.2.5.14，p74–p78：`CalcTextWidth`、`GetTextMetrics`、`GetTextPosition`、`DrawTextEx`、`DrawTextBaseline`、`DrawTextTop`、`DrawMonoTextTop`、`DrawTextInRect`（另含 `DrawTextl`、`DrawSWText`） |
| HBOS 现状 | `app/include/hax.h:237` `hax_gui_text(x,y,s,color,scale)`——只有整数倍缩放，**无度量、无裁剪、无基线**。字体表在 `HIVE/src/graphics/gui_font.h`（应用不可包含） |
| 差距 | (1) 无法算字符串宽度 → 控件无法居中/右对齐/折行；(2) 无裁剪 → 文本溢出控件边框；(3) 无基线 → 无法与图形混排；(4) 无字节偏移→x 映射 → 文本框光标只能按等宽估算（`hax_widgets.h:888` `hax_ui_textbox_offset_at` 现按等宽推算） |
| 补全方式 | 新增 `app/include/hax_text.h`（纯头 + 内联，字体表以"只读数组 + 度量表"形式随 SDK 发布）：<br>`int hax_text_measure(const char *utf8, int scale, hax_text_metrics_t *out);`<br>`int hax_text_width(const char *utf8, int scale);`<br>`int hax_text_index_at(const char *utf8, int scale, int x);`<br>`int hax_text_draw_clipped(int x,int y,const char *s,int scale,HCOLOR c,const hax_rect_t *clip);`<br>`int hax_text_draw_baseline(int x,int baseline,const char *s,int scale,HCOLOR c);`<br>`hax_text_metrics_t{ascent,descent,line_gap,x_height,ch_width,advance_x}` |
| 代价 | 中低。字体是 `gui_font.h` 里的位图表，需把"字宽/行高/基线"从隐含常量提升为表并复制进 SDK 头（不引入新绘制路径） |
| 落位 | `SDK头`（+ 复制字体度量表）；绘制入口复用现有 `HAX_DRAW_TEXT` / `HBOS_SYS_GUI_TEXT` |
| 优先级理由 | 唯一的"零服务端改动即可显著提升排版质量"项；且它同时解锁 4 个 XJ380 文本变体的映射 |

### P0-2　系统剪贴板 + PASTE/EDIT 消息（9 函数 + 2 消息）

| 项 | 内容 |
| --- | --- |
| XJ380 来源 | 4.8.9–4.8.17，p107–p111：`ClipboardSetText/SetData/Query/Attach/RequestPaste/GetPasteInfo/ReadPaste/FinishPaste/ClipboardClaimMove`；配套 `MSG_PASTE`(19, p90)、`MSG_EDIT`(18, p90) |
| HBOS 现状 | **系统级无实现**。仅应用内选区：`hax_widgets.h:816` `hax_ui_textbox_selection`、`:833` `hax_ui_textbox_select`、`:843` `select_all`、`:848` `copy_selection`、`:864` `delete_selection`。`docs/HIVE_DESKTOP_API.md:85` 已自认该缺口 |
| 差距 | (1) 无法跨应用复制/粘贴；(2) 无所有者 + 修订号（`XClipInfo{revision,window,control}`），无法做变更通知；(3) 无格式协商（文本/图片/文件列表/路径五类）；(4) 无剪切所有权转移 |
| 补全方式 | 在 `gui_service` ABI 增加 4 个 op（`CLIP_SET/CLIP_QUERY/CLIP_ATTACH/CLIP_REVISION`），`hax.h` 增：<br>`typedef struct { HU32 size, version, formats; HU64 revision, owner; } hax_clip_info_t;`<br>`int hax_clipboard_set_text(const char *utf8, HU32 bytes);`<br>`int hax_clipboard_query(hax_clip_info_t *out);`<br>`int hax_clipboard_get_text(char *buf, HU32 cap, HU32 *required);`<br>`int hax_clipboard_attach(hax_window_t w, HU32 formats);`<br>事件侧：`HAX_EV_PASTE`(=`MSG_PASTE` 等价)、`HAX_EV_EDIT`。控件侧在 `hax_widgets.h` 增加 `Ctrl+C/X/V` 到 `hax_ui_textbox_key`(:899) 的分支 |
| 代价 | 中。要定义所有权语义（窗口关闭后内容存续）、变更修订号、跨进程缓冲区拷贝上限 |
| 落位 | `SDK头`（接口）+ `HIVE`（服务实现）+ `内核`（`gui_service` op 追加；`REPOSITORY_SPLIT_BOUNDARIES.md:33` 要求 ABI 编号只追加不重排） |
| 优先级理由 | 唯一"跨应用用户体验断点"，且 HBOS 侧选区基础设施已完备，边际成本最低 |

### P1-3　窗口级动画与服务端帧驱动（4 函数 + 1 消息）

| 项 | 内容 |
| --- | --- |
| XJ380 来源 | 4.7.1–4.7.7，p100–p104；`MSG_ANIMATION`(17U, p83) |
| HBOS 现状 | `app/include/hax_anim.h` 全套：7 属性（46–54）、8 条整数曲线（62–76）、`hax_anim_declare/to/tick/finish`（80–121）。但**纯应用内数学**：无窗口绑定、无 `tag`、无服务端统一时基、无事件回传 |
| 差距 | (1) 每个应用各自 `hax_anim_tick`，帧率不统一；(2) 无法让 WM/合成器驱动过渡（窗口最小化/最大化等系统动效）；(3) 无 `XAnimSnapshot{elapsed_ms,progress,sequence}` 聚合查询；(4) 无 pause/resume 操作码 |
| 补全方式 | `hax.h` 增 `hax_anim_bind(hax_window_t w, hax_anim_t *a, HU64 tag)` + `HAX_EV_ANIM`(18) 事件（`a`=窗口, `b`=tag, `c`=progress×512）；`hax_anim.h` 增 `hax_anim_snapshot(const hax_anim_t*, hax_anim_snapshot_t *out)` 与 `HAX_ANIM_OP_PAUSE/RESUME` |
| 代价 | 中。可先做"应用侧 + 事件回传"（SDK 头），服务端统一 tick 作为第二步 |
| 落位 | `SDK头` → 后续 `HIVE` |
| 优先级理由 | `hax_anim` 曲线与插值已写好，缺的只是"绑定 + 事件"，性价比高；同时解锁 WM 动效统一 |

### P1-4　图像解码（5 函数，先 PNG/BMP）

| 项 | 内容 |
| --- | --- |
| XJ380 来源 | 4.3.2–4.3.6，p80–p82（BMP/PNG/JPEG/GIF/HDR） |
| HBOS 现状 | SDK 内**零解码器**。`HAX_WIDGET_IMAGE` 收裸 `0xAARRGGBB`（`hax_widgets.h:431`）；唯一解码是 `HIVE/app/imgview.c:24` 的未压缩 24/32 位 BMP，且不支持缩放 |
| 差距 | 应用无法加载任何压缩图片；`GetPicSize` 无法只读头 |
| 补全方式 | 新增 `app/include/hax_image.h` + `hax_image.c`（可链接进 `.hax`）：<br>`typedef struct { int width, height, stride; HU32 *pixels; } hax_image_t;`<br>`int hax_image_load(const char *path, hax_image_t *out);`<br>`int hax_image_size(const char *path, int *w, int *h);`<br>`int hax_image_blit_scaled(...);`<br>`void hax_image_free(hax_image_t *);`<br>先支持 BMP + PNG（PNG 需 inflate；`third_party` 已有 mbedtls，其 bignum/`psa` 不含 zlib，建议内置 ~500 行 `tinflate` 级实现或复用 `web-vendor` 中的解压代码——**立项前需确认许可**） |
| 代价 | 中高（inflate 是主要成本）；单做 BMP+未压缩 PNG 可降到低 |
| 落位 | `SDK头` + 新 `.c`（新增文件，不触内核） |
| 优先级理由 | 每个 GUI 应用都要图片；但成本高于 P0，故列 P1 |

### P1-5　系统信息与任务枚举（5 函数）

| 项 | 内容 |
| --- | --- |
| XJ380 来源 | 3.4.5 `GetTaskList`(p31)、3.5.5 `GetCurrentUser`(p33)、3.5.7 `GetCpuModel`(p34)、3.5.8 `GetMemorySize`(p34)、3.6.6 `GetSystemStats`(p35) |
| HBOS 现状 | 全部无公开接口。`HIVE/app/taskmgr.c` 是"输入 PID → 发信号"的前端（`hive_process_signal`），不能枚举 |
| 差距 | 任务管理器无法列出进程；无内存/CPU 使用率；`/proc` 仅 devfs 内部使用 |
| 补全方式 | 新增 syscall `HBOS_SYS_TASKLIST(buf, cap)`，`hax.h` 增：<br>`typedef struct { HU64 pid, ppid, tid, threads, windows, mem_bytes, cpu_ticks; HU32 state; char name[32]; } hax_task_info_t;`<br>`int hax_task_list(hax_task_info_t *buf, int cap);`<br>`int hax_sys_stats(hax_stats_t *out);`<br>`int hax_mem_total(HU64 *bytes);`<br>（字段与 `XapiTaskInfo`(p30)/`XapiSystemStats`(p35) 对齐，便于映射） |
| 代价 | 中（内核遍历任务 + 拷贝；注意 `src/core/**` 属主代理域） |
| 落位 | `SDK头` + `内核` |
| 优先级理由 | 任务管理器/系统监视器是刚需，但涉及内核改动，排 P1 |

### P1-6　音频用户 API（先做 PCM 流 13 项，不做 player/mp3）

| 项 | 内容 |
| --- | --- |
| XJ380 来源 | 3.8.1.5–3.8.1.17，p42–p47（PCM 流）；3.8.2/3.8.3（player 8 + mp3 6）为后续 |
| HBOS 现状 | `src/ac97.c`（驱动）+ `src/wav.c`（解码）+ `src/tools/audio.c`（内核内 `playwav` 工具）。**用户态零接口** |
| 差距 | 应用不能出声；无设备枚举/流句柄/音量/静音/欠载统计 |
| 补全方式 | `HBOS_SYS_AUDIO_OPEN/WRITE/QUERY/CLOSE`，`hax.h` 增：<br>`typedef struct { HU32 size, version, sample_rate, channels, format, ring_frames; } hax_audio_desc_t;`<br>`HU64 hax_audio_open(const hax_audio_desc_t*, int *err);`<br>`int hax_audio_write(HU64 h, const void *pcm, HU32 frames);`<br>`int hax_audio_query(HU64 h, hax_audio_status_t *out);`<br>`int hax_audio_set_volume(HU64 h, HU32 q16);`<br>`int hax_audio_close(HU64 h);` |
| 代价 | 中高（需要内核侧环形缓冲 + 用户页映射 + 阻塞/超时语义；`XAudioStatus` 的 `underrun_count/latency_ns` 需要真实时基） |
| 落位 | `SDK头` + `内核` |
| 优先级理由 | 缺口最大（27 项），但成本也最高；建议只做 PCM 流，player/mp3 暂不做（wav 已在，MP3 见 §6） |

### P2-7　类型转换族 + `xapi_` 别名头（5 函数 + L0 别名）

- 现状：HBOS 无 `xcr_*`/`toRGB`/`toRGBA`；类型别名前缀为 `H`（`hax.h:47–55`）。
- 补全：`app/include/xj380_compat.h` —— 提供 `INT8..UINT64/WSTR/HDLE` 别名 + `xcr_char2int/int2char/hex2char` + `toRGB/toRGBA` + 可直接映射的 `xapi_*` 内联实现，不可映射者返回明确 `XAPI_ENOSYS`。
- 代价：**低**（纯头文件，无构建影响）。本次已提交该草案（见 §7）。
- 价值：让 XJ380 示例代码具备"可编译、可运行、缺口立即暴露"的移植路径，并给后续实现定锚。

### P2-8　XML 解析（3 函数）

- 现状：全树无 XML；JSON 仅内核侧扁平对象（`src/lib/json.h:85`）。
- 补全：`app/include/hax_xml.h` + `.c`，DOM 模型直接对齐 `XXmlNode{type,name,text,attributes,parent,firstChild,lastChild,nextSibling}`（p59）。
- 代价：低–中（约 600 行）。价值：配置文件/主题/包清单可用。

### P2-9　截屏（1 函数）

- 现状：无对外接口；framebuffer 在 `src/graphics/graphics.c` 内。
- 补全：`HBOS_SYS_SCREEN_CAPTURE(XCOLORA *buf, cap, *w, *h)`——注意需先有 4.5 的"回读"能力（现无反读路径）。
- 代价：中（需 framebuffer 读权限与合成后快照时机）。

### P2-10　文件对话框（1 函数）

- 现状：`HIVE/app/files.c` 是独立应用，未服务化。
- 补全：文件选择器作为服务（`gui_service` 的 `FILEDIALOG` op）或"应用内嵌 `hax_widgets` 目录列表"约定。
- 代价：中。价值：所有"打开/另存为"场景。

### P2-11　窗口图标运行期设置（1 函数）

- 现状：构建期 `.haxicon` 段 + `hax_app_icon_at()`（`hax.h:99–103`）。
- 补全：`hax_window_set_icon(w, const char *path)` → 需要 P1-4 的解码器。
- 代价：低（前置依赖 P1-4）。

### P2-12　HTTP/TLS 报文级 API（17 函数）

- 现状：`hax_https_get`(:164)/`hax_https_get_v2`(:183) 只能 GET；TLS 在 `third_party/mbedtls` + `src/net/mbedtls_port.c` 内核侧。
- 补全：先暴露 `hax_http_request_start(desc) → handle`、`hax_http_read`、`hax_http_get_header`；TLS 句柄 API 待 HTTP 层稳定后再开。
- 代价：高（内核侧 TLS 会话管理 + 超时 + 取消）。价值：下载器/包管理器/浏览器共需。

### P3-13　IME（16 函数）—— 暂不补

- 现状：零。键盘事件只有 ASCII（`docs/HIVE_DESKTOP_API.md:85`）。
- 不补理由：前置条件不成立——需先有 (a) UTF-8 键盘事件、(b) 候选窗服务、(c) 跨进程 IME 会话协议。XJ380 的 16 个函数 + 双代 V1/V2 结构是这三者的**结果**，不是起点。建议路线：UTF-8 键盘 → 应用内软键盘 → 独立 IME 进程 + 候选窗 → 最后才谈 `XIme*` 兼容层。

### P3-14　`xapi_PaintControls` / 服务端控件布局 —— 不补

- 理由：与 HBOS 架构冲突。`docs/REPOSITORY_SPLIT_BOUNDARIES.md:92–103` 规定"应用状态必须由应用进程自己持有；SDK 控件状态保存在 `hax_ui_t`"。让服务端持有控件布局会把 18 类控件的绘制/命中/焦点全部搬进 HIVE，破坏拆分边界。
- 等价收益：HBOS 的 `hax_ui_dispatch`（:1072）+ `hax_ui_hit_index`（:987）已提供同一能力，只是位置在应用侧。

---

## 6. 不适用的概念

| 概念 | 页 | 不适用理由 |
| --- | --- | --- |
| 5 类用户 / 7 项权限模型（`UserPermisson` 全族 6 函数） | p18, p55–p58 | HBOS 是单用户桌面：无 `/users/<name>`、无登录会话、`getuid` 恒常量。XJ380 的模型建立在"每进程有用户身份 + 密码提升（System 密码默认 114514）"之上，全部基础设施 HBOS 都没有。**不要照搬**；若要做沙箱，应走 capability 模型（按应用授权目录），而非用户等级 |
| `xapi_RequestSpecialPermission` / `xapi_LiftPermissions` | p57 | 依赖密码学与提权流程，见上 |
| `xapi_Run` 降权为 Visitor | p18, p36 | 无权限层级可降。HBOS 语义应为"继承父进程凭据" |
| 交叉编译套件（`xxcc/xxccte/xxcc-elf`、EPF 格式、`-Ttext=0x200000`） | p14–15 | HBOS 用 `hbos-gcc.specs` + `.hax` manifest，链接脚本 `linker.ld`/`user.ld` 已定；EPF 是 XJ380 私有容器格式，无兼容价值 |
| 头文件族 `x3api.h/x4api.h/xposix.h/xguiapi.h/xtuiapi.h/krlibc.h` | p13 | HBOS 侧统一由 `hive.h` → `hax.h`/`hax_widgets.h`/`hax_anim.h` 承接；多入口头文件会引入重复定义风险 |
| `__XJ380_OS__` 宏 | p18 | HBOS 的构建期识别是 `.hax` manifest + `HAX_KIND_*`（`hax.h:58–64`） |
| `xapi_OutputSerial` | p23 | HBOS 无面向用户的串口设备抽象（串口仅调试输出） |
| `xapi_SetSecret`（防截屏） | p128 | 需要 DRM/内容保护层与合成器配合；HBOS 无此层，且当前无商业内容保护需求 |
| MP3 解码（`xapi_Mp3*` 6 项） | p51–p54 | 体积与许可成本高；HBOS 已有 `src/wav.c`，音频缺口应先补 PCM 流（P1-6）而非解码器。除非有明确 MP3 素材需求，否则不做 |
| XNet 配置管理 12 项（接口枚举/DHCP 状态/保存配置/watch） | p147–p156 | lwIP 在本仓库是静态配置的单网卡实现，无 NetworkManager 等价物。做这 12 项等于先造一套网络管理器，ROI 极低。保留 BSD `getaddrinfo` 足够 |
| `XCOLOR`（24 位）与 `XCOLORA`（32 位）双轨颜色 | p80, p91 | HBOS 统一 `HCOLOR` = `0xRRGGBB`（`hax.h:55`）+ 绘制命令中 ARGB（`HAX_DRAW_ARGB`）。**保留单一表示**，`XCOLOR/XCOLORA` 只在 `xj380_compat.h` 内做转换 |
| BridgeEngine BAPI（Ch6）/ Stardust UI（Ch7） | p170–p171 | 两章均无内容（仅"简介"标题），规范本身未定义任何函数或类型，无可对标对象 |
| `xapi_Getch` 的阻塞语义 | p22 | XJ380 在 TUI 下阻塞读；HBOS GUI 应用必须走事件队列（否则窗口事件饿死）。映射时需注明"仅 TUI 可用" |

---

## 7. 已提交的头文件草案

`app/include/xj380_compat.h`（新增，纯头文件，不参与内核构建）：

- 提供 L0 类型别名（`INT8..UINT64`、`WSTR`、`HDLE`、`MsgPrcor`）与颜色转换；
- 用 `static inline` 把 **HBOS 已有能力**映射到 `xapi_*` 名字：文本 I/O、文件读写/目录、进程、时间、内存、窗口创建/标题/尺寸、绘制（点/线/矩形/文本）、`Sleep`；
- 对**尚未实现**的族（音频、IME、剪贴板、通知、截屏、XNet 配置、XML）只声明 `XAPI_ENOSYS` 桩并注明对应本文条目，使移植代码"编译通过、运行时报明确缺口"，而不是链接期才炸；
- 每个桩上方一行注释指向本文件的 §5 条目号，便于后续逐条替换实现。

配套宿主测试：`tests/host/test_xj380_compat.c`（46 断言，只测不触碰 syscall 的层）。

验证方式（不需要 QEMU，不改 Makefile；该文件刻意不接入 `make test`，避免与主代理的构建线冲突）：

```sh
gcc -O2 -Wall -Wextra -Iapp/include -Isrc/user \
    tests/host/test_xj380_compat.c -o build/tests/test_xj380_compat
./build/tests/test_xj380_compat
# 实测输出：xj380_compat: 46 passed, 0 failed
```

测试覆盖：类型宽度（1/2/4/8 字节）、句柄装载（负窗口不得变成大正数）、
`xcr_*` 三函数（含安全截断）、`toRGB/toRGBA` → `HCOLOR`/`0xAARRGGBB`、
`XWINDOW`/`XFILE`/`DirNode` 结构布局，以及**所有明示缺口必须返回 `-ENOSYS`
或 0/清零**——防止"静默成功"把缺口藏起来。

---

## 8. 复核方法

本文所有 HBOS 侧陈述可按下表复核：

```sh
# 1. HBOS 侧接口面
grep -n 'static inline' app/include/hax.h app/include/hax_widgets.h app/include/hax_anim.h | wc -l

# 2. syscall 面
sed -n '/^enum {/,/^};/p' src/user/libc/syscall.h | grep -c HBOS_SYS_

# 3. 确认缺口（应全部无输出）
grep -rn 'clipboard' HIVE/src app/include --include=*.h --include=*.c
grep -rn 'xapi_\|XCOLORA\|XIme' app/ src/user HIVE/app/include | grep -v xj380_compat

# 4. XJ380 侧
pdftotext -layout <pdf> build/tmp/xj380.txt
grep -cE '^[0-9]+(-[0-9]+)+\s+xapi_?\w+\(\);' build/tmp/xj380.txt | head   # 目录条目
```

---

## 9. 边界声明

- 本文**未修改** `src/syscall.c`、`src/linux_compat.c`、`src/elf.c`、`src/core/**`（主代理域）。
- 本文涉及的 `gui_service` op 追加、syscall 追加均为**契约草案**，实施需主代理域排期；按 `docs/REPOSITORY_SPLIT_BOUNDARIES.md:49` 要求，ABI 编号只追加、不重排。
- 未运行 QEMU / `make bios-iso` / `make run*` / `make test-all`。
