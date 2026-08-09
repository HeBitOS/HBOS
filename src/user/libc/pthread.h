#ifndef HBOS_USER_LIBC_PTHREAD_H
#define HBOS_USER_LIBC_PTHREAD_H

/* Single-threaded stub pthread API: HBOS .hax apps run one thread per
 * process, so mutexes are no-ops.  Provided so third-party code that
 * references pthread symbols (e.g. quickjs's class-id/atomics locks)
 * links and works without a real pthread implementation. */

typedef int pthread_mutex_t;
typedef struct { int __v; } pthread_mutexattr_t;
typedef struct { int __v; } pthread_cond_t;
typedef struct { int __v; } pthread_condattr_t;

#define PTHREAD_MUTEX_INITIALIZER 0
#define PTHREAD_COND_INITIALIZER 0

static inline int pthread_mutex_init(pthread_mutex_t *m,
                                     const pthread_mutexattr_t *a) {
    (void)a;
    *m = 0;
    return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m;
    return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m) {
    (void)m;
    return 0;
}
static inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    (void)m;
    return 0;
}
static inline int pthread_cond_init(pthread_cond_t *c,
                                    const pthread_condattr_t *a) {
    (void)a;
    c->__v = 0;
    return 0;
}
static inline int pthread_cond_destroy(pthread_cond_t *c) {
    (void)c;
    return 0;
}
static inline int pthread_cond_signal(pthread_cond_t *c) {
    (void)c;
    return 0;
}
static inline int pthread_cond_broadcast(pthread_cond_t *c) {
    (void)c;
    return 0;
}
/* HBOS is single-threaded: a waiter can never be woken by another
 * thread, so report an immediate timeout (Atomics.wait semantics). */
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    (void)c;
    (void)m;
    return 0;
}
static inline int pthread_cond_timedwait(pthread_cond_t *c,
                                         pthread_mutex_t *m,
                                         const struct timespec *t) {
    (void)c;
    (void)m;
    (void)t;
    return 0;   /* ETIMEDOUT would need errno; 0 keeps callers simple */
}

#endif
