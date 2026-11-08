# files/ — 用户素材落地目录

这里放的东西不算源码，构建 release 产物时会**原样**打进镜像根下：

```text
files/示例.txt      →  ISO:/files/示例.txt   →  HBOS 里 /files/示例.txt
files/data/*.csv    →  ISO:/files/data/...    →  目录层级保留
```

## 什么时候该用它

- 想往系统里塞一份自己的数据/脚本/壁纸，但**不想改源码、也不想动 Makefile**
- 要给某个 release 版本配套一份固定的示例内容
- 临时测试一批文件在 HBOS 里能不能被读

## 什么时候不该用

- 要进内核的字体、壁纸、图标 → 那些走 `fonts/`、`photo/`、`icon-overrides/`，
  构建期会被 `tools/gen*.py` 变成二进制嵌入内核
- 要成为系统一部分的应用 → 放 `app/`（HAX 应用）或 `src/app/legacy/`
  （内核内注册的旧式应用）
- 只是文档 → `docs/`

## 行为

- 目录为空时，ISO 构建**完全不**复制任何东西，也不会因此重新打镜像。
- 目录里有文件时，构建用内容 sha256 记账（`build/.files-stamp`），
  **内容变了才重打 ISO**，改时间戳不会触发无谓的重建。
- 文件按原样复制，不改名、不压缩、不编码转换。中文文件名在 ISO9660
  的 Joliet 扩展下可用（`grub-mkrescue` 与 `xorriso` 都带 `-J`），
  但**建议 ASCII 文件名**——HBOS 内核对 UTF-8 路径的支持边界见
  `src/vfs.c`，跨平台拷贝时省得踩坑。

这个目录本身应该长期是空的（或者只放真正要给用户的样例）。别把它当
临时垃圾桶——它会跟着 ISO 一起发出去。
