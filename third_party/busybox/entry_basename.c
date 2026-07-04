#include "hax_applet.h"
HBOS_APPLET_META(hbos_basename_meta, "basename", "BusyBox basename (FILE [SUFFIX])");
#include "basename.c"

int main(int argc, char **argv) {
    applet_name = "basename";
    return basename_main(argc, argv);
}
