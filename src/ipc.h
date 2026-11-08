/**
 * @file    ipc.h
 * @brief   导出 15 个接口（1 类型 / 9 宏 / 5 函数）
 *
 * @layer   core
 * @owner   src/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  shm_seg_t   结构体 / 类型别名
 *   宏    IPC_SHM_MAX   常量 / 特性宏
 *         IPC_SHM_SIZE  常量 / 特性宏
 *         IPC_CREAT     常量 / 特性宏
 *         IPC_EXCL      常量 / 特性宏
 *         IPC_NOWAIT    常量 / 特性宏
 *         IPC_RMID      常量 / 特性宏
 *         IPC_SET       IPC设置
 *         IPC_STAT      常量 / 特性宏
 *         SHM_RDONLY    常量 / 特性宏
 *   函数  ipc_init    （待补）
 *         shmget      （待补）
 *         shmat       （待补）
 *         shmdt       （待补）
 *         shmctl      （待补）
 */
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