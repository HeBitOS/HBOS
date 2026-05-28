#ifndef HBOS_FD_H
#define HBOS_FD_H

#include <stdbool.h>
#include <stdint.h>
#include "vfs.h"

#define POSIX_MAX_FDS 32

typedef struct {
    bool used;
    vfs_node_t *node;
    uint32_t offset;
    int flags;
} fd_entry_t;

#endif
