# HBOS GUI 框架重构设计

> 状态：**P0–P6 已完成并验证**（P7–P8 为长期迁移项，见 §7 的实际进度表）
> 目标：统一 UI 库 + 统一主题 + 外部主题包 + 模块化，根治现有 GUI 的 bug 与扩展性问题
>
> **落地成果**：主题令牌层 + 外部主题包（`/themes/*.theme`）+ 9 个控件库
> + 统一窗口管理器 + 单一应用注册源；472 处硬编码颜色清零并有 CI 断言；
> 495 项宿主机单测全绿。细节见 §10「实施记录」。

---

## 1. 现状诊断（基于实测，不是猜测）

| 问题 | 实测证据 | 后果 |
|---|---|---|
| **上帝对象** | `gui_state_t` 有 **115 个字段**，塞进计算器、贪吃蛇、记事本、浏览器、代码编辑器、WM 的全部状态 | 任何应用改动都可能影响其他应用；无法独立测试 |
| **全局单例** | `src/tools/gui.c:7376` `static gui_state_t st` | 只能开一个桌面实例；应用无法多开 |
| **巨石文件** | `src/tools/gui.c` **8603 行**；单函数 `cmd_gui()` **1170 行** | 改一处要读几千行；合并冲突地狱 |
| **两套窗口系统并行** | `winsrv`（给 hax 应用）与 `wm`（给内置应用）各自实现窗口、焦点、输入 | 行为不一致，同一 bug 要修两遍 |
| **手写命中测试** | **40 处**散落的 `mx >= … && mx < …` | 坐标算错/漏判是点击类 bug 的主要来源 |
| **样式硬编码** | **472 处**颜色（gui.c 279 + 应用 193）+ **43 处** `theme_light` 三元分支 | 加一个新主题要改几百处；深浅色不一致 |
| **主题是布尔值** | 只有 `theme_light` 0/1 | 无法支持外部主题包，无法换强调色 |

**结论**：不是"哪里写错了"，而是**缺少分层**。所有职责（状态、绘制、输入、样式、窗口管理）都挤在一层里，所以修一个 bug 会冒出新 bug。

---

## 2. 重构目标（可验证）

1. **应用之间零耦合**：删除一个应用，其余应用与框架照样编译运行
2. **样式零硬编码**：除 `theme.c` 外，全代码库不再出现裸颜色值
3. **主题可外部扩展**：往 `/themes/` 丢一个 `.theme` 文件即可新增主题，无需重新编译
4. **单一窗口系统**：内置应用与 hax 应用走同一套窗口/输入路径
5. **可测试**：控件库能在**宿主机**上单测（不需要启动 QEMU）
6. **不退化**：现有 13 个应用功能全部保留（可分批迁移）

---

## 3. 分层架构

```
┌──────────────────────────────────────────────────────────┐
│ 应用层  apps/app_*.c                                      │
│   只调用控件库 + 读写自己的 app_ctx_t                      │
│   禁止：直接 gui_rect / 直接读全局状态 / 裸颜色              │
├──────────────────────────────────────────────────────────┤
│ 控件库  ui/ui_*.c        ← 新增，本方案核心                 │
│   ui_button / ui_list / ui_tabs / ui_input / ui_dialog …  │
│   无全局状态；每个控件自带状态与命中测试                    │
├──────────────────────────────────────────────────────────┤
│ 主题层  ui/theme.c + ui/theme_file.c  ← 新增               │
│   gui_theme_t 令牌 + .theme 解析 + 内置 dark/light 兜底     │
├──────────────────────────────────────────────────────────┤
│ 框架层  gui/framework.c   ← 从 gui.c 拆出                   │
│   事件循环 / 窗口管理 / 焦点 / 输入分发（收编 wm + winsrv）  │
├──────────────────────────────────────────────────────────┤
│ 渲染层  gui/gui_draw.c  ← 需从 gui.c 抽出（见下方注）        │
│   gui_rect / gui_text / gui_blit …（纯绘制，无状态）        │
└──────────────────────────────────────────────────────────┘
```

**关键原则**：上层可以调下层，下层**永远不知道**上层。渲染层不知道主题，控件层不知道窗口，应用层不知道绘制细节。

> **注**：`src/gui/gui_draw.h` 的**声明**已存在，但**实现全部在 `src/tools/gui.c` 里**
> （实测：`gui_rect` 定义在 `gui.c:8491`）。也就是说渲染原语当前与窗口逻辑、应用状态
> 混在同一个文件。抽出 `gui_draw.c` 是 P4 的一部分，不是"已存在、直接复用"。

