/**
 * @file    app_runtime.c
 * @brief   HBOS 用户应用运行时 — 应用注册表与派生执行
 *
 * 实现应用的查找、当前任务内运行和新任务派生执行。
 * 应用通过 hbos_apps 段自动链接注册。
 */

#include "../errno.h"
#include "../core/task.h"
#include "../string.h"
#include "app.h"

/** hbos_apps 段起始地址 */
extern const hbos_app_t *__start_hbos_apps[];

/** hbos_apps 段结束地址 */
extern const hbos_app_t *__stop_hbos_apps[];

/** 获取已注册应用的数量 */
uint32_t hbos_app_count(void) {
    return (uint32_t)(__stop_hbos_apps - __start_hbos_apps);
}

/**
 * 根据索引获取应用描述符
 * @param index  应用索引
 * @return 应用描述符指针，越界返回 0
 */
const hbos_app_t *hbos_app_get(uint32_t index) {
    if (index >= hbos_app_count()) return 0;
    return __start_hbos_apps[index];
}

/**
 * 根据名称查找应用
 * @param name  应用名称
 * @return 应用描述符指针，未找到返回 0
 */
const hbos_app_t *hbos_app_find(const char *name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < hbos_app_count(); i++) {
        const hbos_app_t *app = hbos_app_get(i);
        if (app && strcmp(app->name, name) == 0) return app;
    }
    return 0;
}

/**
 * 在当前任务中运行指定应用
 * @param name  应用名称
 * @param argc  参数数量
 * @param argv  参数数组
 * @return 应用返回值，失败返回 -1 并设置 errno
 */
int hbos_app_run(const char *name, int argc, char **argv) {
    const hbos_app_t *app = hbos_app_find(name);
    if (!app || !app->main) {
        errno = ENOENT;
        return -1;
    }
    return app->main(argc, argv);
}

/** 派生应用最大参数数量 */
#define APP_SPAWN_MAX_ARGS 8

/** 派生应用单个参数最大长度 */
#define APP_SPAWN_ARG_LEN 32

/**
 * 派生应用运行时上下文
 * 保存应用的参数副本和任务关联信息
 */
typedef struct {
    const hbos_app_t *app;                          /**< 应用描述符 */
    int argc;                                       /**< 参数数量 */
    char arg_storage[APP_SPAWN_MAX_ARGS][APP_SPAWN_ARG_LEN]; /**< 参数存储区 */
    char *argv[APP_SPAWN_MAX_ARGS];                 /**< 参数指针数组 */
    int used;                                       /**< 槽位是否被占用 */
} spawned_app_t;

/** 派生应用槽位数组，每个任务对应一个 */
static spawned_app_t spawned_apps[MAX_TASKS];

/**
 * 派生任务的入口函数
 * 调用应用主函数并设置退出状态
 * @param arg  指向 spawned_app_t 的指针
 */
static void app_task_entry(void *arg) {
    spawned_app_t *run = (spawned_app_t *)arg;
    int status = 1;
    if (run && run->app && run->app->main) {
        status = run->app->main(run->argc, run->argv);
        run->used = 0;
    }
    task_set_exit_status(status);
}

/**
 * 在新任务中派生运行指定应用
 * 复制参数到独立存储区，创建新任务执行
 * @param name  应用名称
 * @param argc  参数数量
 * @param argv  参数数组
 * @return 新任务 ID，失败返回 -1 并设置 errno
 */
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
