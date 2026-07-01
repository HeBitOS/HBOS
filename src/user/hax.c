/**
 * @file    hax.c
 * @brief   HAX 应用运行时（内核侧）实现
 *
 * 应用表与二进制 blob 由 tools/genhax.py 在构建期生成：
 *   - hax_app_table / hax_app_table_count  ：build/hax_manifest.c
 *   - _binary_build_hax_blob_bin_start     ：build/hax_blob.bin（incbin）
 */
#include "hax_app.h"
#include "../elf.h"
#include "../string.h"
#include "../core/task.h"

/** 由 hax_blob.asm 经 incbin 嵌入的应用二进制总 blob 起始地址 */
extern const uint8_t _binary_build_hax_blob_bin_start[];

#define HAX_MAX_ARGS 32

uint32_t hax_app_count(void) { return hax_app_table_count; }

const hax_app_entry_t *hax_app_at(uint32_t index) {
    if (index >= hax_app_table_count) return 0;
    return &hax_app_table[index];
}

const hax_app_entry_t *hax_app_find(const char *name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < hax_app_table_count; i++) {
        if (strcmp(hax_app_table[i].name, name) == 0)
            return &hax_app_table[i];
    }
    return 0;
}

/* 公共加载逻辑：组装 argv 并生成任务，返回 pid 或 -1（不等待） */
static int hax_spawn_internal(const char *name, int argc, char **argv) {
    const hax_app_entry_t *e = hax_app_find(name);
    if (!e) return -1;

    /* 组装 NULL 结尾的 argv（argv[0] 用应用名兜底） */
    char *spawn_argv[HAX_MAX_ARGS];
    int n = 0;
    if (argc > 0 && argv) {
        for (int i = 0; i < argc && n < HAX_MAX_ARGS - 1; i++)
            spawn_argv[n++] = argv[i];
    } else {
        spawn_argv[n++] = (char *)name;
    }
    spawn_argv[n] = 0;

    const uint8_t *image = _binary_build_hax_blob_bin_start + e->offset;
    return elf64_load_and_spawn(image, e->size, spawn_argv, 0, e->name);
}

int hax_app_spawn(const char *name, int argc, char **argv) {
    return hax_spawn_internal(name, argc, argv);
}

int hax_app_run(const char *name, int argc, char **argv) {
    int pid = hax_spawn_internal(name, argc, argv);
    if (pid < 0) return -1;

    int status = 0;
    if (task_wait((uint32_t)pid, &status) < 0) return -1;
    return status;
}
