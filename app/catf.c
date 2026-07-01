/* catf —— 读取并显示文件内容（TUI 应用） */
#include <hax.h>

HAX_APP("catf", "读取并显示文件内容", HAX_KIND_TUI);

int main(int argc, char **argv) {
    if (argc < 2) {
        hax_println("用法: run catf <文件路径>");
        return 2;                       /* 用法错误 */
    }

    static char buf[8192];
    long n = hax_read_file(argv[1], buf, sizeof(buf) - 1);
    if (n < 0) {
        hax_printf("无法读取文件: %s\n", argv[1]);
        return 1;                       /* 一般错误 */
    }

    buf[n] = '\0';                      /* 当字符串处理 */
    hax_print(buf);
    if (n > 0 && buf[n - 1] != '\n')
        hax_println("");                /* 补一个换行 */

    hax_printf("\n[共 %ld 字节]\n", n);
    return 0;
}
