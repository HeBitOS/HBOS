#include "hax_applet.h"
HBOS_APPLET_META(hbos_ls_meta, "ls", "BusyBox ls (-l/-a/-R/-d; no color/columns)");
#include "ls.c"

int main(int argc, char **argv) {
    applet_name = "ls";
    return ls_main(argc, argv);
}
