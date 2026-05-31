#ifndef HBOS_USER_APP_H
#define HBOS_USER_APP_H

#include <stdint.h>

typedef int (*hbos_app_main_t)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *description;
    hbos_app_main_t main;
} hbos_app_t;

#define HBOS_APP(app_name, app_desc, app_main) \
    static const hbos_app_t __hbos_app_desc_##app_main = { app_name, app_desc, app_main }; \
    static const hbos_app_t *__hbos_app_ptr_##app_main \
        __attribute__((used, section("hbos_apps"))) = &__hbos_app_desc_##app_main

uint32_t hbos_app_count(void);
const hbos_app_t *hbos_app_get(uint32_t index);
const hbos_app_t *hbos_app_find(const char *name);
int hbos_app_run(const char *name, int argc, char **argv);
int hbos_app_spawn(const char *name, int argc, char **argv);

#endif