---

## 4. 核心数据结构

### 4.1 主题令牌（替代 472 处硬编码）

```c
/* src/ui/theme.h —— 全系统唯一的颜色来源 */
typedef struct {
    /* 面 */
    uint32_t bg, surface, surface2, surface3;
    uint32_t line, line2;
    /* 字 */
    uint32_t fg, fg2, dim;
    /* 强调 */
    uint32_t accent, accent_fg, accent_hover, accent_active;
    /* 语义 */
    uint32_t ok, warn, err, info;
    uint32_t sel;                    /* 选中底色（可含 alpha） */
    /* 几何 */
    uint8_t  radius, radius_small, radius_large;
    uint8_t  shadow;                 /* 0=无 1=轻 2=中 */
    uint8_t  border_w;               /* 0=无边框 1=细 2=粗 */
    uint8_t  is_dark;                /* 供少数需要感知明暗的绘制逻辑 */
    char     name[32];               /* 主题名，设置界面显示 */
} gui_theme_t;

/* 只读访问：返回当前主题指针（永不返回 NULL） */
const gui_theme_t *ui_theme(void);
int  ui_theme_set(const char *name);      /* 按名切换，失败返回 -1 */
int  ui_theme_reload(void);                /* 重新扫描 /themes/ */
int  ui_theme_scan(char names[][32], int max);  /* 列出可用主题 */
```

**迁移策略**：`theme_light ? A : B` → `ui_theme()->surface2`。旧代码里 `cyber_neon_pink(light)` 这类函数保留为**过渡适配层**（内部转调 `ui_theme()`），这样 193 处应用代码可以分批改，不必一次全动。

### 4.2 应用上下文（替代上帝对象）

```c
/* src/ui/ui_app.h —— 每个应用实例独立一份 */
typedef struct ui_app {
    const struct ui_app_vt *vt;   /* 应用实现的虚表 */
    struct ui_window *win;        /* 我所在的窗口 */
    int   x, y, w, h;             /* 内容区（窗口内坐标） */
    void *state;                  /* ★ 应用私有状态，框架不碰 */
    int   dirty;
    struct ui_app *next;
} ui_app_t;
```

应用自定义状态：

```c
/* 计算器只声明自己需要的 */
typedef struct { char op; double acc; int sci; … } calc_state_t;
```

**效果**：计算器的 20 个字段、贪吃蛇的 15 个字段从 `gui_state_t` 里搬走，框架层不再知道"计算器"是什么。

### 4.3 控件（自带状态与命中测试）

```c
/* src/ui/ui_widget.h */
typedef struct {
    int x, y, w, h;
    int hot, active, focus;      /* 交互态，由控件自己维护 */
    int enabled, visible;
    const gui_theme_t *t;        /* 绘制时的令牌快照 */
} ui_widget_t;

/* 每个控件三个职责齐全：绘制 / 命中 / 事件 */
bool ui_button(const ui_button_t *b, const ui_event_t *ev);
```

**统一命中测试**（根治 40 处手写坐标判断）：

```c
static inline bool ui_hit(int x, int y, int w, int h, int mx, int my) {
    return (unsigned)(mx - x) < (unsigned)w &&
           (unsigned)(my - y) < (unsigned)h;
}
```
用无符号比较，负数坐标自动判假——这类边界错误是点击 bug 的常见来源。

---

## 5. 主题包（`/themes/*.theme`）

**格式**（纯文本 `key = value`，解析器约 150 行）：

```ini
# HBOS 主题包 v1
name = Breeze Card Dark
based_on = dark            # 基主题：dark / light
accent = #3daee9
accent_hover = #4db8f0
bg = #191c1f
surface = #24282c
surface2 = #2b3034
surface3 = #33393f
fg = #eff0f1
line = #3a4045
ok = #27ae60
warn = #f67400
err = #da4453
radius = 10
radius_small = 7
shadow = soft
```

**加载与回退**（每一步都必须能安全降级）：

| 情况 | 行为 |
|---|---|
| 文件不存在 | 回退内置 dark/light |
| 只写了部分键 | 其余继承 `based_on` 基主题 |
| 颜色格式非法 | 该键回退基主题值，**继续加载** |
| 未知键 | 忽略（向前兼容新版本主题包） |
| `based_on` 不存在 | 用内置 dark |
| 文件巨大/损坏 | 限制读取上限，解析失败即整体回退 |
| 全部失败 | **系统必须能启动**（内置主题永不依赖外部文件） |

