/* 多级优先级队列单测：优先级顺序、同级 FIFO、摘除、老化、计数一致性。 */
#include <stdio.h>
#include <string.h>
#include "core/sched_core.h"

static int failures;
#define CHECK(e) do { if (!(e)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #e); failures++; } } while (0)

typedef struct {
    sched_node_t node;
    int id;
} item_t;

static int pick_id(sched_queue_t *q) {
    sched_node_t *n = schedq_pick(q);
    return n ? ((item_t *)((char *)n - offsetof(item_t, node)))->id : -1;
}

static void reset_items(item_t *it, int n) {
    for (int i = 0; i < n; i++) {
        memset(&it[i], 0, sizeof(it[i]));
        it[i].id = i;
    }
}

int main(void) {
    sched_queue_t q;
    item_t it[8];
    reset_items(it, 8);

    printf("[SCHED] 优先级顺序\n");
    reset_items(it, 8);
    schedq_init(&q, 0);
    CHECK(schedq_push(&q, &it[0].node, 2) == 0);
    CHECK(schedq_push(&q, &it[1].node, 0) == 0);
    CHECK(schedq_push(&q, &it[2].node, 1) == 0);
    CHECK(schedq_count(&q) == 3);
    CHECK(pick_id(&q) == 1);
    CHECK(pick_id(&q) == 2);
    CHECK(pick_id(&q) == 0);
    CHECK(schedq_empty(&q));

    printf("[SCHED] 同级 FIFO\n");
    reset_items(it, 8);
    schedq_init(&q, 0);
    for (int i = 0; i < 4; i++) CHECK(schedq_push(&q, &it[i].node, 1) == 0);
    CHECK(pick_id(&q) == 0);
    CHECK(pick_id(&q) == 1);
    CHECK(pick_id(&q) == 2);
    CHECK(pick_id(&q) == 3);

    printf("[SCHED] 摘除中间节点\n");
    reset_items(it, 8);
    schedq_init(&q, 0);
    for (int i = 0; i < 3; i++) CHECK(schedq_push(&q, &it[i].node, 2) == 0);
    CHECK(schedq_remove(&q, &it[1].node) == 0);
    CHECK(schedq_count(&q) == 2);
    CHECK(pick_id(&q) == 0);
    CHECK(pick_id(&q) == 2);
    CHECK(schedq_remove(&q, &it[2].node) == -1);   /* 已出队 */

    printf("[SCHED] 重复入队被拒\n");
    reset_items(it, 8);
    schedq_init(&q, 0);
    CHECK(schedq_push(&q, &it[0].node, 0) == 0);
    CHECK(schedq_push(&q, &it[0].node, 1) == -1);
    CHECK(schedq_count(&q) == 1);

    printf("[SCHED] 老化：低优先级升一级\n");
    reset_items(it, 8);
    schedq_init(&q, 2);                 /* 每 2 次取出老化一次 */
    CHECK(schedq_push(&q, &it[0].node, 3) == 0);   /* L1 */
    CHECK(schedq_push(&q, &it[1].node, 3) == 0);   /* L2 */
    CHECK(schedq_push(&q, &it[2].node, 0) == 0);   /* H  */
    CHECK(pick_id(&q) == 2);            /* H，picks=1 */
    CHECK(pick_id(&q) == 0);            /* L1，picks=2 → 老化 */
    CHECK(it[1].node.level == 2);       /* L2 从 3 升到 2 */
    CHECK(pick_id(&q) == 1);

    printf("[SCHED] 容量与计数一致性（10000 次）\n");
    reset_items(it, 8);
    schedq_init(&q, 7);
    int live = 0, next_push = 0;
    for (int step = 0; step < 10000; step++) {
        int op = (step * 2654435761u) % 3;
        if (op == 0 && next_push < 8) {
            if (schedq_push(&q, &it[next_push].node, (uint8_t)(step % SCHED_LEVELS)) == 0) {
                live++;
                next_push = (next_push + 1) % 8;
            }
        } else if (op == 1) {
            if (pick_id(&q) >= 0) live--;
        } else {
            for (int i = 0; i < 8; i++) {
                if (it[i].node.queued) { schedq_remove(&q, &it[i].node); live--; break; }
            }
        }
        if ((int)schedq_count(&q) != live) { CHECK(0 && "count mismatch"); break; }
    }

    if (failures) { printf("[SCHED] FAILED: %d\n", failures); return 1; }
    printf("[SCHED] all tests passed\n");
    return 0;
}
