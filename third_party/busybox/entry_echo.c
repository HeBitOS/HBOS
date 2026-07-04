#include "hax_applet.h"
HBOS_APPLET_META(hbos_echo_meta, "echo", "BusyBox echo (basic: no -n/-e/-E, no escapes)");
#include "echo.c"

int main(int argc, char **argv) {
    applet_name = "echo";
    return echo_main(argc, argv);
}
