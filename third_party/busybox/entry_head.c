#include "hax_applet.h"
HBOS_APPLET_META(hbos_head_meta, "head", "BusyBox head (-n only, no -c/-q/-v)");
#include "head.c"

int main(int argc, char **argv) {
    applet_name = "head";
    return head_main(argc, argv);
}
