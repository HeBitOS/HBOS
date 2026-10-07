/* tests/host/test_theme.c — 主题令牌层的宿主机单元测试。
 *
 * 重点验证"坏主题不能让系统起不来"这条安全性质：
 *   - 损坏/缺键/非法颜色的主题文件 → 逐键降级，绝不能整体崩
 *   - 内置主题永远可用（ui_theme() 绝不返回 NULL）
 *   - JSON 覆盖只改声明的键，未声明的继承基主题
 *
 * 这些性质在 QEMU 里极难构造（要造损坏的磁盘文件并观察加载行为），
 * 放宿主机上可以穷举。 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "ui/theme.h"

/* 由 theme_fs_stub.c 提供：内存文件系统替身，用于测试 /themes 加载 */
extern void stub_reset(void);
extern int  stub_put(const char *path, const char *text);
extern int  stub_add_dir_entry(const char *name);
extern void stub_set_themes_dir(bool exists);

static int g_fail = 0, g_pass = 0;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; printf("  FAIL: %s\n", m); } } while (0)

/* ── 内置主题的完整性 ─────────────────────────────────────── */
static void test_builtins(void) {
    printf("[theme] 内置主题\n");
    const gui_theme_t *d = ui_theme_builtin_dark();
    const gui_theme_t *l = ui_theme_builtin_light();

    CHECK(d && l, "内置主题存在");
    CHECK(d->is_dark == 1, "dark 标记为深色");
    CHECK(l->is_dark == 0, "light 标记为浅色");

    /* 深浅色必须真的不同（防止复制粘贴错误） */
    CHECK(d->bg != l->bg, "深浅底色不同");
    CHECK(d->fg != l->fg, "深浅文字色不同");
    CHECK(d->surface != l->surface, "深浅面不同");

    /* 关键令牌不得为全透明/纯黑误值 */
    const gui_theme_t *ts[2] = { d, l };
    for (int i = 0; i < 2; i++) {
        const gui_theme_t *t = ts[i];
        CHECK(((t->fg >> 24) & 0xFF) == 0xFF, "主文字不透明");
        CHECK(((t->accent >> 24) & 0xFF) == 0xFF, "强调色不透明");
        CHECK(((t->bg >> 24) & 0xFF) == 0xFF, "底色不透明");
        CHECK(t->radius > 0 && t->radius <= 40, "圆角在合理范围");
        CHECK(t->name[0] != '\0', "主题有名字");
    }

    /* 文字与底色对比度：至少不能相同（粗筛，防止设置成同色导致不可读） */
    CHECK((d->fg & 0xFFFFFF) != (d->bg & 0xFFFFFF), "深色：文字与底不同色");
    CHECK((l->fg & 0xFFFFFF) != (l->bg & 0xFFFFFF), "浅色：文字与底不同色");
}

/* ── ui_theme() 永不返回 NULL ─────────────────────────────── */
static void test_never_null(void) {
    printf("[theme] 兜底\n");
    const gui_theme_t *t = ui_theme();
    CHECK(t != NULL, "ui_theme() 不为 NULL");
    CHECK(t->name[0] != '\0', "兜底主题也有名字");

    /* 切换到不存在的主题必须失败且不改变当前主题 */
    const gui_theme_t *before = ui_theme();
    uint32_t bg_before = before->bg;
    CHECK(ui_theme_set("这是一个不存在的主题") == -1, "切换不存在主题返回 -1");
    CHECK(ui_theme()->bg == bg_before, "失败后当前主题不变");

    CHECK(ui_theme_set(NULL) == -1, "NULL 主题名返回 -1");

    /* 越界索引 */
    CHECK(ui_theme_set_index(-1) == -1, "负索引失败");
    CHECK(ui_theme_set_index(99999) == -1, "越界索引失败");
}

/* ── JSON 覆盖：只改声明的键 ──────────────────────────────── */
static void test_override(void) {
    printf("[theme] JSON 覆盖\n");

    const char *j =
        "{\n"
        "  \"name\": \"My Custom Theme\",\n"
        "  \"accent\": \"#ff0000\",\n"
        "  \"radius\": 20,\n"
        "  \"shadow\": 0\n"
        "}";

    gui_theme_t out;
    int n = ui_theme_parse_json(j, (uint32_t)strlen(j), ui_theme_builtin_dark(), &out);
    CHECK(n > 0, "覆盖成功");
    CHECK(out.accent == 0xFFFF0000u, "accent 被覆盖为红色");
    CHECK(out.radius == 20, "radius 被覆盖为 20");
    CHECK(out.shadow == 0, "shadow 被覆盖为 0");
    CHECK(strcmp(out.name, "My Custom Theme") == 0, "name 被覆盖");

    /* 未声明的键必须继承基主题 */
    const gui_theme_t *base = ui_theme_builtin_dark();
    CHECK(out.bg == base->bg, "未声明的 bg 继承基主题");
    CHECK(out.fg == base->fg, "未声明的 fg 继承基主题");
    CHECK(out.err == base->err, "未声明的 err 继承基主题");
    CHECK(out.is_dark == base->is_dark, "is_dark 继承");

    /* 基于浅色主题覆盖，应继承浅色的其余值 */
    gui_theme_t out2;
    CHECK(ui_theme_parse_json(j, (uint32_t)strlen(j), ui_theme_builtin_light(), &out2) > 0,
          "基于浅色覆盖成功");
    CHECK(out2.bg == ui_theme_builtin_light()->bg, "浅色基主题的 bg 被继承");
    CHECK(out2.is_dark == 0, "浅色基主题的 is_dark 被继承");
}

