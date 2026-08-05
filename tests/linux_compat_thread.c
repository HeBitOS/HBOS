#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <sched.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static unsigned char thread_stack[16384] __attribute__((aligned(16)));
static unsigned char thread_tls[64] __attribute__((aligned(16)));
static volatile int shared_value;
static volatile int child_pid;
static volatile uint64_t child_fs;

struct robust_list_test {
    struct robust_list_test *next;
};

struct robust_head_test {
    struct robust_list_test list;
    long futex_offset;
    struct robust_list_test *pending;
};

struct robust_item_test {
    struct robust_list_test list;
    uint32_t futex;
};

static struct robust_head_test child_robust_head;
static struct robust_item_test child_robust_item;

static int worker(void *argument) {
    shared_value = *(int *)argument;
    child_pid = (int)syscall(SYS_getpid);
    (void)syscall(SYS_arch_prctl, 0x1003L, (long)&child_fs);
    int tid = (int)syscall(SYS_gettid);
    child_robust_item.futex = (uint32_t)tid | 0x80000000U;
    child_robust_item.list.next = &child_robust_head.list;
    child_robust_head.list.next = &child_robust_item.list;
    child_robust_head.futex_offset =
        (long)(offsetof(struct robust_item_test, futex) -
               offsetof(struct robust_item_test, list));
    child_robust_head.pending = NULL;
    if (syscall(SYS_set_robust_list, (long)&child_robust_head,
                (long)sizeof(child_robust_head)) < 0)
        return 90;
    return 0;
}

int main(void) {
    int expected = 0x4842;
    int parent_tid = 0;
    int child_tid = 0;
    int flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                CLONE_THREAD | CLONE_SYSVSEM | CLONE_PARENT_SETTID |
                CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | CLONE_SETTLS;
    int tid = clone(worker, thread_stack + sizeof(thread_stack), flags,
                    &expected, &parent_tid, thread_tls, &child_tid);
    if (tid < 0) {
        puts("LINUX_THREAD: clone failed");
        return 1;
    }
    while (child_tid != 0)
        (void)syscall(SYS_futex, (long)&child_tid, 9, tid, 0L, 0L,
                      0x40000000L);

    int parent_pid = (int)syscall(SYS_getpid);
    if (parent_tid != tid || shared_value != expected ||
        child_pid != parent_pid ||
        child_fs != (uint64_t)(uintptr_t)thread_tls ||
        child_robust_item.futex != 0xc0000000U) {
        puts("LINUX_THREAD: shared state/TID failed");
        return 2;
    }

    int pair[2] = {-1, -1};
    char buffer[8] = {0};
    struct ucred credentials;
    socklen_t credentials_length = sizeof(credentials);
    struct iovec send_vectors[2] = {
        {(void *)"db", 2},
        {(void *)"us", 2}
    };
    struct msghdr send_message = {0};
    send_message.msg_iov = send_vectors;
    send_message.msg_iovlen = 2;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) < 0 ||
        getsockopt(pair[0], SOL_SOCKET, SO_PEERCRED,
                   &credentials, &credentials_length) < 0 ||
        credentials.uid != 0 || credentials.gid != 0 ||
        sendmsg(pair[0], &send_message, 0) != 4) {
        puts("LINUX_THREAD: AF_UNIX setup failed");
        return 3;
    }
    struct pollfd ready = {pair[1], POLLIN, 0};
    struct iovec receive_vector = {buffer, sizeof(buffer)};
    struct msghdr receive_message = {0};
    receive_message.msg_iov = &receive_vector;
    receive_message.msg_iovlen = 1;
    if (poll(&ready, 1, 0) != 1 || !(ready.revents & POLLIN) ||
        recvmsg(pair[1], &receive_message, 0) != 4 ||
        memcmp(buffer, "dbus", 4) != 0) {
        puts("LINUX_THREAD: AF_UNIX data failed");
        return 4;
    }
    if (shutdown(pair[0], SHUT_WR) < 0 ||
        recv(pair[1], buffer, sizeof(buffer), 0) != 0) {
        puts("LINUX_THREAD: AF_UNIX shutdown failed");
        return 5;
    }
    close(pair[0]);
    close(pair[1]);

    int shared_fd = memfd_create("wayland-buffer", MFD_CLOEXEC);
    if (shared_fd < 0 || ftruncate(shared_fd, 4096) < 0) {
        puts("LINUX_THREAD: memfd setup failed");
        return 6;
    }
    uint32_t *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED, shared_fd, 0);
    if (shared == MAP_FAILED) {
        puts("LINUX_THREAD: memfd mmap failed");
        return 7;
    }
    *shared = 0x48424f53U;
    if (munmap(shared, 4096) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        puts("LINUX_THREAD: rights socket failed");
        return 8;
    }

    unsigned char rights_send_control[CMSG_SPACE(sizeof(int))]
        __attribute__((aligned(sizeof(size_t))));
    memset(rights_send_control, 0, sizeof(rights_send_control));
    char marker = 'F';
    struct iovec rights_send_vector = {&marker, 1};
    struct msghdr rights_send = {0};
    rights_send.msg_iov = &rights_send_vector;
    rights_send.msg_iovlen = 1;
    rights_send.msg_control = rights_send_control;
    rights_send.msg_controllen = sizeof(rights_send_control);
    struct cmsghdr *send_header = CMSG_FIRSTHDR(&rights_send);
    send_header->cmsg_len = CMSG_LEN(sizeof(int));
    send_header->cmsg_level = SOL_SOCKET;
    send_header->cmsg_type = SCM_RIGHTS;
    *(int *)CMSG_DATA(send_header) = shared_fd;
    if (sendmsg(pair[0], &rights_send, 0) != 1) {
        puts("LINUX_THREAD: SCM_RIGHTS send failed");
        return 9;
    }
    close(shared_fd);

    unsigned char rights_receive_control[CMSG_SPACE(sizeof(int))]
        __attribute__((aligned(sizeof(size_t))));
    memset(rights_receive_control, 0, sizeof(rights_receive_control));
    char received_marker = 0;
    struct iovec rights_receive_vector = {&received_marker, 1};
    struct msghdr rights_receive = {0};
    rights_receive.msg_iov = &rights_receive_vector;
    rights_receive.msg_iovlen = 1;
    rights_receive.msg_control = rights_receive_control;
    rights_receive.msg_controllen = sizeof(rights_receive_control);
    if (recvmsg(pair[1], &rights_receive, 0) != 1 ||
        received_marker != 'F' ||
        (rights_receive.msg_flags & MSG_CTRUNC)) {
        puts("LINUX_THREAD: SCM_RIGHTS receive failed");
        return 10;
    }
    struct cmsghdr *receive_header = CMSG_FIRSTHDR(&rights_receive);
    if (!receive_header || receive_header->cmsg_level != SOL_SOCKET ||
        receive_header->cmsg_type != SCM_RIGHTS ||
        receive_header->cmsg_len < CMSG_LEN(sizeof(int))) {
        puts("LINUX_THREAD: SCM_RIGHTS header failed");
        return 11;
    }
    int received_fd = *(int *)CMSG_DATA(receive_header);
    shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                  MAP_SHARED, received_fd, 0);
    if (shared == MAP_FAILED || *shared != 0x48424f53U) {
        puts("LINUX_THREAD: shared fd data failed");
        return 12;
    }
    munmap(shared, 4096);
    close(received_fd);
    close(pair[0]);
    close(pair[1]);
    puts("LINUX_THREAD: PASS");
    return 0;
}
