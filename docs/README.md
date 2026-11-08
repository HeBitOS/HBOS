# HBOS 文档

两份入口，别搞混：

- **[`wiki/`](wiki/)** —— 人类读的长期参考文档，改代码后必须同步。
- **[`agents/`](agents/)** —— AI 工作区：子系统地图、进度台账、工作约定。

## 长期参考（`wiki/`）

- **[HBOS_HAX_API](../HBOS_HAX_API.pdf)** —— HBOS 应用开发手册（HAX SDK / `.hax`）。
  源文件为 [`wiki/HBOS_HAX_API.html`](wiki/HBOS_HAX_API.html)。
- **[wiki/CODE_STYLE.md](wiki/CODE_STYLE.md)** —— 命名、注释（含头文件模板
  §3.1）、错误处理与性能纪律。
- **[wiki/PERFORMANCE.md](wiki/PERFORMANCE.md)** —— 性能工作记录：测量方法、
  已实施优化及其验证证据、刻意不改的取舍，以及分子系统审计出的待办清单。
- **[CHROMIUM_COMPAT_BASELINE.md](CHROMIUM_COMPAT_BASELINE.md)** —— 软件模块
  化、窗口 API 与 Chromium 兼容层的阶段 1 能力基线和 ABI 约定。
- **[Chromium 固定源码与构建入口](../browser/chromium/README.md)** —— 固定上游
  Stable 提交、外部工作树管理、主机参考构建与 HBOS 目标 GN 参数。
- **[wiki/REPOSITORY_SPLIT_BOUNDARIES.md](wiki/REPOSITORY_SPLIT_BOUNDARIES.md)**
  —— 内核、GUI 与应用的拆仓边界、no-GUI 构建矩阵和独立性规则。
- **[wiki/HIVE_DESKTOP_API.md](wiki/HIVE_DESKTOP_API.md)** —— HIVE 桌面工具包、
  交互约定与应用迁移说明。
- **[wiki/LINUX_KDE_COMPAT.md](wiki/LINUX_KDE_COMPAT.md)** —— 轻量 Linux ABI
  兼容层、已实现接口和不修改上游 KDE 的分阶段路线。
- **[wiki/HEADLESS-DEPS.md](wiki/HEADLESS-DEPS.md)** —— 无头构建与运行所需的
  宿主依赖清单。
- **[HIVE 浏览器与 Bilibili 兼容升级方案](../HIVE/docs/BROWSER_BILIBILI_UPGRADE.md)**
  —— 当前 HTML/CSS/JS 能力审计、Bilibili 现网技术栈抽样和可验收的升级门槛。

## 一次性产物（本目录根下）

某次改动的证据、某轮的对比报告、截图、logo。有历史价值，但不说明
"现在该怎么写代码"。

- `evidence/` `screenshots/` —— 复现证据与界面截图
- `ROADMAP-*.md` `STATUS-*.md` `PERF-BASELINE-*.md` —— 某轮的路线与基线
- `*-DIFF.md` —— 与其他项目的逐项对比
- `GUI_REFACTOR.md` `jindu.md` `HANDOFF-*.md` —— 重构记录与交接说明

## 重新生成 PDF

PDF 由 HTML 经 LibreOffice 无头转换得到：

```sh
make hax-doc
# 等价于：
soffice --headless --convert-to pdf:writer_web_pdf_Export \
        --outdir . docs/wiki/HBOS_HAX_API.html
mv docs/wiki/HBOS_HAX_API.pdf HBOS_HAX_API.pdf
```

需要安装中文字体（如 `fonts-noto-cjk`，提供 Noto Sans CJK SC）。
