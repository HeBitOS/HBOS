#ifndef HBOS_FD_H
#define HBOS_FD_H

#include <stdbool.h>
#include <stdint.h>
#include "vfs.h"

#define POSIX_MAX_FDS 32

#define FD_FILE    1
#define FD_SOCKET  2
#define FD_PIPE    3

#define PIPE_BUF_SIZE 4096

typedef struct {
    uint8_t buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
    int ref_count;
} pipe_t;

typedef struct {
    bool used;
    vfs_node_t *node;
    uint32_t offset;
    int flags;
    int type;
    uint16_t local_port;
    pipe_t *pipe;
    /* Resolved path this fd was opened with. vfs_node_t itself has no
     * parent/children links (see vfs.h) -- listing a directory's contents
     * only works via the path-string-based fs_readdir()/vfs_readdir_at(),
     * not a per-node op, so a directory fd needs this to answer getdents()
     * (see HBOS_SYS_GETDENTS in syscall.c). */
    char path[256];
} fd_entry_t;

#endif
