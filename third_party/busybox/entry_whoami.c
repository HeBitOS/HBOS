#include "hax_applet.h"
HBOS_APPLET_META(hbos_whoami_meta, "whoami", "BusyBox whoami (always 'root' -- no multi-user model)");
#include "whoami.c"

int main(int argc, char **argv) {
    applet_name = "whoami";
    return whoami_main(argc, argv);
}
