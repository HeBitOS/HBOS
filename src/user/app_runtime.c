#include "../errno.h"
#include "../core/task.h"
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

#define APP_SPAWN_MAX_ARGS 8
#define APP_SPAWN_ARG_LEN 32

typedef struct {
    const hbos_app_t *app;
    int argc;
    char arg_storage[APP_SPAWN_MAX_ARGS][APP_SPAWN_ARG_LEN];
    char *argv[APP_SPAWN_MAX_ARGS];
    int used;
} spawned_app_t;

static spawned_app_t spawned_apps[MAX_TASKS];

static void app_task_entry(void *arg) {
    spawned_app_t *run = (spawned_app_t *)arg;
    int status = 1;
    if (run && run->app && run->app->main) {
        status = run->app->main(run->argc, run->argv);
        run->used = 0;
    }
    task_set_exit_status(status);
}

int hbos_app_spawn(const char *name, int argc, char **argv) {
    const hbos_app_t *app = hbos_app_find(name);
    if (!app || !app->main) {
        errno = ENOENT;
        return -1;
    }

    spawned_app_t *slot = NULL;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (!spawned_apps[i].used) {
            slot = &spawned_apps[i];
            break;
        }
    }
    if (!slot) {
        errno = EAGAIN;
        return -1;
    }

    slot->used = 1;
    slot->app = app;
    if (argc > APP_SPAWN_MAX_ARGS) argc = APP_SPAWN_MAX_ARGS;
    slot->argc = argc;
    for (int i = 0; i < argc; i++) {
        uint32_t j = 0;
        const char *src = argv && argv[i] ? argv[i] : "";
        while (src[j] && j + 1 < APP_SPAWN_ARG_LEN) {
            slot->arg_storage[i][j] = src[j];
            j++;
        }
        slot->arg_storage[i][j] = 0;
        slot->argv[i] = slot->arg_storage[i];
    }

    int id = task_create(app->name, app_task_entry, slot);
    if (id < 0) {
        slot->used = 0;
        errno = EAGAIN;
        return -1;
    }
    return id;
}
