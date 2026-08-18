/* app/leapyear/leap.c —— 年份输入解析（多文件应用的第二翻译单元） */
#include <libc/stdlib.h> /* atoi */

/* 解析一行输入为年份：合法（1~10000 整数）返回 0 并写入 *year，否则 -1 */
int leap_parse_year(const char *line, int *year) {
    if (line == 0 || line[0] == 0 || line[0] == '-') return -1;
    int y = atoi(line);
    if (y <= 0 || y > 10000) return -1;
    *year = y;
    return 0;
}
