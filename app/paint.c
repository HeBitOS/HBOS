/* paint —— GUI 画布示例：用鼠标在画布上画方块，演示真实帧缓冲绘图。
 *
 * 从图形桌面（或 GUI 终端）启动；按住左键拖动绘制，按 c 清屏，q/ESC 退出。
 * 若从纯文本环境启动（无 GUI），回退打印提示。 */
#include <hax.h>

HAX_APP("paint", "GUI 画布示例：鼠标涂鸦", HAX_KIND_GUI);

#define BG      0x10141A
#define BAR     0x14A6E0
#define BRUSH   0xF5C842

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int w, h;
    if (!hax_gui_begin(&w, &h)) {
        hax_println("paint 需要从图形桌面启动（当前无 GUI 画布）。");
        return 1;
    }

    /* 初始画面 */
    hax_gui_clear(BG);
    hax_gui_rect(0, 0, w, 28, BAR);
    hax_gui_text(8, 6, "HBOS Paint  -  左键涂鸦  c 清屏  q 退出", 0xFFFFFF, 1);
    hax_gui_present();

    int prev_btn = 0;
    for (;;) {
        int mx, my;
        int btn = hax_gui_pollmouse(&mx, &my);

        /* 左键按下：在光标处画一个圆形笔刷 */
        if ((btn & 1) && my > 28) {
            hax_gui_fill_circle(mx, my, 5, BRUSH);
            hax_gui_present();
        }
        prev_btn = btn;
        (void)prev_btn;

        int k = hax_gui_pollkey();
        if (k == 'q' || k == 27) break;       /* q 或 ESC 退出 */
        if (k == 'c') {                        /* c 清屏 */
            hax_gui_clear(BG);
            hax_gui_rect(0, 0, w, 28, BAR);
            hax_gui_text(8, 6, "HBOS Paint  -  左键涂鸦  c 清屏  q 退出", 0xFFFFFF, 1);
            hax_gui_present();
        }

        hax_sleep(0);   /* 让出 CPU，避免空转占满 */
    }

    return 0;
}