/* ── 非法值必须跳过而不是毁掉整个主题 ─────────────────────── */
static void test_invalid_values(void) {
    printf("[theme] 非法值降级\n");

    const gui_theme_t *base = ui_theme_builtin_dark();

    /* 非法颜色：应跳过，保留基主题值 */
    const char *j = "{\"accent\":\"not-a-color\",\"bg\":\"#gggggg\",\"radius\":10}";
    gui_theme_t out;
    int n = ui_theme_parse_json(j, (uint32_t)strlen(j), base, &out);
    CHECK(n > 0, "至少 radius 生效");
    CHECK(out.accent == base->accent, "非法 accent 保留基主题值");
    CHECK(out.bg == base->bg, "非法 bg 保留基主题值");
    CHECK(out.radius == 10, "合法 radius 生效");

    /* 超范围数值：钳制而非溢出 */
    const char *j2 = "{\"radius\":9999,\"shadow\":-5,\"border_w\":100}";
    CHECK(ui_theme_parse_json(j2, (uint32_t)strlen(j2), base, &out) > 0, "超范围仍解析");
    CHECK(out.radius <= 40, "radius 被钳制在上限");
    CHECK(out.shadow <= 3, "shadow 被钳制");
    CHECK(out.border_w <= 4, "border_w 被钳制");

    /* 类型不符：字符串当数字 → 跳过 */
    const char *j3 = "{\"radius\":\"big\"}";
    CHECK(ui_theme_parse_json(j3, (uint32_t)strlen(j3), base, &out) >= 0, "类型不符不崩");
    CHECK(out.radius == base->radius, "类型不符时保留基主题值");
}

/* ── 完全损坏的 JSON：返回 -1，但不崩 ─────────────────────── */
static void test_broken_json(void) {
    printf("[theme] 损坏文件\n");

    const gui_theme_t *base = ui_theme_builtin_dark();
    gui_theme_t out;

    const char *bad[] = {
        "", "{", "}", "[1,2]", "not json at all",
        "{\"accent\":}", "{\"accent\":\"#fff\"", "{{}}",
        "\x01\x02\x03\x04", "{\"a\":1,}",
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        /* 关键：不能崩溃；返回值要么 -1（整体失败），要么 >=0（部分成功）。
         * 无论哪种，out 都必须是一个完整可用的主题（等于 base 或覆盖后的）。 */
        int n = ui_theme_parse_json(bad[i], (uint32_t)strlen(bad[i]), base, &out);
        CHECK(n == -1 || n >= 0, "损坏输入返回确定值");
        if (n >= 0) {
            CHECK(out.bg == base->bg || ((out.bg >> 24) & 0xFF) == 0xFF,
                  "部分成功时结构完整");
        }
    }
    printf("  （%zu 个损坏输入全部安全）\n", sizeof(bad)/sizeof(bad[0]));

    /* NULL 参数 */
    CHECK(ui_theme_parse_json(NULL, 10, base, &out) == -1, "NULL json");
    CHECK(ui_theme_parse_json("{}", 2, NULL, &out) == -1, "NULL base");
    CHECK(ui_theme_parse_json("{}", 2, base, NULL) == -1, "NULL out");
}

