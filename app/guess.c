/* guess —— 猜数字小游戏（GUI 应用：可从桌面启动到终端窗口） */
#include <hax.h>

HAX_APP("guess", "猜数字小游戏（1-100）", HAX_KIND_GUI);

/* 简易伪随机：用 PID 与时间混合做种子 */
static unsigned rng_state;
static unsigned rng_next(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7fff;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    rng_state = (unsigned)hax_pid() * 2654435761u + 1u;
    for (int i = 0; i < 7; i++) rng_next();

    int secret = (int)(rng_next() % 100) + 1;
    hax_println("=== 猜数字 ===");
    hax_println("我想了一个 1 到 100 之间的数，你来猜！(输入 q 退出)");

    char line[32];
    int tries = 0;
    while (1) {
        hax_print("你的猜测> ");
        if (hax_input(line, sizeof(line)) < 0) break;
        if (line[0] == 'q' || line[0] == 'Q') {
            hax_printf("答案是 %d，下次再来！\n", secret);
            break;
        }
        int v = atoi(line);
        tries++;
        if (v < secret)      hax_println("太小了，再大一点。");
        else if (v > secret) hax_println("太大了，再小一点。");
        else {
            hax_printf("猜对了！答案是 %d，你用了 %d 次。\n", secret, tries);
            break;
        }
    }
    return 0;
}
