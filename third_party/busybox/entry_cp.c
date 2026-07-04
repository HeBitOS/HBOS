#include "hax_applet.h"
HBOS_APPLET_META(hbos_cp_meta, "cp", "BusyBox cp (-r/-i/-f/-n/-v; no -l/-s/-p attr preserve)");
#include "cp.c"

int main(int argc, char **argv) {
    applet_name = "cp";
    return cp_main(argc, argv);
}
