/* 线程可用性探针：pthread_create / join / 共享内存 / futex 唤醒。
 *
 * 为什么单独测这个：Chromium 是重度多线程的（IO 线程、worker、GPU 线程），
 * 一旦 clone(CLONE_THREAD) 有问题，浏览器会"连上了 Wayland 却什么都不做"——
 * 主线程等一个永远不会被 worker 填满的队列，看起来像卡死在初始化。
 *
 * 只用 pthread，不碰任何 Chromium 相关的东西，把内核问题单独隔离出来。
 *
 * 退出码：0 全通过；1 创建失败；2 join 超时（线程没被调度）；
 *         3 共享内存不可见；4 互斥量/条件变量失败。
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define THREADS 4
#define ROUNDS  200

static volatile int g_counter;
static volatile int g_started;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg) {
    long id = (long)arg;
    __sync_fetch_and_add(&g_started, 1);
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&g_mutex);
        g_counter++;
        pthread_mutex_unlock(&g_mutex);
    }
    return (void *)(id + 1);
}

int main(void) {
    printf("thread: creating %d threads\n", THREADS);

    pthread_t threads[THREADS];
    for (long i = 0; i < THREADS; i++) {
        int rc = pthread_create(&threads[i], NULL, worker, (void *)i);
        if (rc != 0) {
            printf("thread: pthread_create failed rc=%d at %ld\n", rc, i);
            return 1;
        }
    }

    /* 等所有线程真的跑起来。如果这里超时，说明 clone 出来的任务
     * 没被调度 —— 那就是内核线程调度的问题，不是库的问题。 */
    for (int spin = 0; spin < 200 && g_started < THREADS; spin++)
        usleep(10000);
    printf("thread: started=%d (expect %d)\n", g_started, THREADS);
    if (g_started < THREADS) {
        printf("thread: FAIL threads did not start\n");
        return 2;
    }

    for (int i = 0; i < THREADS; i++) {
        void *ret = NULL;
        int rc = pthread_join(threads[i], &ret);
        if (rc != 0) {
            printf("thread: pthread_join failed rc=%d\n", rc);
            return 1;
        }
    }

    int expected = THREADS * ROUNDS;
    printf("thread: counter=%d (expect %d)\n", g_counter, expected);
    if (g_counter != expected) {
        /* 少计数几乎必然是 mutex 没起作用（CLONE_THREAD 的共享地址空间
         * 或 futex 唤醒有问题），不是业务逻辑错。 */
        printf("thread: FAIL counter mismatch\n");
        return 3;
    }

    printf("thread: PASS\n");
    return 0;
}
