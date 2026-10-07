/* 浏览器 —— 以标准应用模块的形式接入，使它能出现在应用列表与开始菜单。
 *
 * 背景（这是一个真实回归的修复）：浏览器的实现一直在 gui.c 内部
 * （draw_browser_app / 键鼠分发），但 P6 把应用清单统一到 gui_app_module_t
 * 注册表时，**忘了它没有对应模块**：于是浏览器从应用列表和开始菜单里
 * 消失了，只能靠代码里的 mode 分支间接访问。文件管理器等应用都有模块，
 * 唯独浏览器没有——统一清单时必须逐个核对，而不是只搬表。
 *
 * 本文件提供薄适配层：
 *   - draw/on_click/on_key 转发到 gui.c 里的既有实现
 *   - 元数据（名称/描述/搜索关键词/图标/尺寸）在此声明，于是它自动出现在
 *     应用列表与开始菜单，和其余 14 个应用一致
 *
 * 之所以不把浏览器的 3000 行实现也搬进来：那需要先把 gui_state_t 里的
 * 浏览器状态（约 20 个字段）拆成私有 ctx，属于 P5 的迁移工作。这里先
 * 修掉"应用不见了"这个用户可见的问题，迁移留待后续。
 */
#include "gui_app.h"
#include "../../ui/ui_tokens.h"
#include "gui_draw.h"

/* gui.c 提供的浏览器绘制实现（原先由 mode 分支直接调用）。
 * 输入仍走 gui.c 主循环里的既有分发——浏览器的交互与窗口命中测试
 * （地址栏/导航按钮/链接热点）深度耦合在那边，抽回调需要先把约 20 个
 * 状态字段从 gui_state_t 拆成私有 ctx（P5 的活）。 */
void browser_module_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h);

const gui_app_module_t gui_app_browser = {
    .mode     = GUI_APP_BROWSER,
    .kw       = "web browser liulan wangye http",
    .icon     = ICON_BROWSER,
    .name     = "浏览器",
    .desc     = "打开 HTTP/HTTPS 网页",
    .want_w   = 0,   /* 用框架默认尺寸（浏览器内容自适应） */
    .want_h   = 0,
    .draw     = browser_module_draw,
    .on_key   = 0,   /* 见上方说明：输入仍由主循环分发 */
    .on_tick  = 0,
    .on_click = 0,
};
