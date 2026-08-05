#include "errno.h"
#include "syscall.h"

#define ERRNO_SLOT_COUNT 64

typedef struct {
    volatile int tid;
    int value;
} errno_slot_t;

static errno_slot_t errno_slots[ERRNO_SLOT_COUNT];

int *__errno_location(void) {
    long raw_tid = __syscall1(HBOS_SYS_GETTID, 0);
    int tid = raw_tid > 0 ? (int)raw_tid : 1;
    unsigned int start = (unsigned int)tid % ERRNO_SLOT_COUNT;

    for (unsigned int offset = 0; offset < ERRNO_SLOT_COUNT; offset++) {
        errno_slot_t *slot =
            &errno_slots[(start + offset) % ERRNO_SLOT_COUNT];
        int owner = __atomic_load_n(&slot->tid, __ATOMIC_ACQUIRE);
        if (owner == tid) return &slot->value;
        if (owner == 0) {
            int empty = 0;
            if (__atomic_compare_exchange_n(
                    &slot->tid, &empty, tid, 0,
                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return &slot->value;
            if (empty == tid) return &slot->value;
        }
    }

    /*
     * MAX_TASKS is 16, so a full 64-slot table contains stale exited TIDs.
     * Reclaim the hashed slot; active collision would require more live
     * tasks than the kernel permits.
     */
    errno_slot_t *slot = &errno_slots[start];
    __atomic_store_n(&slot->tid, tid, __ATOMIC_RELEASE);
    slot->value = 0;
    return &slot->value;
}

long __syscall_errno(long ret) {
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return ret;
}
