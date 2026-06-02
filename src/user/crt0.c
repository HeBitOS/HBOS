/**
 * @file    crt0.c
 * @brief   HBOS C Runtime Startup
 *
 * 用户程序入口点。链接器将 _start 设为入口。
 * 初始化 libc 内部状态，调用 main(argc, argv)，
 * 然后通过 exit() 清理退出。
 *
 * 栈布局（由 ELF loader 设置）:
 *   [argc] [argv[0]] [argv[1]] ... [NULL] [envp[0]] ...
 */

#include "../libc/stdlib.h"

extern int main(int argc, char **argv);

void _start(int argc, char **argv) {
    extern int main(int, char **);
    int ret = main(argc, argv);
    exit(ret);
}