#include "hax_applet.h"
HBOS_APPLET_META(hbos_dirname_meta, "dirname", "BusyBox dirname");
#include "dirname.c"

int main(int argc, char **argv) {
    applet_name = "dirname";
    return dirname_main(argc, argv);
}
