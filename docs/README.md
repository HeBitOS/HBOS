# HBOS 文档

- **[HBOS_HAX_API.pdf](../HBOS_HAX_API.pdf)** —— HBOS 应用开发手册（HAX SDK / `.hax` 应用）。
  源文件为 [`HBOS_HAX_API.html`](HBOS_HAX_API.html)。

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
