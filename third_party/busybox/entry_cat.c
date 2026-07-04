#include "hax_applet.h"
HBOS_APPLET_META(hbos_cat_meta, "cat", "BusyBox cat (basic: no -v/-t/-e/-A/-n/-b)");
#include "cat.c"

int main(int argc, char **argv) {
    applet_name = "cat";
    return cat_main(argc, argv);
}
