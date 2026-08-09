#include <pthread.h>
#include <sched.h>
#include <fenv.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#define THREADS 4
#define ITERATIONS 256

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t phase_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t phase_cond = PTHREAD_COND_INITIALIZER;
static pthread_rwlock_t state_lock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_once_t once_control = PTHREAD_ONCE_INIT;
static int counter;
static int worker_failures;
static int ready_workers;
static int release_workers;
static int shared_state;
static int once_calls;
static __thread int thread_local_value = 17;
static int worker_ids[THREADS];

static void initialize_once(void) {
    once_calls++;
}

static void *worker(void *opaque) {
    int id = *(int *)opaque;
    if (thread_local_value != 17 || fegetround() != FE_DOWNWARD)
        __atomic_add_fetch(&worker_failures, 1, __ATOMIC_RELAXED);
    thread_local_value = 100 + id;
    if (pthread_once(&once_control, initialize_once) != 0)
        __atomic_add_fetch(&worker_failures, 1, __ATOMIC_RELAXED);

    if (pthread_mutex_lock(&phase_lock) != 0)
        return (void *)(uintptr_t)1;
    ready_workers++;
    if (pthread_cond_broadcast(&phase_cond) != 0)
        __atomic_add_fetch(&worker_failures, 1, __ATOMIC_RELAXED);
    while (!release_workers) {
        if (pthread_cond_wait(&phase_cond, &phase_lock) != 0) {
            __atomic_add_fetch(&worker_failures, 1, __ATOMIC_RELAXED);
            break;
        }
    }
    if (pthread_mutex_unlock(&phase_lock) != 0)
        __atomic_add_fetch(&worker_failures, 1, __ATOMIC_RELAXED);

    if (pthread_rwlock_rdlock(&state_lock) != 0) {
        __atomic_add_fetch(&worker_failures, 1, __ATOMIC_RELAXED);
    } else {
        if (shared_state != 0x4850)
            __atomic_add_fetch(&worker_failures, 1, __ATOMIC_RELAXED);
        if (pthread_rwlock_unlock(&state_lock) != 0)
            __atomic_add_fetch(&worker_failures, 1, __ATOMIC_RELAXED);
    }
    for (int i = 0; i < ITERATIONS; i++) {
        if (pthread_mutex_lock(&counter_lock) != 0) {
            __atomic_add_fetch(&worker_failures, 1, __ATOMIC_RELAXED);
            break;
        }
        counter++;
        if (pthread_mutex_unlock(&counter_lock) != 0) {
            __atomic_add_fetch(&worker_failures, 1, __ATOMIC_RELAXED);
            break;
        }
        if ((i & 15) == 0) sched_yield();
    }
    if (thread_local_value != 100 + id)
        __atomic_add_fetch(&worker_failures, 1, __ATOMIC_RELAXED);
    return (void *)(uintptr_t)(0x100 + id);
}

int main(void) {
    pthread_t threads[THREADS];
    if (fesetround(FE_DOWNWARD) != 0) return 1;
    for (int i = 0; i < THREADS; i++) {
        worker_ids[i] = i;
        if (pthread_create(&threads[i], 0, worker, &worker_ids[i]) != 0)
            return 2;
    }

    if (pthread_mutex_lock(&phase_lock) != 0) return 6;
    while (ready_workers != THREADS) {
        if (pthread_cond_wait(&phase_cond, &phase_lock) != 0)
            return 7;
    }
    if (pthread_rwlock_wrlock(&state_lock) != 0) return 8;
    shared_state = 0x4850;
    if (pthread_rwlock_unlock(&state_lock) != 0) return 9;
    release_workers = 1;
    if (pthread_cond_broadcast(&phase_cond) != 0) return 10;
    if (pthread_mutex_unlock(&phase_lock) != 0) return 11;

    for (int i = 0; i < THREADS; i++) {
        void *result = 0;
        if (pthread_join(threads[i], &result) != 0 ||
            result != (void *)(uintptr_t)(0x100 + i))
            return 3;
    }

    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return 12;
    deadline.tv_nsec += 20000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    if (pthread_mutex_lock(&phase_lock) != 0) return 13;
    int timed_result = pthread_cond_timedwait(&phase_cond, &phase_lock,
                                              &deadline);
    if (pthread_mutex_unlock(&phase_lock) != 0) return 14;
    if (timed_result != ETIMEDOUT) return 15;

    static const char pass[] = "LINUX_MUSL_PTHREAD: PASS\n";
    if (counter != THREADS * ITERATIONS || worker_failures != 0 ||
        thread_local_value != 17 || once_calls != 1 ||
        ready_workers != THREADS || shared_state != 0x4850)
        return 4;
    return write(STDOUT_FILENO, pass, sizeof(pass) - 1) ==
           (long)(sizeof(pass) - 1) ? 0 : 5;
}
