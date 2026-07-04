#include "hax_applet.h"
HBOS_APPLET_META(hbos_mkdir_meta, "mkdir", "BusyBox mkdir (-m, -p; no SELinux -Z)");
#include "mkdir.c"

int main(int argc, char **argv) {
    applet_name = "mkdir";
    return mkdir_main(argc, argv);
}
