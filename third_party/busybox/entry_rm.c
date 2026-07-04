#include "hax_applet.h"
HBOS_APPLET_META(hbos_rm_meta, "rm", "BusyBox rm (-i/-f/-r/-R/-v, recursive)");
#include "rm.c"

int main(int argc, char **argv) {
    applet_name = "rm";
    return rm_main(argc, argv);
}
