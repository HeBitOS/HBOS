/* hello —— 最小 HAX 示例（终端应用） */
#include <hax.h>

HAX_APP("hello", "最小 HAX 示例：问候并回显参数", HAX_KIND_TUI);

int main(int argc, char **argv) {
    hax_println("你好，HBOS！这是一个 .hax 应用。");
    hax_printf("我的 PID 是 %d\n", hax_pid());

    if (argc > 1) {
        hax_print("收到参数:");
        for (int i = 1; i < argc; i++) hax_printf(" %s", argv[i]);
        hax_print("\n");
    } else {
        hax_println("提示：试试 `run hello 星际 你好`");
    }
    return 0;
}