**切换持久化**：写入 `/startup.cfg`（复用现有 `shell_get/set_startup_pref` 那条已验证的磁盘链路），重启后生效。

---

## 6. 单一窗口系统（收编 wm + winsrv）

现状是内置应用用 `wm`、hax 应用用 `winsrv`，两套逻辑带来行为不一致。重构后：

- **一个** `ui_window_t`，含装饰信息、内容区、事件队列、z-order
- 内置应用：`ui_app_t` 直接持有 `ui_window_t`
- hax 应用：走同一个 `ui_window_t`，只是绘制目标是一块**独立表面**（winsrv 的物理页机制保留）
- 焦点、捕获、关闭、最小化/最大化：**一份**实现

这一步风险最高，因此**放在后期**，且要保留 `winsrv` 现有 syscall ABI（避免破坏 hax 应用）。

---

## 7. 分阶段实施（每阶段可独立验证、可随时停）

| 阶段 | 内容 | 交付 | 风险 |
|---|---|---|---|
| **P0** | 主题层：`theme.c` + `theme_file.c` + 内置 dark/light + `/themes` 加载 | 主题可切换，`startup.cfg` 持久化 | 低 |
| **P1** | 适配层：把 `cyber_neon_*`/`theme_light` 改为转调主题令牌 | 现有 UI 观感统一，**行为不变** | 低 |
| **P2** | 控件库：button/list/input/tabs/scrollbar/dialog + 宿主单测 | 控件可用且有测试 | 中 |
| **P3** | 应用上下文：`ui_app_t` + 私有状态，从上帝对象搬出 | 先迁 2 个应用（文件管理器 + 设置） | 中 |
| **P4** | 框架拆分：`gui.c` 拆为 framework/window/input/render | 8603 行 → 若干 <1000 行模块 | 高 |
| **P5** | 其余 11 个应用迁移到控件库 | 全部统一 | 中 |
| **P6** | 收编 wm + winsrv 为单一窗口系统 | 行为一致 | 高 |

**实际节奏**：P0–P6 已连续完成（激进式，无适配层），每阶段都编译通过、
测试全绿、并在 QEMU 里实机验证。P7–P8 是"把 13 个应用逐个搬到新控件库"
的长尾工作，可随时增量推进而不影响已完成部分。

| 阶段 | 状态 | 说明 |
|---|---|---|
| P0 主题层 | ✅ 完成 | `src/ui/theme.{h,c}` + `src/lib/json.{h,c}`；内置 Breeze Card 深/浅 |
| P1 消灭硬编码 | ✅ 完成 | 472 处 → 0，`scripts/check_no_hardcoded_colors.sh` 守住 |
| P2 控件库 | ✅ 完成 | 基础 + 9 控件；`src/ui/ui_widget.*`、`src/ui/ui_controls.*` |
| P3 窗口管理 | ✅ 完成 | `src/ui/ui_app.*` 收编 wm + winsrv，统一 z 序/焦点/几何 |
| P4 接入真实 GUI | ✅ 完成 | 新增「UI 组件」预览页；修掉 4 处"多清单"缺陷 |
| P5 外部主题包 | ✅ 完成 | 3 个示例主题随系统发布，设置里可选，跨重启保留 |
| P6 单一注册源 | ✅ 完成 | 4 处应用清单合并为 `gui_app_module_t` |
| P7–P8 应用全量迁移 | ⏳ 待办 | 13 个应用逐个改用 `ui_*` 控件与私有 ctx |

---

## 8. 验证方式（重构最容易失控的地方）

1. **宿主单测**：控件库与主题解析器在 PC 上编译测试（`tests/host/` 已有先例，如 `test_inflate_diff.c`）——不启动 QEMU 就能跑
2. **主题解析负例测试**：损坏/缺键/非法色的 `.theme` 文件必须安全降级
3. **回归**：`make smoke` / `make fat32-smoke` / `make tcp-bulk-test` 现有三套必须持续全绿
4. **视觉回归**：关键界面截图人工比对（我已验证过该流程可用）
5. **零硬编码检查**：加一条 grep 断言——`theme.c` 外不得出现 `0x[0-9A-F]{6}` 颜色字面量

---

## 9. 需要你确认的事项

1. **分层与阶段划分**（尤其 P0→P2 先验收）是否认可？
2. **主题包格式**用上面的 `key = value` 纯文本，还是你想要别的（如 JSON）？
3. **旧主题适配层**（保留 `cyber_neon_*` 转调新令牌）——我建议保留一个版本周期以便渐进迁移，之后删除。同意吗？
4. **P4 拆 `gui.c`** 是高风险动作：你希望**激进重构**（快，但中间态可能几天不可用），还是**绞杀式**（新旧并行，逐步切换，始终可用）？我推荐后者。
5. 应用状态搬出上帝对象时，**hax 应用（用户态）** 是否也要一并纳入新框架，还是先只管内置应用？


