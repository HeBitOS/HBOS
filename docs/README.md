# HBOS 文档

- **[HBOS_HAX_API.pdf](../HBOS_HAX_API.pdf)** —— HBOS 应用开发手册（HAX SDK / `.hax` 应用）。
  源文件为 [`HBOS_HAX_API.html`](HBOS_HAX_API.html)。
- **[CHROMIUM_COMPAT_BASELINE.md](CHROMIUM_COMPAT_BASELINE.md)** —— 软件模块化、
  窗口 API 与 Chromium 兼容层的阶段 1 能力基线和 ABI 约定。
- **[Chromium 固定源码与构建入口](../browser/chromium/README.md)** —— 固定上游
  Stable 提交、外部工作树管理、主机参考构建与 HBOS 目标 GN 参数。
- **[REPOSITORY_SPLIT_BOUNDARIES.md](REPOSITORY_SPLIT_BOUNDARIES.md)** —— 内核、
  GUI 与应用的拆仓边界、no-GUI 构建矩阵和独立性规则。
- **[HIVE_DESKTOP_API.md](HIVE_DESKTOP_API.md)** —— HIVE 桌面工具包、交互约定
  与应用迁移说明。
- **[LINUX_KDE_COMPAT.md](LINUX_KDE_COMPAT.md)** —— 轻量 Linux ABI 兼容层、
  已实现接口和不修改上游 KDE 的分阶段路线。
- **[HIVE 浏览器与 Bilibili 兼容升级方案](../HIVE/docs/BROWSER_BILIBILI_UPGRADE.md)**
  —— 当前 HTML/CSS/JS 能力审计、Bilibili 现网技术栈抽样和可验收的升级门槛。

## 重新生成 PDF

PDF 由 HTML 经 LibreOffice 无头转换得到：

```sh
make hax-doc
# 等价于：
soffice --headless --convert-to pdf:writer_web_pdf_Export \
        --outdir . docs/HBOS_HAX_API.html
mv docs/HBOS_HAX_API.pdf HBOS_HAX_API.pdf
```

需要安装中文字体（如 `fonts-noto-cjk`，提供 Noto Sans CJK SC）。
