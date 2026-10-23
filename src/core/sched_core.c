/**
 * @file    sched_core.c
 * @brief   多级优先级就绪队列实现
 */

#include "sched_core.h"

static sched_node_t *q_pop_head(sched_queue_t *q, uint8_t level) {
    sched_node_t *n = q->head[level];
    if (!n) return 0;
    q->head[level] = n->next;
    if (q->head[level]) q->head[level]->prev = 0;
    else q->tail[level] = 0;
    n->prev = n->next = 0;
    n->queued = 0;
    q->count--;
    return n;
}

void schedq_init(sched_queue_t *q, uint32_t age_quantum) {
    if (!q) return;
    for (int i = 0; i < SCHED_LEVELS; i++) {
        q->head[i] = 0;
        q->tail[i] = 0;
    }
    q->count = 0;
    q->picks = 0;
    q->age_quantum = age_quantum;
}

int schedq_push(sched_queue_t *q, sched_node_t *n, uint8_t level) {
    if (!q || !n || n->queued) return -1;
    if (level >= SCHED_LEVELS) level = SCHED_LEVELS - 1;

    n->level = level;
    n->next = 0;
    n->prev = q->tail[level];
    if (q->tail[level]) q->tail[level]->next = n;
    else q->head[level] = n;
    q->tail[level] = n;
    n->queued = 1;
    q->count++;
    return 0;
}

int schedq_remove(sched_queue_t *q, sched_node_t *n) {
    if (!q || !n || !n->queued) return -1;
    uint8_t level = n->level;

    if (n->prev) n->prev->next = n->next;
    else q->head[level] = n->next;

    if (n->next) n->next->prev = n->prev;
    else q->tail[level] = n->prev;

    n->prev = n->next = 0;
    n->queued = 0;
    q->count--;
    return 0;
}

/* 老化：把最低非空级的队首升一级，防止饿死。
 * 只提升一个节点——一次全提会让低优先级瞬间压过高优先级，破坏语义。 */
static void schedq_age(sched_queue_t *q) {
    for (int lv = SCHED_LEVELS - 1; lv > 0; lv--) {
        if (!q->head[lv]) continue;
        sched_node_t *n = q->head[lv];
        (void)schedq_remove(q, n);
        (void)schedq_push(q, n, (uint8_t)(lv - 1));
        return;
    }
}

sched_node_t *schedq_pick(sched_queue_t *q) {
    if (!q || q->count == 0) return 0;

    for (int lv = 0; lv < SCHED_LEVELS; lv++) {
        if (!q->head[lv]) continue;
        sched_node_t *n = q_pop_head(q, (uint8_t)lv);

        q->picks++;
        if (q->age_quantum && (q->picks % q->age_quantum) == 0)
            schedq_age(q);
        return n;
    }
    return 0;
}

uint32_t schedq_count(const sched_queue_t *q) { return q ? q->count : 0; }
int schedq_empty(const sched_queue_t *q) { return !q || q->count == 0; }
