/**
 * @file    app.h
 * @brief   HBOS 用户应用注册与运行接口
 *
 * 定义用户态应用的描述结构和注册宏，以及应用的查找、运行和派生接口。
 * 应用通过 HBOS_APP 宏在编译时注册到 hbos_apps 段中。
 */

#ifndef HBOS_USER_APP_H
#define HBOS_USER_APP_H

#include <stdint.h>

/** 应用主函数类型 */
typedef int (*hbos_app_main_t)(int argc, char **argv);

/**
 * 用户应用描述结构
 * 每个注册的应用都包含名称、描述和入口函数
 */
typedef struct {
    const char *name;           /**< 应用名称 */
    const char *description;    /**< 应用描述 */
    hbos_app_main_t main;       /**< 应用主函数入口 */
} hbos_app_t;

/**
 * @brief 注册一个用户应用
 *
 * 将应用描述符放入 hbos_apps 段，以便运行时自动发现。
 *
 * @param app_name   应用名称字符串
 * @param app_desc   应用描述字符串
 * @param app_main   应用主函数
 */
#define HBOS_APP(app_name, app_desc, app_main) \
    static const hbos_app_t __hbos_app_desc_##app_main = { app_name, app_desc, app_main }; \
    static const hbos_app_t *__hbos_app_ptr_##app_main \
        __attribute__((used, section("hbos_apps"))) = &__hbos_app_desc_##app_main

/** 获取已注册应用的数量 */
uint32_t hbos_app_count(void);

/** 根据索引获取应用描述符 */
const hbos_app_t *hbos_app_get(uint32_t index);

/** 根据名称查找应用描述符 */
const hbos_app_t *hbos_app_find(const char *name);

/** 在当前任务中运行指定应用 */
int hbos_app_run(const char *name, int argc, char **argv);

/** 在新任务中派生运行指定应用 */
int hbos_app_spawn(const char *name, int argc, char **argv);

#endif
