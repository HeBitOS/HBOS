# Chromium on HBOS —— 验收证据

## `chromium-canvas-render.png`

- **来源**：HBOS/QEMU 内真实运行的 `chromium-headless-shell` 156。
- **产生方式**：页面用 `<canvas>` + `CanvasRenderingContext2D` 绘制，
  再 `toDataURL('image/png')`；`--dump-dom` 把 DOM 打印出来，
  从 `data-png` 属性取回 base64 解码得到该文件。
- **为什么不用 `--screenshot`**：`--screenshot` 走合成器拷贝输出，
  当前在 HBOS 上恒为全透明空白帧（见 `docs/jindu.md` R34–R39）。
  canvas 走的是 Skia CPU 光栅，这条路径在 HBOS 上是**正确工作**的。
- **可验证内容**（480×120）：
  - 白色背景 `(255,255,255)`
  - 黑色粗体文字 `HBOS-CHROMIUM-OK`，字形像素 3355
  - 红色矩形 12,70,300×20 → 6000 像素 `(192,0,0)`
  - 蓝色矩形 12,95,200×12 → 2400 像素 `(0,0,192)`

复现：

```bash
scripts/install_guest_fonts.sh build/c3-chromium.img
python3 scripts/qemu_session.py build/hbos-bios.iso --disk build/c3-chromium.img \
  --mem 4096M --accel tcg --boot-timeout 40 --timeout 100 --log /tmp/text.log -- \
  'run /opt/chromium-headless-shell/usr/lib/chromium/chromium-headless-shell \
   --no-sandbox --no-zygote --single-process --disable-gpu --dump-dom file:///text.html'
```

`/text.html` 见 `docs/evidence/text.html`。
