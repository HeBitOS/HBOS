#include "hax_applet.h"
HBOS_APPLET_META(hbos_touch_meta, "touch", "BusyBox touch (create/no-op timestamp update)");
#include "touch.c"

int main(int argc, char **argv) {
    applet_name = "touch";
    return touch_main(argc, argv);
}
