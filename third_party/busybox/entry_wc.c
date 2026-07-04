#include "hax_applet.h"
HBOS_APPLET_META(hbos_wc_meta, "wc", "BusyBox wc (-lwmcL)");
#include "wc.c"

int main(int argc, char **argv) {
    applet_name = "wc";
    return wc_main(argc, argv);
}
