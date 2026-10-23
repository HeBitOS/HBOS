/**
 * @file    sched_core.h
 * @brief   多级优先级就绪队列（O(1) 入队/出队/取最高优先级）
 *
 * 与内核解耦：节点由调用方内嵌在自己的任务结构里，这里只管链表和计数，
 * 因此可以在宿主机上直接单测。空间 O(N)（每任务一个节点 + 每级两个指针）。
 *
 * 规则：
 *   - level 0 最高，SCHED_LEVELS-1 最低；同级 FIFO（先来后到）。
 *   - 取任务只看最高非空级，扫描级数是常数 → O(1)。
 *   - 老化：每 age_quantum 次取出，把最低非空级的队首升一级，
 *     避免低优先级任务饿死（对应笔记里的"同级先来后到"）。
 */
#ifndef HBOS_SCHED_CORE_H
#define HBOS_SCHED_CORE_H

#include <stdint.h>

#define SCHED_LEVELS 4

typedef struct sched_node {
    struct sched_node *prev;
    struct sched_node *next;
    uint8_t level;      /**< 0..SCHED_LEVELS-1 */
    uint8_t queued;     /**< 是否在队列里（防重复 push / 误 remove） */
} sched_node_t;

typedef struct {
    sched_node_t *head[SCHED_LEVELS];
    sched_node_t *tail[SCHED_LEVELS];
    uint32_t count;
    uint32_t picks;         /**< 累计取出次数，用于老化 */
    uint32_t age_quantum;   /**< 0 = 关闭老化 */
} sched_queue_t;

void schedq_init(sched_queue_t *q, uint32_t age_quantum);

/** 入队到指定等级（超出范围会被夹到两端）。已入队返回 -1。 */
int schedq_push(sched_queue_t *q, sched_node_t *n, uint8_t level);

/** 从队列摘除（未入队返回 -1）。 */
int schedq_remove(sched_queue_t *q, sched_node_t *n);

/** 取出最高优先级的队首；空队列返回 NULL。内部会推进老化。 */
sched_node_t *schedq_pick(sched_queue_t *q);

uint32_t schedq_count(const sched_queue_t *q);
int schedq_empty(const sched_queue_t *q);

#endif /* HBOS_SCHED_CORE_H */
