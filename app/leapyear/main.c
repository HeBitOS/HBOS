/* app/leapyear/main.c —— 闰年判定应用（多文件应用示例）
 *
 * 本应用演示多结构 HAX 构建：
 *   app/leapyear/main.c   入口 + 主循环（本文件，含 HAX_APP 声明）
 *   app/leapyear/leap.c   第二翻译单元：输入解析
 *   app/leapyear/deps     声明依赖独立库 leaputil
 *   app/lib/leaputil/     独立库（闰年判定，可被其他应用复用）
 */
#include <hax.h>
#include <libc/stdlib.h>      /* atoi */
#include <leaputil/leaputil.h> /* 独立库头，按 <库名/xxx.h> 引入 */

HAX_APP("leapyear", "闰年判定", HAX_KIND_BOTH);

/* 由同目录 leap.c 提供：解析一行输入为年份，成功返回 0 并写入 *year */
int leap_parse_year(const char *line, int *year);

static void print_banner(void) {
    hax_println("  _       _____      _      ____   __   __  _____      _      ____  ");
    hax_println(" | |     | ____|    / \\    |  _ \\  \\ \\ / / | ____|    / \\    |  _ \\  ");
    hax_println(" | |     |  _|     / _ \\   | |_) |  \\ V /  |  _|     / _ \\   | |_) | ");
    hax_println(" | |___  | |___   / ___ \\  |  __/    | |   | |___   / ___ \\  |  _ <  ");
    hax_println(" |_____| |_____| /_/   \\_\\ |_|       |_|   |_____| /_/   \\_\\ |_| \\_\\ ");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char line[32];
    print_banner();
    while (1) {
        hax_println("请输入要判定的年份(输入 q 退出)> ");
        if (hax_input(line, sizeof(line)) < 0 || line[0] == 'q') {
            break;
        }
        int year;
        if (leap_parse_year(line, &year) < 0) {
            hax_println("输入的年份须为 1~10000 之间的整数(不能为0或非数字、负号-或q以外的字符)。\n");
            continue;
        }
        if (leaputil_is_leap(year))
            hax_printf("%d年是闰年\n", year);
        else
            hax_printf("%d年不是闰年\n", year);
    }
    hax_println("应用已退出。输入 'run leapyear' 或点击应用图标再次打开\n");
    return 0;
}
