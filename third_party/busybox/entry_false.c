#include "hax_applet.h"
HBOS_APPLET_META(hbos_false_meta, "false", "BusyBox false - always exit failure");
#include "false.c"

int main(int argc, char **argv) {
    applet_name = "false";
    return false_main(argc, argv);
}
