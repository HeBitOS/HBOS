#include "hax_applet.h"
HBOS_APPLET_META(hbos_mv_meta, "mv", "BusyBox mv (-f/-i/-n/-T/-t/-v)");
#include "mv.c"

int main(int argc, char **argv) {
    applet_name = "mv";
    return mv_main(argc, argv);
}
