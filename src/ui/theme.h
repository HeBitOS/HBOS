/**
 * @file    theme.h
 * @brief   全局主题令牌 —— 全系统唯一的颜色/几何来源。
 *
 * 为什么要有这一层：重构前，颜色硬编码散落在 472 处（gui.c 279 + 应用 193），
 * 深浅色靠 43 处 `theme_light ? A : B` 三元分支。加一个主题要改几百处，且
 * 深浅色容易不一致。这里把所有视觉常量收敛成一张令牌表：
 *
 *   - **应用只读令牌**：`const gui_theme_t *T = ui_theme();` 然后 `T->surface`。
 *     任何地方再写裸颜色值都视为缺陷（CI 里的 grep 断言会拦）。
 *   - **主题来自外部文件**：`/themes/*.theme`（JSON），丢文件即可新增主题，
 *     不必重新编译。加载失败逐级回退，内置 dark/light 永远兜底，保证
 *     坏主题不会让系统起不来。
 */
#ifndef HBOS_UI_THEME_H
#define HBOS_UI_THEME_H

#include <stdbool.h>
#include <stdint.h>

/** 主题名长度上限（含 NUL） */
#define UI_THEME_NAME_MAX 32
/** 可列出的外部主题数上限 */
#define UI_THEME_MAX_SLOTS 16

/**
 * @brief 主题令牌表。
 *
 * 颜色一律是 0xAARRGGBB。约定：`line`/`sel` 这类可以带 alpha（半透明覆盖层
 * 用 alpha 混合绘制），其余面/字色通常是不透明。
 */
typedef struct {
    /* ── 面（由低到高的层次）── */
    uint32_t bg;          /**< 桌面/最底层背景 */
    uint32_t surface;     /**< 卡片/窗口主面 */
    uint32_t surface2;    /**< 次级面：工具栏、输入框、状态栏 */
    uint32_t surface3;    /**< 悬浮/激活/按下的面 */

    /* ── 线与分隔 ── */
    uint32_t line;        /**< 常规分隔线（可带 alpha） */
    uint32_t line2;       /**< 强分隔/控件描边 */

    /* ── 文字（由强到弱）── */
    uint32_t fg;          /**< 主文字 */
    uint32_t fg2;         /**< 次文字 */
    uint32_t dim;         /**< 弱文字：元信息、占位符、禁用 */

    /* ── 强调 ── */
    uint32_t accent;        /**< 主强调色：主要按钮、选中、链接、焦点环 */
    uint32_t accent_fg;     /**< 强调色上的前景（文字/图标） */
    uint32_t accent_hover;  /**< 强调色悬浮态 */
    uint32_t accent_active; /**< 强调色按下态 */

    /* ── 语义状态 ── */
    uint32_t ok, warn, err, info;

    /* ── 选中底色（可带 alpha）── */
    uint32_t sel;

    /* ── 几何 ── */
    uint8_t  radius;        /**< 常规圆角（卡片/窗口） */
    uint8_t  radius_small;  /**< 控件圆角（按钮/输入框） */
    uint8_t  radius_large;  /**< 大容器圆角（对话框） */
    uint8_t  border_w;      /**< 边框宽度：0=无 1=细 2=粗 */
    uint8_t  shadow;        /**< 阴影档位：0=无 1=轻 2=中 */

    /* ── 元信息 ── */
    uint8_t  is_dark;       /**< 1=深色主题（少数绘制逻辑需要感知明暗） */
    uint8_t  _pad[3];
    char     name[UI_THEME_NAME_MAX];   /**< 显示名，设置界面用 */
} gui_theme_t;

/**
 * @brief 取当前主题（只读）。
 * @return 永不为 NULL —— 即使外部主题全部加载失败，也返回内置兜底。
 */
const gui_theme_t *ui_theme(void);

/**
 * @brief 初始化主题层：载入内置主题，扫描 /themes/ 并应用已保存的选择。
 * @return 0 成功（含"只有内置主题"的情况）；不会因外部文件损坏而失败
 */
int ui_theme_init(void);

/**
 * @brief 按名切换主题。
 * @param name 主题名（`ui_theme_scan` 返回的名字，或内置 "dark"/"light"）
 * @return 0 成功；-1 未找到（当前主题不变）
 */
int ui_theme_set(const char *name);

/** 按索引切换（设置界面列表用） */
int ui_theme_set_index(int index);

/** 当前主题在列表中的索引；找不到返回 -1 */
int ui_theme_current_index(void);

/**
 * @brief 重新扫描 /themes/ 目录。
 * @return 可用主题数（含内置）
 */
int ui_theme_reload(void);

/**
 * @brief 列出可用主题。
 * @param names 输出缓冲（每项 UI_THEME_NAME_MAX 字节）
 * @param max   最多写入几项
 * @return 实际项数
 */
int ui_theme_scan(char names[][UI_THEME_NAME_MAX], int max);

/** 可用主题总数 */
int ui_theme_count(void);

/** 内置深/浅色主题（供设置界面的"重置"与兜底使用） */
const gui_theme_t *ui_theme_builtin_dark(void);
const gui_theme_t *ui_theme_builtin_light(void);

/**
 * @brief 从 JSON 文本解析一个主题覆盖到 base 上。
 *
 * 只覆盖文件中**声明了的**键；非法值（坏颜色、超范围数字）跳过并沿用 base，
 * 因此半个损坏的文件不会毁掉整个主题。
 *
 * @param json  JSON 文本
 * @param len   长度
 * @param base  基主题（通常是内置 dark/light）
 * @param out   输出
 * @return 成功覆盖的键数；-1 表示 JSON 结构完全无法解析
 */
int ui_theme_parse_json(const char *json, uint32_t len,
                        const gui_theme_t *base, gui_theme_t *out);

/**
 * @brief 把主题切换持久化到 /startup.cfg（跨重启保留）。
 * @return 0 成功；无持久化后端时返回 -1（可接受，仅本次生效）
 */
int ui_theme_save_choice(const char *name);

/** 颜色工具：alpha 混合 （src over dst），供半透明令牌使用 */
uint32_t ui_blend(uint32_t dst, uint32_t src);

/** 按比例调亮/调暗一个颜色（amount 为 -255..255） */
uint32_t ui_shade(uint32_t color, int amount);

#endif /* HBOS_UI_THEME_H */
