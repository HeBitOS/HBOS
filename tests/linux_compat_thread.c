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

static long raw_syscall3(long number, long argument0, long argument1,
                         long argument2) {
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(argument0), "S"(argument1),
                       "d"(argument2)
                     : "rcx", "r11", "memory");
    return result;
}

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
    enum {
        TEST_SIG_BLOCK = 0,
        TEST_SIG_UNBLOCK = 1,
        TEST_SIG_SETMASK = 2,
        TEST_SIGKILL = 9,
        TEST_SIGUSR1 = 10,
        TEST_SIGSTOP = 19
    };
    uint64_t requested_mask =
        (1ULL << (TEST_SIGUSR1 - 1)) |
        (1ULL << (TEST_SIGKILL - 1)) |
        (1ULL << (TEST_SIGSTOP - 1));
    uint64_t observed_mask = 0;
    if (syscall(SYS_rt_sigprocmask, TEST_SIG_BLOCK,
                (long)&requested_mask, 0L, (long)sizeof(requested_mask)) < 0 ||
        syscall(SYS_rt_sigprocmask, TEST_SIG_BLOCK, 0L,
                (long)&observed_mask, (long)sizeof(observed_mask)) < 0 ||
        observed_mask != (1ULL << (TEST_SIGUSR1 - 1)) ||
        syscall(SYS_rt_sigprocmask, TEST_SIG_UNBLOCK,
                (long)&requested_mask, 0L, (long)sizeof(requested_mask)) < 0) {
        puts("LINUX_THREAD: signal mask failed");
        return 13;
    }
    observed_mask = UINT64_MAX;
    if (syscall(SYS_rt_sigprocmask, TEST_SIG_SETMASK, 0L,
                (long)&observed_mask, (long)sizeof(observed_mask)) < 0 ||
        observed_mask != 0) {
        puts("LINUX_THREAD: signal unmask failed");
        return 14;
    }

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

    /* D-Bus discovers and validates named AF_UNIX endpoints through these
     * calls. Exercise native syscall 51 plus libc/HBOS wrappers, abstract
     * names, truncation, accepted-local and peer address direction. */
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un server_address;
    struct sockaddr_un client_address;
    memset(&server_address, 0, sizeof(server_address));
    memset(&client_address, 0, sizeof(client_address));
    server_address.sun_family = AF_UNIX;
    client_address.sun_family = AF_UNIX;
    memcpy(server_address.sun_path + 1, "hbos-dbus", 9);
    memcpy(client_address.sun_path + 1, "hbos-client", 11);
    socklen_t server_length =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + 9);
    socklen_t client_length =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + 11);
    struct sockaddr_un observed_address;
    memset(&observed_address, 0, sizeof(observed_address));
    socklen_t observed_length = sizeof(uint16_t);
    if (listener < 0 || client < 0 ||
        bind(listener, &server_address, server_length) < 0 ||
        bind(client, &client_address, client_length) < 0 ||
        raw_syscall3(SYS_getsockname, listener,
                     (long)&observed_address,
                     (long)&observed_length) != 0 ||
        observed_length != server_length ||
        observed_address.sun_family != AF_UNIX ||
        raw_syscall3(SYS_getpeername, listener,
                     (long)&observed_address,
                     (long)&observed_length) != -107 ||
        listen(listener, 2) < 0 ||
        connect(client, &server_address, server_length) < 0) {
        puts("LINUX_THREAD: AF_UNIX address setup failed");
        return 15;
    }
    memset(&observed_address, 0, sizeof(observed_address));
    observed_length = sizeof(observed_address);
    int accepted = accept(listener, &observed_address, &observed_length);
    if (accepted < 0 || observed_length != client_length ||
        memcmp(&observed_address, &client_address, client_length) != 0) {
        puts("LINUX_THREAD: AF_UNIX accept peer address failed");
        return 16;
    }
    memset(&observed_address, 0, sizeof(observed_address));
    observed_length = sizeof(observed_address);
    if (getsockname(accepted, &observed_address, &observed_length) < 0 ||
        observed_length != server_length ||
        memcmp(&observed_address, &server_address, server_length) != 0) {
        puts("LINUX_THREAD: AF_UNIX accepted names failed");
        return 17;
    }
    memset(&observed_address, 0, sizeof(observed_address));
    observed_length = sizeof(observed_address);
    if (getpeername(accepted, &observed_address, &observed_length) < 0 ||
        observed_length != client_length ||
        memcmp(&observed_address, &client_address, client_length) != 0) {
        puts("LINUX_THREAD: AF_UNIX accepted peer name failed");
        return 17;
    }
    memset(&observed_address, 0, sizeof(observed_address));
    observed_length = sizeof(observed_address);
    if (syscall(SYS_getpeername, (long)client, (long)&observed_address,
                (long)&observed_length) != 0 ||
        observed_length != server_length ||
        memcmp(&observed_address, &server_address, server_length) != 0) {
        puts("LINUX_THREAD: AF_UNIX client peer name failed");
        return 18;
    }
    close(accepted);
    close(client);
    close(listener);

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
