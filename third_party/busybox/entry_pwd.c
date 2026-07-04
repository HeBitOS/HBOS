#include "hax_applet.h"
HBOS_APPLET_META(hbos_pwd_meta, "pwd", "BusyBox pwd");
#include "pwd.c"

int main(int argc, char **argv) {
    applet_name = "pwd";
    return pwd_main(argc, argv);
}
