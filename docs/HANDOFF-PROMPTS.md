# 交接提示词（复制给其他 AI 用）

> 用法：把下面「任务 A」「任务 B」各自整段粘贴给对应 AI 的新会话。
> 两份都自带背景与约束，不依赖本对话上下文。

---

## 任务 A：HIVE 桌面改造为 Wayland 合成器（P0–P5）

```
你在 HBOS 仓库（C 语言 + 自研内核的 x86_64 玩具/教学操作系统）里工作。
当前桌面 HIVE 是 src/tools/gui.c 里的单体合成器：自己画窗口、自己处理
鼠标键盘、自己 present 到帧缓冲。目标是在一个对话内把它改造成
Wayland 风格的合成器，分 P0–P5 六个阶段完成。

【硬性流程】
1. 你的第一条回复只能是 10 个问题，不要写任何代码、不要给计划。
   问题必须覆盖：范围边界、兼容性承诺（现有内置应用能否不改代码）、
   性能预算、协议取舍（wl_compositor/wl_surface/wl_seat/xdg-shell 等价物）、
   共享内存模型、失败回退策略、测试与验收方式、P0–P5 的里程碑划分、
   与调度器（另一条线在做 CPU 多线程优化）的接口、最终交付形态。
2. 我回答后，你再输出目标 + todo + P0–P5 计划，然后逐步实现。
3. 每个阶段结束要能编译、能启动、能用 QEMU 截图验证；不许留半成品。

【现状事实（不要重复调研）】
- 入口：src/tools/gui.c（约 6800 行），窗口管理在 wm.c，服务层 winsrv.c，
  绘制层 gui_draw_impl.c（gd_* 原语），应用通过 gui_canvas.h 的
  gui_app_* 接口画画。
- 已有双缓冲/脏矩形基础：gui_damage_region、gui_dirty_mark_full。
- 构建：make bios-iso；测试：make test（宿主机单测）。
- QEMU 截图可用于视觉回归：-display none + monitor screendump。

【验收】
- P0–P5 全部完成，现有内置应用（文件管理器/计算器/浏览器/任务管理器等）
  不改代码即可运行。
- 拖动窗口时不再整屏重绘（可用脏区计数证明）。
- 缺 dmabuf/多输出时自动回退 shm，不崩。

不要动 CPU 调度器和桌面动效实现，只通过约定接口交互。
```

---

## 任务 B：HPT 接入 Linux 软件源（deb / pacman / AUR → HAX）

> 上一版这段写得太"愿景"，没有落到仓库现状。这一版把 HPT 的真实格式、
> 真实限制、真实验收命令都写进去了，接手方不需要猜。

```
你在 HBOS 仓库（C 内核 + 用户态 HAX/ELF 应用）里工作。目标不是"从零写一个
包管理器"——HPT 已经存在，你要做的是让 HPT 能消费 Linux 发行版的包。

【现状事实（先读，别重复发明）】
- HPT 在 HPT/（子模块），是 apt 风格的 HAX 包管理器：
  update / search / install / remove / upgrade / verify / run。
- 仓库格式：Packages 索引 + pool/，每行八个 | 分隔字段：
  name|version|arch|size|sha256|file|depends|description
- 安装模型：单文件载荷 → /bin/<包名>；数据库在 /var/lib/hpt；
  安装走"写 .stage → 复读校验 → 提交"，升级保留 .backup 可回滚。
- 已知硬限制（你必须先解决或明确绕过）：
  1) 声明大小上限 2 MiB —— Chromium 有 100MB+，这条会直接卡死；
  2) 一个包 = 一个文件（/bin/<name>）—— 而 deb 是目录树（/usr/lib、/etc、/usr/share）；
  3) 依赖字段只有包名，没有版本区间。
- HBOS 的 Linux 兼容层：src/linux_compat.c（syscall 翻译）、src/user/hax.c、
  src/user/ldso.c（动态链接）；文档 docs/LINUX_KDE_COMPAT.md、
  CHROMIUM_COMPAT_ROADMAP.md、docs/HBOS_HAX_API.html。

【按三个可独立验收的子任务推进，不要一口气全做】

## B1 解包器：deb / pacman / AUR → HAX 载荷
- 输入：.deb（ar + data.tar.{gz,xz,zst}）、.pkg.tar.zst、AUR 的 PKGBUILD 产物。
- 输出：一个 HAX 包 + 一份 manifest（文件清单、权限、依赖、图标、入口）。
- 落点：HPT/tools/ 下新增转换脚本（Python，与 hpt_repo.py 同风格），
  不要动 HPT 的 C 客户端协议。
- 验收命令：
    python3 HPT/tools/pkg2hax.py testdata/hello.deb -o out/
    python3 -c "import json;print(json.load(open('out/hello.manifest.json'))['files'])"
  必须能正确解析 3 个真实包（建议：hello、tree、zlib1g）。

## B2 打包/安装模型：从"单文件"到"多文件 + 目录树"
- 目标：HAX 包可以携带目录树与符号链接；安装到 /opt/<pkg>/ 并在 /bin 建入口。
- 必须先改 HPT 客户端：包格式版本号 + 解包到 /opt + 记录文件清单（卸载要能删干净）。
- 不改旧格式的兼容性：老包（单文件）必须继续可装。
- 验收命令（QEMU 内）：
    run hpt update && run hpt install <多文件测试包>
    run ls /opt/<pkg>/usr/lib      # 目录树存在
    run hpt remove <多文件测试包>   # /opt 与 /bin 入口都被清掉

## B3 GUI 安装器（HIVE 应用）
- 落点：HIVE 应用（参考 src/gui/apps/app_files.c 的列表/滚动/输入模式）。
- 功能：搜索、包详情（大小/依赖/校验值）、安装进度、错误提示、已安装列表。
- 验收：QEMU 截图能看到列表 + 安装进度条；装完后开始菜单出现入口。

【红线】
- 只用 Chromium（BSD），禁止 Chrome 的品牌/图标/官方二进制。
- 先不上传 GitHub；所有产物留在本地。
- 不许把"能装"写成"能跑"：每个阶段给出可复现命令 + 实际输出。

【第一步只做这个】
读完 HPT/docs/HPT.md 与 HPT/src 后，回答 5 个问题（不要写代码）：
1) 2 MiB 上限在哪个文件里，是常量还是协议字段？改成多少安全？
2) 单文件→目录树，包格式版本号怎么加才不破坏老客户端？
3) /opt 前缀与 HBOS 的 /bin 入口，卸载时如何保证不残留？
4) 依赖没有版本区间，遇到 glibc 版本冲突时先报错还是先装？
5) Chromium 的动态链接依赖（ld.so、libc 符号）由谁负责——HAX 转换器还是
   linux_compat 层？
回答后我再确认里程碑顺序。
```
