/**
 * @file    theme_seed.c
 * @brief   把随系统发布的示例主题包写进 /themes/。
 *
 * 为什么需要：主题包功能（/themes 下的 .theme，JSON）如果系统里一个样例都没有，
 * 用户就无从知道格式、也无从验证"丢个文件就能换主题"。这里在启动时把几个
 * 示例主题落盘，于是：
 *   - 设置里能立刻看到内置之外的选项
 *   - /themes/ 里有一份可照抄的模板
 *   - 主题包加载路径在真机上被实际走通（不只是单测）
 *
 * 只在文件不存在时写入，因此用户改过或删掉后不会每次开机被覆盖回来。
 */
#include "../vfs.h"
#include "../string.h"
#include "graphics/graphics.h"

/* ── 示例主题 ────────────────────────────────────────────────
 * 全部只覆盖少数几个键，其余继承 based_on —— 这正是主题包的设计意图：
 * 写一个主题只需要改你关心的那几项，不必复制整张令牌表。 */

static const char g_theme_ocean[] =
"{\n"
"  /* 深海蓝：只改强调色与圆角，其余继承深色内置主题 */\n"
"  \"name\": \"Ocean\",\n"
"  \"based_on\": \"dark\",\n"
"  \"dark\": true,\n"
"  \"accent\": \"#00a0ff\",\n"
"  \"accent_hover\": \"#33b4ff\",\n"
"  \"accent_active\": \"#0088dd\",\n"
"  \"sel\": \"#3d00a0ff\",\n"
"  \"radius\": 12,\n"
"  \"radius_small\": 8\n"
"}\n";

static const char g_theme_forest[] =
"{\n"
"  /* 森林绿：改强调色与语义色 */\n"
"  \"name\": \"Forest\",\n"
"  \"based_on\": \"dark\",\n"
"  \"dark\": true,\n"
"  \"accent\": \"#3fb950\",\n"
"  \"accent_hover\": \"#56d364\",\n"
"  \"accent_active\": \"#2ea043\",\n"
"  \"ok\": \"#3fb950\",\n"
"  \"warn\": \"#d29922\",\n"
"  \"sel\": \"#3d3fb950\"\n"
"}\n";

static const char g_theme_paper[] =
"{\n"
"  /* 米色纸：基于浅色主题，降低对比、加暖色调 */\n"
"  \"name\": \"Paper\",\n"
"  \"based_on\": \"light\",\n"
"  \"dark\": false,\n"
"  \"bg\": \"#f2ede4\",\n"
"  \"surface\": \"#fbf8f2\",\n"
"  \"surface2\": \"#f0eae0\",\n"
"  \"surface3\": \"#e6dfd3\",\n"
"  \"fg\": \"#2b2620\",\n"
"  \"accent\": \"#a0522d\",\n"
"  \"accent_hover\": \"#8b4513\",\n"
"  \"border_w\": 1,\n"
"  \"shadow\": 1\n"
"}\n";

typedef struct {
    const char *path;
    const char *text;
} seed_theme_t;

static const seed_theme_t g_themes[] = {
    { "/themes/ocean.theme",  g_theme_ocean  },
    { "/themes/forest.theme", g_theme_forest },
    { "/themes/paper.theme",  g_theme_paper  },
};

void theme_seed_init(void) {
    /* 目录不存在时先建（纯 ramfs 或还没挂盘也能用） */
    if (!vfs_lookup("/themes")) (void)vfs_mkdir("/themes");

    for (unsigned i = 0; i < sizeof(g_themes) / sizeof(g_themes[0]); i++) {
        const seed_theme_t *t = &g_themes[i];

        /* 已存在就不动：用户可能改过或有意删掉，不该每次开机被覆盖 */
        if (vfs_lookup(t->path)) continue;

        uint32_t len = (uint32_t)strlen(t->text);
        /* register_static_file 不拷贝内容，文本是常量数据，生命周期足够 */
        (void)vfs_register_static_file(t->path, (const uint8_t *)t->text, len);
    }
}
