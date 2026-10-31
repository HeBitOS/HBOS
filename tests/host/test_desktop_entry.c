/* test_desktop_entry.c —— .desktop 解析/生成的宿主单测
 *
 * desktop_entry.c 刻意不依赖内核 string.h / VFS，所以能直接和它一起编到
 * 宿主二进制里跑。这里覆盖的是**真实包会遇到的形态**：
 *   - Debian 包里常见的多语言 Name[zh_CN]= 混排
 *   - 注释行、空行、键前空格
 *   - NoDisplay / Hidden（后台组件不该进菜单）
 *   - Keywords（中文显示名 + 英文关键词，开始菜单要能用英文搜到）
 *   - 生成 → 再解析的往返一致性
 */
#include <stdio.h>
#include <string.h>

#include "../../src/gui/desktop_entry.h"
#include "../../src/graphics/gui_icons.h"

static int g_fail;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);          \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

static void test_basic(void) {
    const char *text =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "# comment\n"
        "Name=画板\n"
        "Name[zh_CN]=画板\n"
        "Name[en]=Paint\n"
        "Comment=可以画画的图形应用\n"
        "Exec=/home/linpinf/.local/bin/hivepaint\n"
        "Icon=paint\n"
        "Terminal=false\n"
        "Categories=Graphics;Utility;\n"
        "Keywords=paint huaban\n"
        "X-HBOS-Package=hivepaint\n";
    de_entry_t e;
    int ok = de_parse(text, &e);
    CHECK(ok == 1, "解析基础条目");
    CHECK(strcmp(e.name, "画板") == 0, "Name 取主键，不被 locale 变体覆盖");
    CHECK(strcmp(e.exec, "/home/linpinf/.local/bin/hivepaint") == 0, "Exec 路径");
    CHECK(strcmp(e.package, "hivepaint") == 0, "X-HBOS-Package");
    CHECK(strcmp(e.keywords, "paint huaban") == 0, "Keywords");
    CHECK(e.terminal == 0, "Terminal=false");
    CHECK(e.no_display == 0, "默认可见");
    CHECK(de_has_category(e.categories, "graphics") == 1, "分类忽略大小写");
    CHECK(de_has_category(e.categories, "game") == 0, "不存在的分类");
}

static void test_nodisplay_and_missing(void) {
    de_entry_t e;
    CHECK(de_parse("[Desktop Entry]\nName=后台\nNoDisplay=true\n", &e) == 1,
          "NoDisplay 条目仍可解析");
    CHECK(e.no_display == 1, "NoDisplay=true 生效");

    CHECK(de_parse("[Desktop Entry]\nComment=没有名字\n", &e) == 0,
          "缺 Name 视为无效");
    CHECK(de_parse("[Other]\nName=x\n", &e) == 0,
          "非 Desktop Entry 段忽略");
    CHECK(de_parse("", &e) == 0, "空文本");
    CHECK(de_parse(NULL, &e) == 0, "NULL 输入");
}

static void test_roundtrip(void) {
    de_entry_t e;
    memset(&e, 0, sizeof(e));
    strcpy(e.id, "hivepaint");
    strcpy(e.name, "画板");
    strcpy(e.comment, "画板应用");
    strcpy(e.exec, "/bin/hivepaint");
    strcpy(e.icon_name, "paint");
    strcpy(e.keywords, "paint");
    strcpy(e.categories, "Graphics;");
    strcpy(e.package, "hivepaint");
    e.scope = DE_SCOPE_USER;

    char buf[640];
    uint32_t n = de_format(&e, buf, sizeof(buf));
    CHECK(n > 0, "生成不失败");

    de_entry_t back;
    CHECK(de_parse(buf, &back) == 1, "生成的内容可再解析");
    CHECK(strcmp(back.name, "画板") == 0, "往返 Name");
    CHECK(strcmp(back.exec, "/bin/hivepaint") == 0, "往返 Exec");
    CHECK(strcmp(back.keywords, "paint") == 0, "往返 Keywords");
    CHECK(strstr(buf, "X-HBOS-Scope=user") != NULL, "写入 scope");
}

static void test_icon_and_exec(void) {
    CHECK(de_icon_id("paint") == ICON_NOTES, "paint -> 记事本图标");
    CHECK(de_icon_id("calc") == ICON_CALC, "calc -> 计算器图标");
    CHECK(de_icon_id("system-file-manager") == ICON_FILES, "file 前缀");
    CHECK(de_icon_id("") == ICON_APPS, "空名字回退");

    char base[32];
    de_exec_basename("/opt/calc/bin/calc --flag", base, sizeof(base));
    CHECK(strcmp(base, "calc") == 0, "取 Exec 首段文件名");
}

int main(void) {
    test_basic();
    test_nodisplay_and_missing();
    test_roundtrip();
    test_icon_and_exec();
    if (g_fail) {
        printf("desktop_entry: %d 项失败\n", g_fail);
        return 1;
    }
    printf("desktop_entry 测试通过\n");
    return 0;
}
