/* wdemo —— 并发窗口示例：独立窗口里的计数器，演示与桌面同时运行。
 *
 * 从开始菜单启动；窗口持续自增计数，左键点击窗口加 10，按 q 或点关闭退出。
 * 与全屏 paint 不同，本应用运行时桌面/时钟/其他窗口仍在刷新。 */
#include <hax.h>

HAX_APP("wdemo", "并发窗口示例：计数器", HAX_KIND_GUI);

#define BG    0x121820
#define CARD  0x1E2A38
#define ACCENT 0x14A6E0
#define TEXTC 0xEAF2F8

static void draw(int w, int h, int count) {
    hax_win_clear(BG);
    hax_win_fill(0, 0, w, 30, ACCENT);
    hax_win_text(10, 8, "并发窗口计数器", 0xFFFFFF);

    hax_win_fill(20, 50, w - 40, h - 80, CARD);

    char buf[32];
    int n = count, i = 0;
    char tmp[16];
    if (n == 0) tmp[i++] = '0';
    while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    int j = 0;
    buf[j++] = 'c'; buf[j++] = 'o'; buf[j++] = 'u'; buf[j++] = 'n';
    buf[j++] = 't'; buf[j++] = ':'; buf[j++] = ' ';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;

    hax_win_text(40, h / 2 - 8, buf, TEXTC);
    hax_win_text(40, h - 24, "左键+10  q 退出", 0x8AA0B4);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int w, h;
    if (hax_win_open("wdemo", 320, 220) < 0) {
        hax_println("wdemo 需要从图形桌面启动。");
        return 1;
    }

    int count = 0;
    int tick = 0;
    while (hax_win_active(&w, &h)) {
        /* 每隔若干轮自增，体现窗口在持续刷新 */
        if (++tick % 8 == 0) count++;

        int ev[4];
        int t = hax_win_poll(ev);
        while (t != HAX_EV_NONE) {
            if (t == HAX_EV_KEY && (ev[1] == 'q' || ev[1] == 27)) goto done;
            if (t == HAX_EV_CLOSE) goto done;
            if (t == HAX_EV_MOUSE && (ev[3] & 1)) count += 10;
            t = hax_win_poll(ev);
        }

        draw(w, h, count);
        hax_win_present();
        hax_sleep(0);
    }
done:
    hax_win_close();
    return 0;
}
