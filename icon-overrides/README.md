# 自定义应用图标（构建期定制）

构建 HBOS 镜像时，往本目录放入以图标名命名的 PNG，即可替换内置图集
（build/gui_icons.bin）中对应的那一块。HIVE 桌面（启动器、任务栏、
桌面快捷方式）直接按图标 id 渲染这份内嵌图集，因此替换后无需改任何
GUI 代码即可同步显示新图标。

## 使用方法

1. 把图标放到本目录，文件名 = 图标名（见下方列表），例如：
   cp 我的浏览器图标.png icon-overrides/browser.png
2. 重新构建（图标变化会触发 gui_icons.bin 重新生成并重新链接内核）：
   make        # 或只重新生成资源： make font

想换回内置图标，删掉对应的 icon-overrides/<name>.png 再 make 即可。
也可以临时指定别的目录：

   make GUI_ICON_OVERRIDE_DIR=my-icons

## 可用图标名

| 文件名 | 用途 |
| ------ | ---- |
| files.png | 文件管理器 |
| disk.png | 磁盘面板 |
| sys.png | 资源/系统面板 |
| apps.png | 应用面板 |
| notes.png | 记事本 |
| calc.png | 计算器 |
| uwc.png | UWC |
| snake.png | 贪吃蛇 |
| browser.png | 浏览器 |
| code.png | 代码 |
| term.png | 终端/诊断 |
| clock.png | 时钟 |
| shortcuts.png | 快捷键 |
| folder_row.png | 文件管理器目录行小图标 |

## 注意事项

- 支持任意尺寸 PNG（推荐 64x64 或更大），构建时统一缩放到 64x64；
  建议保留透明背景（RGBA），与图集其余图标风格一致。
- 只认 <name>.png；目录里的其他文件会被忽略。
- 图标名与 tools/genicon.py 的 ICONS[] 顺序一一对应，不要改名。
- HIVE 侧（HIVE/tools/genicon.py）保持同一套覆盖机制，两边构建行为一致。

## HAX 应用的内嵌图标（另一条定制通道）

上面替换的是「内置 GUI 图标图集」的瓦片。如果你自己写 .hax 应用
（app/<名字>.c），可以让图标内嵌进应用本身：把 PNG 放到
app/<名字>.png（多文件应用放 app/<名字>/icon.png），构建时图标以
.haxicon 段写入 .hax 并随应用打包，HIVE 启动器会优先显示它。
详见 app/README.md「应用图标」。

两种方式可同时使用：内置应用（文件管理器、浏览器等）用本目录覆盖图集，
自写的 GUI .hax 应用用内嵌图标。