/* ── 颜色工具 ─────────────────────────────────────────────── */
static void test_color_tools(void) {
    printf("[theme] 颜色工具\n");

    /* alpha=255 时 blend 应返回 src 本身 */
    CHECK(ui_blend(0xFF000000u, 0xFFFFFFFFu) == 0xFFFFFFFFu, "不透明 src 直接覆盖");
    /* alpha=0 时返回 dst */
    CHECK(ui_blend(0xFF123456u, 0x00FFFFFFu) == 0xFF123456u, "全透明 src 等于 dst");
    /* 50% 白压黑：结果应居中偏亮（非预乘 source-over 公式） */
    uint32_t mid = ui_blend(0xFF000000u, 0x80FFFFFFu);
    uint32_t r = (mid >> 16) & 0xFF;
    CHECK(r > 100 && r < 180, "半透明白压黑的中间值合理");

    /* shade 边界 */
    CHECK(ui_shade(0xFFFFFFFFu, 100) == 0xFFFFFFFFu, "shade 上调钳制在 255");
    CHECK(ui_shade(0xFF000000u, -100) == 0xFF000000u, "shade 下调钳制在 0");
    uint32_t lighter = ui_shade(0xFF808080u, 16);
    CHECK(((lighter >> 16) & 0xFF) == 0x90, "shade 正常调亮");
}

/* ── 从 /themes 目录加载外部主题（核心功能）───────────────── */
static void test_theme_files(void) {
    printf("[theme] 外部主题文件\n");

    /* 情形 1：目录不存在 → 只有内置主题，不崩 */
    stub_reset();
    stub_set_themes_dir(false);
    ui_theme_reload();
    CHECK(ui_theme_count() == 2, "无 /themes 目录时只有 2 个内置主题");
    CHECK(ui_theme() != NULL, "无外部主题时仍有可用主题");

    /* 情形 2：放入一个合法主题 */
    stub_reset();
    stub_set_themes_dir(true);
    stub_put("/themes/ocean.theme",
             "{\"name\":\"Ocean\",\"based_on\":\"dark\","
             "\"accent\":\"#00a0ff\",\"radius\":12}");
    stub_add_dir_entry("ocean.theme");
    ui_theme_reload();
    CHECK(ui_theme_count() == 3, "加载了 1 个外部主题（2 内置 + 1）");
    CHECK(ui_theme_set("Ocean") == 0, "可按名切换到外部主题");
    const gui_theme_t *t = ui_theme();
    CHECK(t->accent == 0xFF00A0FFu, "外部主题的 accent 生效");
    CHECK(t->radius == 12, "外部主题的 radius 生效");
    CHECK(t->bg == ui_theme_builtin_dark()->bg, "未声明的 bg 继承 based_on");

    /* 情形 3：损坏的主题文件必须被跳过，且不影响其他主题 */
    stub_reset();
    stub_set_themes_dir(true);
    stub_put("/themes/broken.theme", "{ this is not valid json ");
    stub_add_dir_entry("broken.theme");
    stub_put("/themes/good.theme", "{\"name\":\"Good\",\"accent\":\"#123456\"}");
    stub_add_dir_entry("good.theme");
    ui_theme_reload();
    CHECK(ui_theme_count() == 3, "坏主题被跳过，好主题仍加载");
    CHECK(ui_theme_set("Good") == 0, "好主题可用");
    CHECK(ui_theme_set("broken") == -1, "坏主题不在列表中");

    /* 情形 4：非 .theme 后缀的文件被忽略 */
    stub_reset();
    stub_set_themes_dir(true);
    stub_put("/themes/readme.txt", "{\"accent\":\"#ff0000\"}");
    stub_add_dir_entry("readme.txt");
    ui_theme_reload();
    CHECK(ui_theme_count() == 2, "非 .theme 文件被忽略");

    /* 情形 5：空文件被跳过 */
    stub_reset();
    stub_set_themes_dir(true);
    stub_put("/themes/empty.theme", "");
    stub_add_dir_entry("empty.theme");
    ui_theme_reload();
    CHECK(ui_theme_count() == 2, "空文件被跳过");

    /* 情形 6：主题数量超上限时不死循环、不越界 */
    stub_reset();
    stub_set_themes_dir(true);
    for (int i = 0; i < 30; i++) {
        char path[64], name[32], json[64];
        snprintf(name, sizeof(name), "t%d.theme", i);
        snprintf(path, sizeof(path), "/themes/%s", name);
        snprintf(json, sizeof(json), "{\"name\":\"T%d\",\"accent\":\"#%06x\"}", i, i * 7);
        stub_put(path, json);
        stub_add_dir_entry(name);
    }
    ui_theme_reload();
    CHECK(ui_theme_count() <= UI_THEME_MAX_SLOTS, "主题数被钳制在上限内");
    CHECK(ui_theme() != NULL, "超量后仍有可用主题");

    stub_reset();
    stub_set_themes_dir(false);
    ui_theme_reload();
}

int main(void) {
    printf("=== 主题令牌层宿主机测试 ===\n");
    test_builtins();
    test_never_null();
    test_override();
    test_invalid_values();
    test_broken_json();
    test_color_tools();
    test_theme_files();

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    if (g_fail) { printf("FAILED\n"); return 1; }
    printf("PASS\n");
    return 0;
}