---

## 10. 实施记录（已完成部分）

### 10.1 新增/修改的文件

| 文件 | 作用 |
|---|---|
| `src/lib/json.{h,c}` | 极简只读 JSON：零分配、支持注释、数值用 ×1000 定点（内核禁 SSE） |
| `src/ui/theme.{h,c}` | 主题令牌表 + 内置深/浅 + `/themes` 扫描 + JSON 覆盖 + 选择持久化 |
| `src/ui/ui_tokens.h` | 应用取色的唯一入口（`UI_SURFACE` / `UI_FG` / …） |
| `src/ui/ui_widget.{h,c}` | 统一命中测试 `ui_hit()` + 交互状态机 + 圆角/阴影绘制 |
| `src/ui/ui_controls.{h,c}` | 按钮/输入/复选/开关/分段/标签/滚动条/列表/对话框 |
| `src/ui/ui_app.{h,c}` | 统一窗口管理器 `ui_wm_t` + 应用虚表与实例 `ui_app_t` |
| `src/tools/theme_seed.c` | 随系统发布 3 个示例主题包 |
| `src/gui/apps/app_uigallery.c` | 「UI 组件」预览页（新库的活文档 + 自检页） |
| `tests/host/test_{json,theme,widget,controls,wm}.c` | 495 项宿主机单测 |
| `scripts/check_no_hardcoded_colors.sh` | 禁止硬编码颜色的静态断言 |

### 10.2 开发过程中发现并修掉的真实缺陷

这些都不是"看起来不对"，而是会崩、会丢功能、或会让用户困惑：

| # | 缺陷 | 后果 | 修法 |
|---|---|---|---|
| 1 | `json_doc_t` 约 5.4 KB 放在栈上（两处叠加 10.6 KB），而任务栈只有 8 KB | **进桌面直接 page fault**，故障地址落在无关函数里极难定位 | `json_alloc/free/reset` 走堆；拆出 `ui_theme_apply_json` 复用已解析文档 |
| 2 | `load_theme_file` 用 `fs_find_file`（file_t 接口），但主题可能来自 `vfs_register_static_file`（VFS 独立表） | **外部主题永远加载不了**（ls 能看、cat 能读，扫描读不到） | 全面改用 VFS 接口 |
| 3 | `vfs_readdir_at` 非根路径只看真实 fs；根路径只枚举硬编码 `roots[]` | **`/themes` 下的文件列不出来** | 两张表拼接 + 去重 + 下溢防护 |
| 4 | `draw_sb`/`app_calc` 手写 `(c & 0xff) + N` 无钳制 | 通道 255 时溢出污染相邻通道，**蓝底按钮渲成彩虹色** | 改用内部已钳制的 `ui_shade()` |
| 5 | `ui_hit` 只靠无符号比较 | `w<0` 时 `(unsigned)w` 极大 → **矩形左侧一大片都算命中** | 显式排除非正尺寸 |
| 6 | `wm->count` 同时当 windows[] 高水位与 z[] 长度 | 关窗后 z[] 尾部残留 -1 被当有效索引 | 分离 `count` / `z_count` |
| 7 | 最大化→最小化→还原 未记录前置状态 | 回到"最大化尺寸但状态记为常规"的错配 | 记录 `state_before_min` |
| 8 | `ui_app_find` 的 unique 复用语义未定义 | 点任务栏图标回到最近而非最初的窗口 | 明确为"最早创建的实例" |
| 9 | 主题文件写注释（人类会写）但 JSON 不支持 | 整个主题文件解析失败 | JSON 解析器支持行/块注释 |
| 10 | 应用元数据分散 4 处 | "应用能用但开始菜单搜不到" | 合并为 `gui_app_module_t` 单一来源 |

### 10.3 验证手段

- **宿主机单测**：495 项，秒级回归，覆盖主题降级、控件交互、窗口状态机不变量
- **突变测试**：故意破坏"点击成立条件""滚动条钳制"，确认测试会失败（避免空跑）
- **QEMU 实机**：主题切换、UI 组件页、外部主题包、开始菜单、窗口装饰均截图验证
- **静态断言**：`check_no_hardcoded_colors.sh` 挂进 `make ui-test`
- **回归**：`make smoke` / `make fat32-smoke` / `make tcp-bulk-test` 保持全绿
