#include "../errno.h"
#include "../string.h"
#include "app.h"

extern const hbos_app_t *__start_hbos_apps[];
extern const hbos_app_t *__stop_hbos_apps[];

uint32_t hbos_app_count(void) {
    return (uint32_t)(__stop_hbos_apps - __start_hbos_apps);
}

const hbos_app_t *hbos_app_get(uint32_t index) {
    if (index >= hbos_app_count()) return 0;
    return __start_hbos_apps[index];
}

const hbos_app_t *hbos_app_find(const char *name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < hbos_app_count(); i++) {
        const hbos_app_t *app = hbos_app_get(i);
        if (app && strcmp(app->name, name) == 0) return app;
    }
    return 0;
}

int hbos_app_run(const char *name, int argc, char **argv) {
    const hbos_app_t *app = hbos_app_find(name);
    if (!app || !app->main) {
        errno = ENOENT;
        return -1;
    }
    return app->main(argc, argv);
}
