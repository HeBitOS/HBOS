#include "hax_applet.h"
HBOS_APPLET_META(hbos_whoami_meta, "whoami", "BusyBox whoami (multi-user uid aware)");
#include "whoami.c"

int main(int argc, char **argv) {
    applet_name = "whoami";
    return whoami_main(argc, argv);
}
