# AI 工作区（docs/agents/）

> 给 AI 读的目录结构与工作约定。人类读者请先看 [`docs/wiki/`](../wiki/)。
> 本目录**不存**实现细节的权威版本——权威版本在代码里；这里存的是
> "怎么找、改之前要知道什么、改完了写哪儿"。

## 为什么有这一层

HBOS 的内核、GUI、用户态被拆在不同子树里，一次改动经常横跨四五层。
过去每个子目录各挂一份 `*.agent.md`（`src/core-内核核心.agent.md` 这种
中文文件名 + 空格），结果：

- 文件散落在源码树里，`ls src/` 一眼看不出版图，新人/AI 都得先猜；
- 中文文件名在脚本、CI、跨平台工具链里反复出问题（编码、引号、通配）；
- 每份文档只写自己那一块，看不出层与层之间的依赖方向。

现在统一收进 `docs/agents/`，用**与代码目录同名的英文小写文件名**，
一份文件对一个子系统。新增子系统就加一份，不要往源码目录里塞 md。

## 目录约定

```text
docs/
├── agents/     ← 你在这儿。AI 工作约定、子系统地图、进度台账
│   ├── README.md      本文件：规则与索引
│   ├── PROGRESS.md    主进度台账（里程碑、当前阶段、未完成项）
│   ├── core.md        src/core/         CPU、内存、调度、中断
│   ├── fs.md          src/ 文件系统层    ext2/exFAT/FAT32/HBFS/VFS
│   ├── net.md         src/net*/         网络栈、TLS、IPv6
│   ├── crypto.md      src/crypto/       哈希、曲线、AEAD
│   ├── graphics.md    src/graphics/     字体、图元、blob 解压
│   ├── gui.md         src/gui/          桌面、WM、合成器、Wayland
│   ├── input.md       src/input/        鼠标、键盘、USB HID
│   ├── shell.md       src/shell/        命令行
│   ├── tools.md       src/tools/        内核内建命令与 GUI 工具
│   └── user.md        src/user/ + src/app/  用户态、libc、HAX SDK
└── wiki/       ← 人类读的长期参考文档（改代码后必须同步）
    ├── CODE_STYLE.md                  命名/注释/错误处理/性能纪律
    ├── PERFORMANCE.md                 性能方法与历史数据
    ├── HIVE_DESKTOP_API.md            桌面工具包 API
    ├── LINUX_KDE_COMPAT.md            Linux ABI 兼容层
    ├── REPOSITORY_SPLIT_BOUNDARIES.md 拆仓边界
    ├── HEADLESS-DEPS.md               无头依赖
    └── HBOS_HAX_API.html → PDF        应用开发手册
```

`docs/` 根目录剩下的东西是**一次性产物**：某次改动的证据、某轮的对比
报告、截图、logo。它们有历史价值但不承载"现在该怎么写代码"，所以不
往 `wiki/` 里放。

## 三条硬规则

**1. 权威性单向下沉。** 代码 > `docs/agents/*.md` > `docs/wiki/*.md`。
三者冲突时以代码为准，并且**当场把文档改成和代码一致**，不要留着
"文档写的和代码不一样"过夜。

**2. 改代码就改对应文档。** 动了 `src/gui/` 就更新 `docs/agents/gui.md`
的职责表；改了对外 API、构建开关、目录布局，就更新 `docs/wiki/` 里对应
的文档和根 `README.md`。文档过期比没有文档更糟——它会让人（和 AI）
照着错的做。

**3. 新知识先找位置。** 想记一笔的时候先问：这是"某次怎么查的"（→
`docs/` 根目录的证据文件）、"这个子系统怎么搭的"（→ `docs/agents/x.md`）、
还是"所有人都该遵守的规则"（→ `docs/wiki/`）。找不到位置说明该新建，
但**不要**新建到源码目录里。

## 给 AI 的检索顺序

接手一个任务时按这个顺序读，别一上来就 glob 全仓：

1. `docs/agents/PROGRESS.md` —— 当前阶段在做什么，哪些是坑
2. `docs/agents/<子系统>.md` —— 你要动的那块的地图
3. `docs/wiki/CODE_STYLE.md` —— 写代码前对齐风格（尤其 §3.1 头文件模板）
4. 代码本身

## 命名规范

- 文件名：**全小写英文 + 连字符**，不加前缀后缀（不叫 `gui-agent.md`，
  就叫 `gui.md`）。理由同上：中文名与特殊字符在工具链里会咬人。
- 一份文件只写一个子系统。跨子系统的流程写在 `PROGRESS.md` 或
  `docs/wiki/` 里。
- 文件内用 `##` 分节，第一段说明"这个子系统负责什么、不负责什么"。
  边界比内容重要——大部分误改都发生在边界上。
