#include "hax_applet.h"
HBOS_APPLET_META(hbos_true_meta, "true", "BusyBox true - always exit success");
#include "true.c"

int main(int argc, char **argv) {
    applet_name = "true";
    return true_main(argc, argv);
}
