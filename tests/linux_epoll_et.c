/*
 * epoll edge-triggered and one-shot semantics smoke test.
 *
 * Exercises the Linux-compat epoll layer from user space:
 *   - EPOLLET reports only newly-appeared readiness (no re-report while
 *     data stays buffered), and re-arms once the pipe is drained.
 *   - EPOLLONESHOT fires at most once until EPOLL_CTL_MOD re-arms it.
 *
 * Built as a static PIE, packed as a .hax app, run from the QEMU shell
 * with: run linux_epoll_et
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>

static int failures;

static void expect(int condition, const char *name) {
    if (condition) {
        printf("  PASS %s\n", name);
    } else {
        printf("  FAIL %s\n", name);
        failures++;
    }
}

int main(void) {
    int pipe_fds[2] = {-1, -1};
    if (pipe2(pipe_fds, 0) < 0) {
        puts("LINUX_EPOLL_ET: pipe2 failed");
        return 1;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        puts("LINUX_EPOLL_ET: epoll_create1 failed");
        return 2;
    }

    struct epoll_event watch;
    memset(&watch, 0, sizeof(watch));
    watch.events = EPOLLIN | EPOLLET;
    watch.data.fd = pipe_fds[0];
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pipe_fds[0], &watch) < 0) {
        puts("LINUX_EPOLL_ET: EPOLL_CTL_ADD failed");
        return 3;
    }

    struct epoll_event ready[4];
    int count;

    /* No data yet: an immediate poll must see nothing. */
    count = epoll_wait(epfd, ready, 4, 0);
    expect(count == 0, "empty pipe reports nothing");

    /* First write raises an edge. */
    expect(write(pipe_fds[1], "a", 1) == 1, "write a");
    count = epoll_wait(epfd, ready, 4, 0);
    expect(count == 1 && (ready[0].events & EPOLLIN) &&
           ready[0].data.fd == pipe_fds[0],
           "edge fires on first write");

    /* Data still buffered: edge-triggered must NOT re-report. */
    count = epoll_wait(epfd, ready, 4, 0);
    expect(count == 0, "no re-report while data buffered");

    /* Drain the pipe, then a second write raises a new edge. */
    char buffer[8] = {0};
    expect(read(pipe_fds[0], buffer, sizeof(buffer)) == 1, "read a");
    count = epoll_wait(epfd, ready, 4, 0);
    expect(count == 0, "drained pipe quiet");

    expect(write(pipe_fds[1], "b", 1) == 1, "write b");
    count = epoll_wait(epfd, ready, 4, 0);
    expect(count == 1 && (ready[0].events & EPOLLIN),
           "new edge after drain");

    /* One-shot: fires once, stays dormant until MOD re-arms. */
    memset(&watch, 0, sizeof(watch));
    watch.events = EPOLLIN | EPOLLONESHOT;
    watch.data.fd = pipe_fds[0];
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, pipe_fds[0], &watch) < 0) {
        puts("LINUX_EPOLL_ET: MOD to one-shot failed");
        return 4;
    }
    count = epoll_wait(epfd, ready, 4, 0);
    expect(count == 1 && (ready[0].events & EPOLLIN),
           "one-shot fires while data buffered");

    count = epoll_wait(epfd, ready, 4, 0);
    expect(count == 0, "one-shot dormant after firing");

    if (epoll_ctl(epfd, EPOLL_CTL_MOD, pipe_fds[0], &watch) < 0) {
        puts("LINUX_EPOLL_ET: MOD re-arm failed");
        return 5;
    }
    count = epoll_wait(epfd, ready, 4, 0);
    expect(count == 1 && (ready[0].events & EPOLLIN),
           "MOD re-arms one-shot");

    /* Cleanup and also verify DEL removes the watch. */
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, pipe_fds[0], NULL) < 0) {
        puts("LINUX_EPOLL_ET: EPOLL_CTL_DEL failed");
        return 6;
    }
    count = epoll_wait(epfd, ready, 4, 0);
    expect(count == 0, "deleted watch quiet");

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    close(epfd);

    if (failures) {
        printf("LINUX_EPOLL_ET: FAIL (%d)\n", failures);
        return 1;
    }
    puts("LINUX_EPOLL_ET: PASS");
    return 0;
}
