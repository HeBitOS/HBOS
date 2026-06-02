/**
 * @file    ipc.c
 * @brief   System V IPC 实现 — 共享内存
 *
 * 提供 POSIX shmget/shmat/shmdt/shmctl 接口。
 * 共享内存段以页为单位分配，映射到用户地址空间。
 */

#include "ipc.h"
#include "core/task.h"
#include "core/vmm.h"
#include "core/heap.h"
#include "string.h"

static shm_seg_t shm_segs[IPC_SHM_MAX];
static int shm_next_id = 1;

void ipc_init(void) {
    memset(shm_segs, 0, sizeof(shm_segs));
    shm_next_id = 1;
}

int shmget(int key, size_t size, int flags) {
    if (size == 0) return -1;

    for (int i = 0; i < IPC_SHM_MAX; i++) {
        if (shm_segs[i].used && shm_segs[i].key == key) {
            if ((flags & IPC_CREAT) && (flags & IPC_EXCL)) return -1;
            if (flags & IPC_CREAT) {
                if (size > shm_segs[i].size) {
                    shm_segs[i].size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                }
            }
            return shm_segs[i].id;
        }
    }

    if (!(flags & IPC_CREAT)) return -1;

    for (int i = 0; i < IPC_SHM_MAX; i++) {
        if (!shm_segs[i].used) {
            size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
            size_t alloc_size = pages * PAGE_SIZE;

            void *addr = NULL;
            for (uint64_t va = 0x0000200000000000ULL; ; va += PAGE_SIZE * 256) {
                int free = 1;
                for (uint64_t va2 = va; va2 < va + alloc_size; va2 += PAGE_SIZE) {
                    if (vmm_get_phys(va2) != 0) { free = 0; break; }
                }
                if (free) { addr = (void *)va; break; }
            }

            for (size_t p = 0; p < pages; p++) {
                if (!vmm_alloc_page_at((uint64_t)addr + p * PAGE_SIZE, 0x07)) {
                    return -1;
                }
            }

            shm_segs[i].used = 1;
            shm_segs[i].id = shm_next_id++;
            shm_segs[i].key = key;
            shm_segs[i].size = alloc_size;
            shm_segs[i].addr = addr;
            shm_segs[i].ref_count = 0;
            shm_segs[i].creator_pid = task_get_id();
            shm_segs[i].mode = (uint32_t)(flags & 0777);
            shm_segs[i].ctime = 0;
            shm_segs[i].atime = 0;
            shm_segs[i].dtime = 0;

            return shm_segs[i].id;
        }
    }

    return -1;
}

void *shmat(int shmid, const void *shmaddr, int flags) {
    (void)shmaddr; (void)flags;
    for (int i = 0; i < IPC_SHM_MAX; i++) {
        if (shm_segs[i].used && shm_segs[i].id == shmid) {
            shm_segs[i].ref_count++;
            return shm_segs[i].addr;
        }
    }
    return (void *)-1;
}

int shmdt(const void *shmaddr) {
    if (!shmaddr) return -1;
    for (int i = 0; i < IPC_SHM_MAX; i++) {
        if (shm_segs[i].used && shm_segs[i].addr == shmaddr) {
            if (shm_segs[i].ref_count > 0)
                shm_segs[i].ref_count--;
            return 0;
        }
    }
    return -1;
}

int shmctl(int shmid, int cmd, void *buf) {
    for (int i = 0; i < IPC_SHM_MAX; i++) {
        if (shm_segs[i].used && shm_segs[i].id == shmid) {
            switch (cmd) {
                case IPC_RMID:
                    if (shm_segs[i].ref_count > 0) return -1;
                    for (size_t p = 0; p < shm_segs[i].size; p += PAGE_SIZE)
                        vmm_unmap_page((uint64_t)shm_segs[i].addr + p);
                    memset(&shm_segs[i], 0, sizeof(shm_seg_t));
                    return 0;
                case IPC_STAT:
                    if (buf) memcpy(buf, &shm_segs[i], sizeof(shm_seg_t));
                    return 0;
                case IPC_SET:
                    return 0;
                default:
                    return -1;
            }
        }
    }
    return -1;
}