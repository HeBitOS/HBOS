#ifndef HBOS_IPC_H
#define HBOS_IPC_H

#include <stdint.h>
#include <stddef.h>

#define IPC_SHM_MAX     16
#define IPC_SHM_SIZE    (64 * 1024 * 1024)

#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_NOWAIT  04000
#define IPC_RMID    0
#define IPC_SET     1
#define IPC_STAT    2

#define SHM_RDONLY  010000

typedef struct {
    int id;
    int key;
    size_t size;
    void *addr;
    int ref_count;
    int used;
    uint32_t creator_pid;
    uint32_t mode;
    uint64_t atime;
    uint64_t dtime;
    uint64_t ctime;
} shm_seg_t;

void ipc_init(void);
int shmget(int key, size_t size, int flags);
void *shmat(int shmid, const void *shmaddr, int flags);
int shmdt(const void *shmaddr);
int shmctl(int shmid, int cmd, void *buf);

#endif