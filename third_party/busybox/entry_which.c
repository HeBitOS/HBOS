#include "hax_applet.h"
HBOS_APPLET_META(hbos_which_meta, "which", "BusyBox which (-a)");
#include "which.c"

int main(int argc, char **argv) {
    applet_name = "which";
    return which_main(argc, argv);
}
