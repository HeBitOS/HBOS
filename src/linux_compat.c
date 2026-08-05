/**
 * @file linux_compat.c
 * @brief Small Linux event, local IPC and shared-buffer compatibility layer.
 *
 * eventfd/epoll state lives in bounded static tables.  epoll_wait and poll
 * inspect the native HBOS descriptor table directly and yield only when no
 * descriptor is ready.  The hot path is therefore a linear scan over at most
 * POSIX_MAX_FDS (currently 128), without messages, copies, or a helper process.
 */

#include "linux_compat.h"

#include "core/heap.h"
#include "core/pmm.h"
#include "core/task.h"
#include "core/vmm.h"
#include "errno.h"
#include "fcntl.h"
#include "fd.h"
#include "string.h"
#include "unistd.h"

#define LINUX_EVENT_SLOTS POSIX_MAX_FDS
#define LINUX_EPOLL_SLOTS 8
#define LINUX_EPOLL_WATCHES POSIX_MAX_FDS
#define LINUX_GRND_NONBLOCK 0x0001
#define LINUX_GRND_RANDOM   0x0002
#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
#define LINUX_FUTEX_WAIT_BITSET 9
#define LINUX_FUTEX_WAKE_BITSET 10
#define LINUX_FUTEX_CLOCK_REALTIME 256
#define LINUX_FUTEX_COMMAND_MASK 0x7f
#define LINUX_FUTEX_SLOTS MAX_TASKS
#define LINUX_FUTEX_BITSET_MATCH_ANY UINT32_MAX
#define LINUX_AF_UNIX 1
#define LINUX_SOCK_STREAM 1
#define LINUX_SOCK_TYPE_MASK 0xf
#define LINUX_SOCK_NONBLOCK 0x800
#define LINUX_SOCK_CLOEXEC 0x80000
#define LINUX_MSG_DONTWAIT 0x40
#define UNIX_SOCKET_SLOTS 16
#define UNIX_SOCKET_BUFFER 4096
#define UNIX_SOCKET_BACKLOG 8
#define UNIX_PATH_MAX 108
#define UNIX_RIGHTS_QUEUE 4
#define UNIX_RIGHTS_MAX_FDS 4
#define LINUX_MEMFD_SLOTS 8
#define LINUX_MEMFD_MAX_SIZE (64ULL * 1024ULL * 1024ULL)
#define LINUX_MFD_CLOEXEC 0x0001
#define LINUX_MFD_ALLOW_SEALING 0x0002

typedef struct {
    fd_entry_t *entries;
    uint8_t count;
} unix_rights_message_t;

typedef struct {
    int used;
    uint32_t refs;
    uint64_t value;
    int flags;
} event_slot_t;

typedef struct {
    int used;
    int fd;
    linux_epoll_event_t event;
    /* Ready mask observed at the most recent epoll_wait scan.  Used to
     * detect new edges for EPOLLET watches; level-triggered watches
     * ignore it. */
    uint32_t last_mask;
    /* Set after an EPOLLONESHOT watch fired once; the watch stays
     * registered but dormant until EPOLL_CTL_MOD re-arms it. */
    int oneshot_done;
} epoll_watch_t;

typedef struct {
    int used;
    uint32_t refs;
    epoll_watch_t watches[LINUX_EPOLL_WATCHES];
} epoll_slot_t;

typedef struct {
    int used;
    int woken;
    uint32_t task_id;
    uint32_t bitset;
} futex_waiter_t;

typedef struct {
    uint32_t *address;
    futex_waiter_t waiters[MAX_TASKS];
} futex_slot_t;

typedef struct {
    int used;
    uint32_t refs;
    uint32_t owner_pid;
    int type;
    int flags;
    int peer;
    int listening;
    int read_shutdown;
    int write_shutdown;
    uint8_t address[UNIX_PATH_MAX];
    uint16_t address_length;
    uint8_t receive[UNIX_SOCKET_BUFFER];
    uint32_t read_position;
    uint32_t write_position;
    uint32_t receive_count;
    uint8_t pending[UNIX_SOCKET_BACKLOG];
    uint8_t pending_read;
    uint8_t pending_write;
    uint8_t pending_count;
    uint8_t backlog;
    unix_rights_message_t rights[UNIX_RIGHTS_QUEUE];
    uint8_t rights_read;
    uint8_t rights_write;
    uint8_t rights_count;
} unix_socket_slot_t;

typedef struct {
    int used;
    uint32_t refs;
    uint32_t map_refs;
    unsigned int flags;
    uint64_t size;
    uint32_t page_count;
    uint64_t *pages;
} memfd_slot_t;

static event_slot_t event_slots[LINUX_EVENT_SLOTS];
static epoll_slot_t epoll_slots[LINUX_EPOLL_SLOTS];
static futex_slot_t futex_slots[LINUX_FUTEX_SLOTS];
static unix_socket_slot_t unix_socket_slots[UNIX_SOCKET_SLOTS];
static memfd_slot_t memfd_slots[LINUX_MEMFD_SLOTS];

static int compat_set_errno(int value) {
    errno = value;
    return -1;
}

static task_t *compat_task(void) {
    return task_current();
}

static fd_entry_t *compat_fd(task_t *task, int fd) {
    if (!task || !task->fd_table || fd < 0 || fd >= POSIX_MAX_FDS ||
        !task->fd_table->entries[fd].used)
        return NULL;
    return &task->fd_table->entries[fd];
}

static int compat_fd_alloc(task_t *task, int type, uint32_t slot, int flags) {
    if (!task || !task->fd_table) return compat_set_errno(ESRCH);
    for (int fd = 3; fd < POSIX_MAX_FDS; fd++) {
        if (task->fd_table->entries[fd].used) continue;
        memset(&task->fd_table->entries[fd], 0, sizeof(task->fd_table->entries[fd]));
        task->fd_table->entries[fd].used = true;
        task->fd_table->entries[fd].type = type;
        task->fd_table->entries[fd].compat_id = slot;
        task->fd_table->entries[fd].flags = flags;
        return fd;
    }
    return compat_set_errno(EMFILE);
}

static event_slot_t *event_for_fd(task_t *task, int fd) {
    fd_entry_t *entry = compat_fd(task, fd);
    if (!entry || entry->type != FD_EVENT ||
        entry->compat_id >= LINUX_EVENT_SLOTS)
        return NULL;
    event_slot_t *slot = &event_slots[entry->compat_id];
    return slot->used ? slot : NULL;
}

static epoll_slot_t *epoll_for_fd(task_t *task, int fd) {
    fd_entry_t *entry = compat_fd(task, fd);
    if (!entry || entry->type != FD_EPOLL ||
        entry->compat_id >= LINUX_EPOLL_SLOTS)
        return NULL;
    epoll_slot_t *slot = &epoll_slots[entry->compat_id];
    return slot->used ? slot : NULL;
}

static unix_socket_slot_t *unix_for_fd(task_t *task, int fd) {
    fd_entry_t *entry = compat_fd(task, fd);
    if (!entry || entry->type != FD_UNIX ||
        entry->compat_id >= UNIX_SOCKET_SLOTS)
        return NULL;
    unix_socket_slot_t *slot = &unix_socket_slots[entry->compat_id];
    return slot->used ? slot : NULL;
}

static memfd_slot_t *memfd_for_fd(task_t *task, int fd) {
    fd_entry_t *entry = compat_fd(task, fd);
    if (!entry || entry->type != FD_MEMFD ||
        entry->compat_id >= LINUX_MEMFD_SLOTS)
        return NULL;
    memfd_slot_t *slot = &memfd_slots[entry->compat_id];
    return slot->used ? slot : NULL;
}

static void memfd_maybe_reset(memfd_slot_t *slot) {
    if (!slot || !slot->used || slot->refs || slot->map_refs) return;
    for (uint32_t i = 0; i < slot->page_count; i++) {
        if (slot->pages[i]) pmm_free_page(slot->pages[i]);
    }
    kfree(slot->pages);
    memset(slot, 0, sizeof(*slot));
}

static void unix_slot_reset(unix_socket_slot_t *slot);

static int compat_entry_retain(const fd_entry_t *entry) {
    if (!entry || !entry->used) return compat_set_errno(EBADF);
    if (entry->type == FD_EVENT &&
        entry->compat_id < LINUX_EVENT_SLOTS) {
        event_slot_t *slot = &event_slots[entry->compat_id];
        if (!slot->used || slot->refs == UINT32_MAX)
            return compat_set_errno(EBADF);
        slot->refs++;
        return 0;
    }
    if (entry->type == FD_EPOLL &&
        entry->compat_id < LINUX_EPOLL_SLOTS) {
        epoll_slot_t *slot = &epoll_slots[entry->compat_id];
        if (!slot->used || slot->refs == UINT32_MAX)
            return compat_set_errno(EBADF);
        slot->refs++;
        return 0;
    }
    if (entry->type == FD_UNIX &&
        entry->compat_id < UNIX_SOCKET_SLOTS) {
        unix_socket_slot_t *slot = &unix_socket_slots[entry->compat_id];
        if (!slot->used || slot->refs == UINT32_MAX)
            return compat_set_errno(EBADF);
        slot->refs++;
        return 0;
    }
    if (entry->type == FD_MEMFD &&
        entry->compat_id < LINUX_MEMFD_SLOTS) {
        memfd_slot_t *slot = &memfd_slots[entry->compat_id];
        if (!slot->used || slot->refs == UINT32_MAX)
            return compat_set_errno(EBADF);
        slot->refs++;
        return 0;
    }
    if (entry->type == FD_PIPE && entry->pipe) {
        entry->pipe->ref_count++;
        return 0;
    }
    return compat_set_errno(EOPNOTSUPP);
}

static void compat_entry_release(const fd_entry_t *entry) {
    if (!entry || !entry->used) return;
    if (entry->type == FD_EVENT &&
        entry->compat_id < LINUX_EVENT_SLOTS) {
        event_slot_t *slot = &event_slots[entry->compat_id];
        if (slot->used && slot->refs && --slot->refs == 0)
            memset(slot, 0, sizeof(*slot));
    } else if (entry->type == FD_EPOLL &&
               entry->compat_id < LINUX_EPOLL_SLOTS) {
        epoll_slot_t *slot = &epoll_slots[entry->compat_id];
        if (slot->used && slot->refs && --slot->refs == 0)
            memset(slot, 0, sizeof(*slot));
    } else if (entry->type == FD_UNIX &&
               entry->compat_id < UNIX_SOCKET_SLOTS) {
        unix_socket_slot_t *slot = &unix_socket_slots[entry->compat_id];
        if (slot->used && slot->refs && --slot->refs == 0)
            unix_slot_reset(slot);
    } else if (entry->type == FD_MEMFD &&
               entry->compat_id < LINUX_MEMFD_SLOTS) {
        memfd_slot_t *slot = &memfd_slots[entry->compat_id];
        if (slot->used && slot->refs) slot->refs--;
        memfd_maybe_reset(slot);
    } else if (entry->type == FD_PIPE && entry->pipe) {
        if (--entry->pipe->ref_count <= 0) kfree(entry->pipe);
    }
}

static int unix_slot_index(unix_socket_slot_t *slot) {
    return slot ? (int)(slot - unix_socket_slots) : -1;
}

static unix_socket_slot_t *unix_slot_alloc(int type, int flags) {
    for (int i = 0; i < UNIX_SOCKET_SLOTS; i++) {
        if (unix_socket_slots[i].used) continue;
        memset(&unix_socket_slots[i], 0, sizeof(unix_socket_slots[i]));
        unix_socket_slots[i].used = 1;
        unix_socket_slots[i].type = type;
        unix_socket_slots[i].flags = flags;
        unix_socket_slots[i].owner_pid = task_get_process_id();
        unix_socket_slots[i].peer = -1;
        return &unix_socket_slots[i];
    }
    return NULL;
}

static void unix_slot_reset(unix_socket_slot_t *slot) {
    if (!slot || !slot->used) return;
    int index = unix_slot_index(slot);
    if (slot->peer >= 0 && slot->peer < UNIX_SOCKET_SLOTS) {
        unix_socket_slot_t *peer = &unix_socket_slots[slot->peer];
        if (peer->used && peer->peer == index) peer->peer = -1;
    }
    while (slot->pending_count) {
        int pending = slot->pending[slot->pending_read];
        slot->pending_read =
            (uint8_t)((slot->pending_read + 1) % UNIX_SOCKET_BACKLOG);
        slot->pending_count--;
        if (pending >= 0 && pending < UNIX_SOCKET_SLOTS)
            unix_slot_reset(&unix_socket_slots[pending]);
    }
    while (slot->rights_count) {
        unix_rights_message_t *message =
            &slot->rights[slot->rights_read];
        for (uint8_t i = 0; i < message->count; i++)
            compat_entry_release(&message->entries[i]);
        kfree(message->entries);
        memset(message, 0, sizeof(*message));
        slot->rights_read =
            (uint8_t)((slot->rights_read + 1) % UNIX_RIGHTS_QUEUE);
        slot->rights_count--;
    }
    memset(slot, 0, sizeof(*slot));
}

static int unix_address_copy(uint8_t output[UNIX_PATH_MAX],
                             uint16_t *output_length,
                             const void *opaque_address, size_t length) {
    if (!opaque_address || length <= sizeof(uint16_t))
        return compat_set_errno(EINVAL);
    const uint8_t *address = (const uint8_t *)opaque_address;
    uint16_t family;
    memcpy(&family, address, sizeof(family));
    if (family != LINUX_AF_UNIX) return compat_set_errno(EAFNOSUPPORT);

    size_t path_length = length - sizeof(uint16_t);
    if (path_length > UNIX_PATH_MAX) path_length = UNIX_PATH_MAX;
    if (address[sizeof(uint16_t)] != 0) {
        size_t actual = 0;
        while (actual < path_length &&
               address[sizeof(uint16_t) + actual] != 0)
            actual++;
        path_length = actual;
    }
    if (!path_length) return compat_set_errno(EINVAL);
    memcpy(output, address + sizeof(uint16_t), path_length);
    *output_length = (uint16_t)path_length;
    return 0;
}

static int unix_address_equal(const unix_socket_slot_t *slot,
                              const uint8_t *address, uint16_t length) {
    return slot->address_length == length &&
           memcmp(slot->address, address, length) == 0;
}

static uint32_t fd_ready_mask(task_t *task, int fd) {
    if (fd == 0) return LINUX_POLLIN;
    if (fd == 1 || fd == 2) return LINUX_POLLOUT;

    fd_entry_t *entry = compat_fd(task, fd);
    if (!entry) return LINUX_POLLNVAL;

    if (entry->type == FD_EVENT) {
        event_slot_t *slot = event_for_fd(task, fd);
        if (!slot) return LINUX_POLLERR;
        uint32_t ready = LINUX_POLLOUT;
        if (slot->value) ready |= LINUX_POLLIN;
        return ready;
    }

    if (entry->type == FD_PIPE && entry->pipe) {
        uint32_t ready = 0;
        if (entry->pipe->count || entry->pipe->ref_count < 2)
            ready |= LINUX_POLLIN;
        if (entry->pipe->count < PIPE_BUF_SIZE)
            ready |= LINUX_POLLOUT;
        if (entry->pipe->ref_count < 2) ready |= LINUX_POLLHUP;
        return ready;
    }

    if (entry->type == FD_EPOLL) return LINUX_POLLIN;

    if (entry->type == FD_UNIX) {
        unix_socket_slot_t *slot = unix_for_fd(task, fd);
        if (!slot) return LINUX_POLLERR;
        if (slot->listening)
            return slot->pending_count ? LINUX_POLLIN : 0;
        uint32_t ready = slot->receive_count ? LINUX_POLLIN : 0;
        if (slot->peer < 0 ||
            slot->peer >= UNIX_SOCKET_SLOTS ||
            !unix_socket_slots[slot->peer].used)
            return ready | LINUX_POLLIN | LINUX_POLLHUP;
        if (unix_socket_slots[slot->peer].write_shutdown)
            ready |= LINUX_POLLIN | LINUX_POLLHUP;
        if (unix_socket_slots[slot->peer].receive_count <
            UNIX_SOCKET_BUFFER)
            ready |= LINUX_POLLOUT;
        return ready;
    }

    /*
     * Regular files are always immediately ready.  The current socket
     * backend is polling based and exposes no non-consuming readiness probe,
     * so keep the existing select() contract for sockets until that hook is
     * added: a valid socket may be attempted for either direction.
     */
    return LINUX_POLLIN | LINUX_POLLOUT;
}

static uint64_t timeout_deadline(int timeout_ms) {
    if (timeout_ms < 0) return UINT64_MAX;
    uint64_t ticks = pit_ticks_from_ms((uint32_t)timeout_ms);
    if (timeout_ms > 0 && ticks == 0) ticks = 1;
    return pit_get_ticks() + ticks;
}

static int timeout_expired(int timeout_ms, uint64_t deadline) {
    if (timeout_ms < 0) return 0;
    if (timeout_ms == 0) return 1;
    return (int64_t)(pit_get_ticks() - deadline) >= 0;
}

long linux_compat_read(int fd, void *buffer, size_t count) {
    task_t *task = compat_task();
    event_slot_t *slot = event_for_fd(task, fd);
    if (!slot) {
        if (unix_for_fd(task, fd))
            return linux_compat_unix_recv(fd, buffer, count, 0);
        memfd_slot_t *memfd = memfd_for_fd(task, fd);
        fd_entry_t *entry = compat_fd(task, fd);
        if (memfd && entry) {
            if (!buffer && count) return -EFAULT;
            if (entry->offset > memfd->size) return 0;
            uint64_t available = memfd->size - entry->offset;
            if (count > available) count = (size_t)available;
            size_t copied = 0;
            while (copied < count) {
                uint64_t position = entry->offset + copied;
                uint32_t page = (uint32_t)(position / PAGE_SIZE);
                size_t in_page = (size_t)(position % PAGE_SIZE);
                size_t chunk = PAGE_SIZE - in_page;
                if (chunk > count - copied) chunk = count - copied;
                memcpy((uint8_t *)buffer + copied,
                       (const uint8_t *)(uintptr_t)memfd->pages[page] +
                           in_page,
                       chunk);
                copied += chunk;
            }
            entry->offset += (uint32_t)copied;
            return (long)copied;
        }
        return -EBADF;
    }
    if (!buffer) return -EFAULT;
    if (count < sizeof(uint64_t)) return -EINVAL;

    while (slot->value == 0) {
        if (slot->flags & LINUX_EFD_NONBLOCK) return -EAGAIN;
        task_yield();
    }

    uint64_t value;
    if (slot->flags & LINUX_EFD_SEMAPHORE) {
        value = 1;
        slot->value--;
    } else {
        value = slot->value;
        slot->value = 0;
    }
    memcpy(buffer, &value, sizeof(value));
    return (long)sizeof(value);
}

long linux_compat_write(int fd, const void *buffer, size_t count) {
    task_t *task = compat_task();
    event_slot_t *slot = event_for_fd(task, fd);
    if (!slot) {
        if (unix_for_fd(task, fd))
            return linux_compat_unix_send(fd, buffer, count, 0);
        memfd_slot_t *memfd = memfd_for_fd(task, fd);
        fd_entry_t *entry = compat_fd(task, fd);
        if (memfd && entry) {
            if (!buffer && count) return -EFAULT;
            if (entry->offset >= memfd->size)
                return count ? -ENOSPC : 0;
            uint64_t available = memfd->size - entry->offset;
            if (count > available) count = (size_t)available;
            size_t copied = 0;
            while (copied < count) {
                uint64_t position = entry->offset + copied;
                uint32_t page = (uint32_t)(position / PAGE_SIZE);
                size_t in_page = (size_t)(position % PAGE_SIZE);
                size_t chunk = PAGE_SIZE - in_page;
                if (chunk > count - copied) chunk = count - copied;
                memcpy((uint8_t *)(uintptr_t)memfd->pages[page] + in_page,
                       (const uint8_t *)buffer + copied, chunk);
                copied += chunk;
            }
            entry->offset += (uint32_t)copied;
            return (long)copied;
        }
        return -EBADF;
    }
    if (!buffer) return -EFAULT;
    if (count < sizeof(uint64_t)) return -EINVAL;

    uint64_t value;
    memcpy(&value, buffer, sizeof(value));
    if (value == UINT64_MAX) return -EINVAL;

    while (UINT64_MAX - 1 - slot->value < value) {
        if (slot->flags & LINUX_EFD_NONBLOCK) return -EAGAIN;
        task_yield();
    }
    slot->value += value;
    return (long)sizeof(value);
}

int linux_compat_close(int fd) {
    task_t *task = compat_task();
    fd_entry_t *entry = compat_fd(task, fd);
    if (!entry) return 0;

    if (entry->type == FD_EVENT && entry->compat_id < LINUX_EVENT_SLOTS) {
        event_slot_t *slot = &event_slots[entry->compat_id];
        if (slot->used && slot->refs && --slot->refs == 0)
            memset(slot, 0, sizeof(*slot));
        return 1;
    }
    if (entry->type == FD_EPOLL && entry->compat_id < LINUX_EPOLL_SLOTS) {
        epoll_slot_t *slot = &epoll_slots[entry->compat_id];
        if (slot->used && slot->refs && --slot->refs == 0)
            memset(slot, 0, sizeof(*slot));
        return 1;
    }
    if (entry->type == FD_UNIX && entry->compat_id < UNIX_SOCKET_SLOTS) {
        unix_socket_slot_t *slot = &unix_socket_slots[entry->compat_id];
        if (slot->used && slot->refs && --slot->refs == 0)
            unix_slot_reset(slot);
        return 1;
    }
    if (entry->type == FD_MEMFD && entry->compat_id < LINUX_MEMFD_SLOTS) {
        memfd_slot_t *slot = &memfd_slots[entry->compat_id];
        if (slot->used && slot->refs) slot->refs--;
        memfd_maybe_reset(slot);
        return 1;
    }
    return 0;
}

void linux_compat_retain(int fd) {
    task_t *task = compat_task();
    fd_entry_t *entry = compat_fd(task, fd);
    if (!entry) return;
    if (entry->type == FD_EVENT && entry->compat_id < LINUX_EVENT_SLOTS) {
        event_slot_t *slot = &event_slots[entry->compat_id];
        if (slot->used && slot->refs < UINT32_MAX) slot->refs++;
    } else if (entry->type == FD_EPOLL && entry->compat_id < LINUX_EPOLL_SLOTS) {
        epoll_slot_t *slot = &epoll_slots[entry->compat_id];
        if (slot->used && slot->refs < UINT32_MAX) slot->refs++;
    } else if (entry->type == FD_UNIX &&
               entry->compat_id < UNIX_SOCKET_SLOTS) {
        unix_socket_slot_t *slot = &unix_socket_slots[entry->compat_id];
        if (slot->used && slot->refs < UINT32_MAX) slot->refs++;
    } else if (entry->type == FD_MEMFD &&
               entry->compat_id < LINUX_MEMFD_SLOTS) {
        memfd_slot_t *slot = &memfd_slots[entry->compat_id];
        if (slot->used && slot->refs < UINT32_MAX) slot->refs++;
    }
}

void linux_compat_retain_task(struct task *opaque_task) {
    task_t *task = (task_t *)opaque_task;
    if (!task) return;
    for (int fd = 0; fd < POSIX_MAX_FDS; fd++) {
        fd_entry_t *entry = compat_fd(task, fd);
        if (!entry) continue;
        if (entry->type == FD_EVENT && entry->compat_id < LINUX_EVENT_SLOTS) {
            event_slot_t *slot = &event_slots[entry->compat_id];
            if (slot->used && slot->refs < UINT32_MAX) slot->refs++;
        } else if (entry->type == FD_EPOLL &&
                   entry->compat_id < LINUX_EPOLL_SLOTS) {
            epoll_slot_t *slot = &epoll_slots[entry->compat_id];
            if (slot->used && slot->refs < UINT32_MAX) slot->refs++;
        } else if (entry->type == FD_UNIX &&
                   entry->compat_id < UNIX_SOCKET_SLOTS) {
            unix_socket_slot_t *slot = &unix_socket_slots[entry->compat_id];
            if (slot->used && slot->refs < UINT32_MAX) slot->refs++;
        } else if (entry->type == FD_MEMFD &&
                   entry->compat_id < LINUX_MEMFD_SLOTS) {
            memfd_slot_t *slot = &memfd_slots[entry->compat_id];
            if (slot->used && slot->refs < UINT32_MAX) slot->refs++;
        }
    }
}

void linux_compat_release_task(struct task *opaque_task) {
    task_t *task = (task_t *)opaque_task;
    if (!task) return;
    for (int fd = 0; fd < POSIX_MAX_FDS; fd++) {
        fd_entry_t *entry = compat_fd(task, fd);
        if (!entry) continue;
        if (entry->type == FD_EVENT && entry->compat_id < LINUX_EVENT_SLOTS) {
            event_slot_t *slot = &event_slots[entry->compat_id];
            if (slot->used && slot->refs && --slot->refs == 0)
                memset(slot, 0, sizeof(*slot));
        } else if (entry->type == FD_EPOLL &&
                   entry->compat_id < LINUX_EPOLL_SLOTS) {
            epoll_slot_t *slot = &epoll_slots[entry->compat_id];
            if (slot->used && slot->refs && --slot->refs == 0)
                memset(slot, 0, sizeof(*slot));
        } else if (entry->type == FD_UNIX &&
                   entry->compat_id < UNIX_SOCKET_SLOTS) {
            unix_socket_slot_t *slot = &unix_socket_slots[entry->compat_id];
            if (slot->used && slot->refs && --slot->refs == 0)
                unix_slot_reset(slot);
        } else if (entry->type == FD_MEMFD &&
                   entry->compat_id < LINUX_MEMFD_SLOTS) {
            memfd_slot_t *slot = &memfd_slots[entry->compat_id];
            if (slot->used && slot->refs) slot->refs--;
            memfd_maybe_reset(slot);
        }
    }
}

int linux_compat_poll(linux_pollfd_t *fds, uint32_t count, int timeout_ms) {
    if ((!fds && count) || count > POSIX_MAX_FDS)
        return compat_set_errno(EINVAL);

    task_t *task = compat_task();
    if (!task) return compat_set_errno(ESRCH);
    uint64_t deadline = timeout_deadline(timeout_ms);

    for (;;) {
        int ready_count = 0;
        for (uint32_t i = 0; i < count; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            uint32_t mask = fd_ready_mask(task, fds[i].fd);
            uint32_t requested = (uint16_t)fds[i].events;
            uint32_t result = mask &
                (requested | LINUX_POLLERR | LINUX_POLLHUP | LINUX_POLLNVAL);
            fds[i].revents = (int16_t)result;
            if (result) ready_count++;
        }
        if (ready_count || timeout_expired(timeout_ms, deadline))
            return ready_count;
        task_yield();
    }
}

int linux_compat_pipe2(int pipefd[2], int flags) {
    if (flags & ~(O_NONBLOCK | O_CLOEXEC))
        return compat_set_errno(EINVAL);
    if (pipe(pipefd) < 0) return -1;
    task_t *task = compat_task();
    if (!task) return compat_set_errno(ESRCH);
    task->fd_table->entries[pipefd[0]].flags |= flags;
    task->fd_table->entries[pipefd[1]].flags |= flags;
    return 0;
}

int linux_compat_eventfd2(uint32_t initial_value, int flags) {
    if (flags & ~(LINUX_EFD_SEMAPHORE | LINUX_EFD_NONBLOCK |
                  LINUX_EFD_CLOEXEC))
        return compat_set_errno(EINVAL);

    for (uint32_t i = 0; i < LINUX_EVENT_SLOTS; i++) {
        if (event_slots[i].used) continue;
        event_slots[i].used = 1;
        event_slots[i].refs = 1;
        event_slots[i].value = initial_value;
        event_slots[i].flags = flags;
        int fd = compat_fd_alloc(compat_task(), FD_EVENT, i, flags | O_RDWR);
        if (fd < 0) memset(&event_slots[i], 0, sizeof(event_slots[i]));
        return fd;
    }
    return compat_set_errno(ENFILE);
}

int linux_compat_epoll_create1(int flags) {
    if (flags & ~O_CLOEXEC) return compat_set_errno(EINVAL);
    for (uint32_t i = 0; i < LINUX_EPOLL_SLOTS; i++) {
        if (epoll_slots[i].used) continue;
        memset(&epoll_slots[i], 0, sizeof(epoll_slots[i]));
        epoll_slots[i].used = 1;
        epoll_slots[i].refs = 1;
        int fd = compat_fd_alloc(compat_task(), FD_EPOLL, i, flags | O_RDWR);
        if (fd < 0) memset(&epoll_slots[i], 0, sizeof(epoll_slots[i]));
        return fd;
    }
    return compat_set_errno(ENFILE);
}

int linux_compat_epoll_ctl(int epfd, int operation, int fd,
                           const linux_epoll_event_t *event) {
    task_t *task = compat_task();
    epoll_slot_t *epoll = epoll_for_fd(task, epfd);
    if (!epoll) return compat_set_errno(EBADF);
    if (epfd == fd) return compat_set_errno(EINVAL);
    if (!compat_fd(task, fd) && fd > 2) return compat_set_errno(EBADF);
    if (operation != LINUX_EPOLL_CTL_DEL && !event)
        return compat_set_errno(EFAULT);

    int found = -1;
    int empty = -1;
    for (int i = 0; i < LINUX_EPOLL_WATCHES; i++) {
        if (epoll->watches[i].used && epoll->watches[i].fd == fd)
            found = i;
        if (!epoll->watches[i].used && empty < 0) empty = i;
    }

    if (operation == LINUX_EPOLL_CTL_ADD) {
        if (found >= 0) return compat_set_errno(EEXIST);
        if (empty < 0) return compat_set_errno(ENOSPC);
        epoll->watches[empty].used = 1;
        epoll->watches[empty].fd = fd;
        epoll->watches[empty].event = *event;
        epoll->watches[empty].last_mask = 0;
        epoll->watches[empty].oneshot_done = 0;
        return 0;
    }
    if (operation == LINUX_EPOLL_CTL_MOD) {
        if (found < 0) return compat_set_errno(ENOENT);
        epoll->watches[found].event = *event;
        /* Re-arming via MOD resets the edge tracker and wakes a dormant
         * EPOLLONESHOT watch, matching Linux semantics. */
        epoll->watches[found].last_mask = 0;
        epoll->watches[found].oneshot_done = 0;
        return 0;
    }
    if (operation == LINUX_EPOLL_CTL_DEL) {
        if (found < 0) return compat_set_errno(ENOENT);
        memset(&epoll->watches[found], 0, sizeof(epoll->watches[found]));
        return 0;
    }
    return compat_set_errno(EINVAL);
}

int linux_compat_epoll_wait(int epfd, linux_epoll_event_t *events,
                            int max_events, int timeout_ms) {
    if (!events) return compat_set_errno(EFAULT);
    if (max_events <= 0 || max_events > POSIX_MAX_FDS)
        return compat_set_errno(EINVAL);

    task_t *task = compat_task();
    epoll_slot_t *epoll = epoll_for_fd(task, epfd);
    if (!epoll) return compat_set_errno(EBADF);
    uint64_t deadline = timeout_deadline(timeout_ms);

    for (;;) {
        int count = 0;
        for (int i = 0; i < LINUX_EPOLL_WATCHES && count < max_events; i++) {
            epoll_watch_t *watch = &epoll->watches[i];
            if (!watch->used) continue;
            if (watch->oneshot_done) continue;
            uint32_t mask = fd_ready_mask(task, watch->fd);
            uint32_t result = mask &
                (watch->event.events | LINUX_POLLERR | LINUX_POLLHUP);
            if (watch->event.events & LINUX_EPOLLET) {
                /* Edge-triggered: report only ready bits that appeared
                 * since the last scan.  EPOLLERR/EPOLLHUP are exempt and
                 * always surface, as on Linux. */
                result = (result & ~watch->last_mask) |
                         (result & (LINUX_POLLERR | LINUX_POLLHUP));
            }
            /* Track the observed ready mask for the next edge scan. */
            watch->last_mask = mask;
            if (!result) continue;
            events[count] = watch->event;
            events[count].events = result;
            count++;
            if (watch->event.events & LINUX_EPOLLONESHOT)
                watch->oneshot_done = 1;
        }
        if (count || timeout_expired(timeout_ms, deadline)) return count;
        task_yield();
    }
}

static int cpu_has_rdrand(void) {
    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    (void)ebx;
    (void)edx;
    return (ecx & (1U << 30)) != 0;
}

static int random_u64(uint64_t *value) {
    for (int retry = 0; retry < 10; retry++) {
        unsigned char ok;
        __asm__ volatile("rdrand %0; setc %1"
                         : "=r"(*value), "=qm"(ok));
        if (ok) return 0;
    }
    return -1;
}

long linux_compat_getrandom(void *buffer, size_t count, unsigned int flags) {
    if (!buffer && count) return -EFAULT;
    if (flags & ~(LINUX_GRND_NONBLOCK | LINUX_GRND_RANDOM)) return -EINVAL;
    if (!cpu_has_rdrand()) return -ENOSYS;

    uint8_t *out = (uint8_t *)buffer;
    size_t done = 0;
    while (done < count) {
        uint64_t word;
        if (random_u64(&word) < 0) return done ? (long)done : -EAGAIN;
        size_t take = count - done;
        if (take > sizeof(word)) take = sizeof(word);
        memcpy(out + done, &word, take);
        done += take;
    }
    return (long)done;
}

int linux_compat_unix_socket(int type, int protocol) {
    int base_type = type & LINUX_SOCK_TYPE_MASK;
    int flags = type & (LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC);
    if (protocol != 0) return compat_set_errno(EPROTONOSUPPORT);
    if (base_type != LINUX_SOCK_STREAM)
        return compat_set_errno(ESOCKTNOSUPPORT);
    if (type & ~(LINUX_SOCK_TYPE_MASK | LINUX_SOCK_NONBLOCK |
                 LINUX_SOCK_CLOEXEC))
        return compat_set_errno(EINVAL);

    unix_socket_slot_t *slot = unix_slot_alloc(base_type, flags);
    if (!slot) return compat_set_errno(ENFILE);
    int index = unix_slot_index(slot);
    int fd = compat_fd_alloc(compat_task(), FD_UNIX, (uint32_t)index,
                             O_RDWR | flags);
    if (fd < 0) {
        unix_slot_reset(slot);
        return -1;
    }
    slot->refs = 1;
    return fd;
}

int linux_compat_unix_socketpair(int type, int protocol, int pair[2]) {
    if (!pair) return compat_set_errno(EFAULT);
    pair[0] = -1;
    pair[1] = -1;
    int first = linux_compat_unix_socket(type, protocol);
    if (first < 0) return -1;
    int second = linux_compat_unix_socket(type, protocol);
    if (second < 0) {
        (void)linux_compat_close(first);
        task_t *task = compat_task();
        if (task && task->fd_table)
            memset(&task->fd_table->entries[first], 0,
                   sizeof(task->fd_table->entries[first]));
        return -1;
    }

    task_t *task = compat_task();
    unix_socket_slot_t *a = unix_for_fd(task, first);
    unix_socket_slot_t *b = unix_for_fd(task, second);
    if (!a || !b) return compat_set_errno(EIO);
    a->peer = unix_slot_index(b);
    b->peer = unix_slot_index(a);
    pair[0] = first;
    pair[1] = second;
    return 0;
}

int linux_compat_unix_bind(int fd, const void *address, size_t length) {
    unix_socket_slot_t *slot = unix_for_fd(compat_task(), fd);
    if (!slot) return compat_set_errno(EBADF);
    if (slot->address_length) return compat_set_errno(EINVAL);

    uint8_t path[UNIX_PATH_MAX];
    uint16_t path_length = 0;
    if (unix_address_copy(path, &path_length, address, length) < 0)
        return -1;
    for (int i = 0; i < UNIX_SOCKET_SLOTS; i++) {
        unix_socket_slot_t *other = &unix_socket_slots[i];
        if (!other->used || !other->address_length) continue;
        if (unix_address_equal(other, path, path_length))
            return compat_set_errno(EADDRINUSE);
    }
    memcpy(slot->address, path, path_length);
    slot->address_length = path_length;
    return 0;
}

int linux_compat_unix_listen(int fd, int backlog) {
    unix_socket_slot_t *slot = unix_for_fd(compat_task(), fd);
    if (!slot) return compat_set_errno(EBADF);
    if (!slot->address_length) return compat_set_errno(EDESTADDRREQ);
    if (slot->peer >= 0) return compat_set_errno(EINVAL);
    if (backlog < 0) return compat_set_errno(EINVAL);
    if (backlog == 0) backlog = 1;
    if (backlog > UNIX_SOCKET_BACKLOG) backlog = UNIX_SOCKET_BACKLOG;
    slot->backlog = (uint8_t)backlog;
    slot->listening = 1;
    return 0;
}

int linux_compat_unix_connect(int fd, const void *address, size_t length) {
    unix_socket_slot_t *client = unix_for_fd(compat_task(), fd);
    if (!client) return compat_set_errno(EBADF);
    if (client->peer >= 0) return compat_set_errno(EISCONN);

    uint8_t path[UNIX_PATH_MAX];
    uint16_t path_length = 0;
    if (unix_address_copy(path, &path_length, address, length) < 0)
        return -1;
    unix_socket_slot_t *listener = NULL;
    for (int i = 0; i < UNIX_SOCKET_SLOTS; i++) {
        unix_socket_slot_t *candidate = &unix_socket_slots[i];
        if (!candidate->used || !candidate->listening) continue;
        if (unix_address_equal(candidate, path, path_length)) {
            listener = candidate;
            break;
        }
    }
    if (!listener) return compat_set_errno(ECONNREFUSED);
    if (listener->pending_count >= listener->backlog)
        return compat_set_errno(EAGAIN);

    unix_socket_slot_t *server =
        unix_slot_alloc(LINUX_SOCK_STREAM, listener->flags);
    if (!server) return compat_set_errno(ENFILE);
    int client_index = unix_slot_index(client);
    int server_index = unix_slot_index(server);
    server->owner_pid = listener->owner_pid;
    client->peer = server_index;
    server->peer = client_index;
    listener->pending[listener->pending_write] = (uint8_t)server_index;
    listener->pending_write =
        (uint8_t)((listener->pending_write + 1) % UNIX_SOCKET_BACKLOG);
    listener->pending_count++;
    return 0;
}

int linux_compat_unix_accept(int fd, void *address, uint32_t *length) {
    unix_socket_slot_t *listener = unix_for_fd(compat_task(), fd);
    if (!listener) return compat_set_errno(EBADF);
    if (!listener->listening) return compat_set_errno(EINVAL);

    while (!listener->pending_count) {
        fd_entry_t *entry = compat_fd(compat_task(), fd);
        if ((entry && (entry->flags & O_NONBLOCK)) ||
            (listener->flags & LINUX_SOCK_NONBLOCK))
            return compat_set_errno(EAGAIN);
        task_yield();
        if (!listener->used) return compat_set_errno(EBADF);
    }
    int server_index = listener->pending[listener->pending_read];
    listener->pending_read =
        (uint8_t)((listener->pending_read + 1) % UNIX_SOCKET_BACKLOG);
    listener->pending_count--;
    if (server_index < 0 || server_index >= UNIX_SOCKET_SLOTS ||
        !unix_socket_slots[server_index].used)
        return compat_set_errno(ECONNABORTED);

    int accepted = compat_fd_alloc(
        compat_task(), FD_UNIX, (uint32_t)server_index,
        O_RDWR | (listener->flags &
                  (LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC)));
    if (accepted < 0) {
        unix_slot_reset(&unix_socket_slots[server_index]);
        return -1;
    }
    unix_socket_slots[server_index].refs = 1;
    if (length) {
        if (!address) return accepted;
        uint32_t available = *length;
        uint32_t needed = sizeof(uint16_t) + listener->address_length + 1;
        uint32_t copy = available < needed ? available : needed;
        if (copy >= sizeof(uint16_t)) {
            uint16_t family = LINUX_AF_UNIX;
            memcpy(address, &family, sizeof(family));
            uint32_t path_copy = copy - sizeof(uint16_t);
            if (path_copy > listener->address_length)
                path_copy = listener->address_length;
            memcpy((uint8_t *)address + sizeof(uint16_t),
                   listener->address, path_copy);
        }
        *length = needed;
    }
    return accepted;
}

long linux_compat_unix_send(int fd, const void *buffer, size_t count,
                            int flags) {
    if (!buffer && count) return -EFAULT;
    unix_socket_slot_t *slot = unix_for_fd(compat_task(), fd);
    fd_entry_t *entry = compat_fd(compat_task(), fd);
    if (!slot) return -EBADF;
    if (slot->listening) return -ENOTCONN;
    if (slot->write_shutdown) return -EPIPE;
    if (flags & ~LINUX_MSG_DONTWAIT) return -EOPNOTSUPP;
    if (!count) return 0;

    size_t written = 0;
    while (written < count) {
        if (slot->peer < 0 || slot->peer >= UNIX_SOCKET_SLOTS ||
            !unix_socket_slots[slot->peer].used)
            return written ? (long)written : -EPIPE;
        unix_socket_slot_t *peer = &unix_socket_slots[slot->peer];
        if (peer->read_shutdown)
            return written ? (long)written : -EPIPE;
        if (peer->receive_count == UNIX_SOCKET_BUFFER) {
            if ((entry && (entry->flags & O_NONBLOCK)) ||
                (slot->flags & LINUX_SOCK_NONBLOCK) ||
                (flags & LINUX_MSG_DONTWAIT))
                return written ? (long)written : -EAGAIN;
            task_yield();
            continue;
        }
        peer->receive[peer->write_position] =
            ((const uint8_t *)buffer)[written++];
        peer->write_position =
            (peer->write_position + 1) % UNIX_SOCKET_BUFFER;
        peer->receive_count++;
    }
    return (long)written;
}

long linux_compat_unix_recv(int fd, void *buffer, size_t count, int flags) {
    if (!buffer && count) return -EFAULT;
    unix_socket_slot_t *slot = unix_for_fd(compat_task(), fd);
    fd_entry_t *entry = compat_fd(compat_task(), fd);
    if (!slot) return -EBADF;
    if (slot->listening) return -ENOTCONN;
    if (slot->read_shutdown) return 0;
    if (flags & ~LINUX_MSG_DONTWAIT) return -EOPNOTSUPP;
    if (!count) return 0;

    while (!slot->receive_count) {
        if (slot->peer < 0 || slot->peer >= UNIX_SOCKET_SLOTS ||
            !unix_socket_slots[slot->peer].used)
            return 0;
        if (unix_socket_slots[slot->peer].write_shutdown)
            return 0;
        if ((entry && (entry->flags & O_NONBLOCK)) ||
            (slot->flags & LINUX_SOCK_NONBLOCK) ||
            (flags & LINUX_MSG_DONTWAIT))
            return -EAGAIN;
        task_yield();
    }

    size_t received = 0;
    while (received < count && slot->receive_count) {
        ((uint8_t *)buffer)[received++] =
            slot->receive[slot->read_position];
        slot->read_position =
            (slot->read_position + 1) % UNIX_SOCKET_BUFFER;
        slot->receive_count--;
    }
    return (long)received;
}

int linux_compat_unix_getsockopt(int fd, int level, int option,
                                 void *value, uint32_t *length) {
    enum {
        SOL_SOCKET = 1,
        SO_TYPE = 3,
        SO_ERROR = 4,
        SO_PEERCRED = 17
    };
    unix_socket_slot_t *slot = unix_for_fd(compat_task(), fd);
    if (!slot) return compat_set_errno(EBADF);
    if (!value || !length) return compat_set_errno(EFAULT);
    if (level != SOL_SOCKET) return compat_set_errno(ENOPROTOOPT);

    if (option == SO_TYPE || option == SO_ERROR) {
        if (*length < sizeof(int)) return compat_set_errno(EINVAL);
        *(int *)value = option == SO_TYPE ? slot->type : 0;
        *length = sizeof(int);
        return 0;
    }
    if (option == SO_PEERCRED) {
        struct {
            int pid;
            uint32_t uid;
            uint32_t gid;
        } credentials;
        if (*length < sizeof(credentials)) return compat_set_errno(EINVAL);
        credentials.pid = 0;
        credentials.uid = 0;
        credentials.gid = 0;
        if (slot->peer >= 0 && slot->peer < UNIX_SOCKET_SLOTS &&
            unix_socket_slots[slot->peer].used)
            credentials.pid = (int)unix_socket_slots[slot->peer].owner_pid;
        memcpy(value, &credentials, sizeof(credentials));
        *length = sizeof(credentials);
        return 0;
    }
    return compat_set_errno(ENOPROTOOPT);
}

int linux_compat_unix_setsockopt(int fd, int level, int option,
                                 const void *value, uint32_t length) {
    enum {
        SOL_SOCKET = 1,
        SO_RCVBUF = 8,
        SO_SNDBUF = 7,
        SO_PASSCRED = 16
    };
    unix_socket_slot_t *slot = unix_for_fd(compat_task(), fd);
    if (!slot) return compat_set_errno(EBADF);
    if (level != SOL_SOCKET) return compat_set_errno(ENOPROTOOPT);
    if (option != SO_PASSCRED && option != SO_RCVBUF &&
        option != SO_SNDBUF)
        return compat_set_errno(ENOPROTOOPT);
    if (!value || length < sizeof(int)) return compat_set_errno(EINVAL);
    (void)slot;
    return 0;
}

int linux_compat_unix_shutdown(int fd, int how) {
    unix_socket_slot_t *slot = unix_for_fd(compat_task(), fd);
    if (!slot) return compat_set_errno(EBADF);
    if (how < 0 || how > 2) return compat_set_errno(EINVAL);
    if (how == 0 || how == 2) slot->read_shutdown = 1;
    if (how == 1 || how == 2) slot->write_shutdown = 1;
    return 0;
}

int linux_compat_unix_send_rights(int fd, const int *fds, size_t count) {
    if (!fds || !count || count > UNIX_RIGHTS_MAX_FDS)
        return compat_set_errno(EINVAL);
    task_t *task = compat_task();
    unix_socket_slot_t *sender = unix_for_fd(task, fd);
    if (!sender) return compat_set_errno(EBADF);
    if (sender->peer < 0 || sender->peer >= UNIX_SOCKET_SLOTS ||
        !unix_socket_slots[sender->peer].used)
        return compat_set_errno(ENOTCONN);
    unix_socket_slot_t *receiver = &unix_socket_slots[sender->peer];
    if (receiver->rights_count >= UNIX_RIGHTS_QUEUE)
        return compat_set_errno(EAGAIN);

    fd_entry_t *entries =
        (fd_entry_t *)kcalloc(count, sizeof(fd_entry_t));
    if (!entries) return compat_set_errno(ENOMEM);
    size_t retained = 0;
    for (; retained < count; retained++) {
        fd_entry_t *entry = compat_fd(task, fds[retained]);
        if (!entry || compat_entry_retain(entry) < 0) break;
        entries[retained] = *entry;
    }
    if (retained != count) {
        for (size_t i = 0; i < retained; i++)
            compat_entry_release(&entries[i]);
        kfree(entries);
        return -1;
    }

    unix_rights_message_t *message =
        &receiver->rights[receiver->rights_write];
    message->entries = entries;
    message->count = (uint8_t)count;
    receiver->rights_write =
        (uint8_t)((receiver->rights_write + 1) % UNIX_RIGHTS_QUEUE);
    receiver->rights_count++;
    return 0;
}

int linux_compat_unix_recv_rights(int fd, int *fds, size_t capacity,
                                  size_t *received, int *truncated) {
    if (!received || !truncated || (!fds && capacity))
        return compat_set_errno(EFAULT);
    *received = 0;
    *truncated = 0;
    task_t *task = compat_task();
    unix_socket_slot_t *receiver = unix_for_fd(task, fd);
    if (!receiver) return compat_set_errno(EBADF);
    if (!receiver->rights_count) return 0;

    unix_rights_message_t *message =
        &receiver->rights[receiver->rights_read];
    for (uint8_t i = 0; i < message->count; i++) {
        int new_fd = -1;
        if (*received < capacity) {
            for (int candidate = 3; candidate < POSIX_MAX_FDS; candidate++) {
                if (!task->fd_table->entries[candidate].used) {
                    new_fd = candidate;
                    break;
                }
            }
        }
        if (new_fd < 0) {
            *truncated = 1;
            compat_entry_release(&message->entries[i]);
            continue;
        }
        task->fd_table->entries[new_fd] = message->entries[i];
        task->fd_table->entries[new_fd].used = true;
        fds[(*received)++] = new_fd;
        /* The queue's retained reference is transferred to the new fd. */
    }
    kfree(message->entries);
    memset(message, 0, sizeof(*message));
    receiver->rights_read =
        (uint8_t)((receiver->rights_read + 1) % UNIX_RIGHTS_QUEUE);
    receiver->rights_count--;
    return 0;
}

int linux_compat_memfd_create(const char *name, unsigned int flags) {
    if (!name) return compat_set_errno(EFAULT);
    if (flags & ~(LINUX_MFD_CLOEXEC | LINUX_MFD_ALLOW_SEALING))
        return compat_set_errno(EINVAL);
    /* Linux limits the debug name to 249 bytes.  No pathname is created. */
    size_t length = 0;
    while (name[length] && length <= 249) length++;
    if (length > 249) return compat_set_errno(EINVAL);

    for (uint32_t i = 0; i < LINUX_MEMFD_SLOTS; i++) {
        if (memfd_slots[i].used) continue;
        memset(&memfd_slots[i], 0, sizeof(memfd_slots[i]));
        memfd_slots[i].used = 1;
        memfd_slots[i].refs = 1;
        memfd_slots[i].flags = flags;
        int fd = compat_fd_alloc(
            compat_task(), FD_MEMFD, i,
            O_RDWR | ((flags & LINUX_MFD_CLOEXEC) ? O_CLOEXEC : 0));
        if (fd < 0) memset(&memfd_slots[i], 0, sizeof(memfd_slots[i]));
        return fd;
    }
    return compat_set_errno(ENFILE);
}

int linux_compat_memfd_truncate(int fd, uint64_t size) {
    memfd_slot_t *slot = memfd_for_fd(compat_task(), fd);
    if (!slot) return compat_set_errno(EBADF);
    if (size > LINUX_MEMFD_MAX_SIZE) return compat_set_errno(EFBIG);
    uint32_t wanted =
        (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (wanted == slot->page_count) {
        slot->size = size;
        return 0;
    }
    if (wanted < slot->page_count && slot->map_refs)
        return compat_set_errno(EBUSY);

    uint64_t *new_pages = wanted
        ? (uint64_t *)kcalloc(wanted, sizeof(uint64_t)) : NULL;
    if (wanted && !new_pages) return compat_set_errno(ENOMEM);
    uint32_t keep = wanted < slot->page_count ? wanted : slot->page_count;
    for (uint32_t i = 0; i < keep; i++) new_pages[i] = slot->pages[i];

    uint32_t allocated = keep;
    for (; allocated < wanted; allocated++) {
        uint64_t physical = pmm_alloc_page();
        if (!physical) {
            for (uint32_t j = keep; j < allocated; j++)
                pmm_free_page(new_pages[j]);
            kfree(new_pages);
            return compat_set_errno(ENOMEM);
        }
        new_pages[allocated] = physical;
        memset((void *)(uintptr_t)physical, 0, PAGE_SIZE);
    }
    for (uint32_t i = wanted; i < slot->page_count; i++)
        pmm_free_page(slot->pages[i]);
    kfree(slot->pages);
    slot->pages = new_pages;
    slot->page_count = wanted;
    slot->size = size;
    return 0;
}

int linux_compat_memfd_size(int fd, uint64_t *size) {
    memfd_slot_t *slot = memfd_for_fd(compat_task(), fd);
    if (!slot) return compat_set_errno(EBADF);
    if (!size) return compat_set_errno(EFAULT);
    *size = slot->size;
    return 0;
}

int linux_compat_memfd_map(int fd, uint64_t address, size_t length,
                           uint64_t offset, int writable,
                           uint32_t *backing_id) {
    task_t *task = compat_task();
    memfd_slot_t *slot = memfd_for_fd(task, fd);
    fd_entry_t *entry = compat_fd(task, fd);
    if (!slot || !entry) return compat_set_errno(EBADF);
    if (!address || !length || (address & (PAGE_SIZE - 1)) ||
        (offset & (PAGE_SIZE - 1)))
        return compat_set_errno(EINVAL);
    if (offset > slot->size || length > slot->size - offset)
        return compat_set_errno(EINVAL);

    uint32_t first_page = (uint32_t)(offset / PAGE_SIZE);
    uint32_t page_count =
        (uint32_t)((length + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t page_flags = VMM_P | VMM_U | (writable ? VMM_W : 0);
    uint32_t mapped = 0;
    for (; mapped < page_count; mapped++) {
        if (vmm_map_page(address + (uint64_t)mapped * PAGE_SIZE,
                         slot->pages[first_page + mapped],
                         page_flags) != 0)
            break;
    }
    if (mapped != page_count) {
        while (mapped)
            vmm_unmap_page(address + (uint64_t)--mapped * PAGE_SIZE);
        return compat_set_errno(ENOMEM);
    }
    if (slot->map_refs < UINT32_MAX) slot->map_refs++;
    if (backing_id) *backing_id = entry->compat_id;
    return 0;
}

void linux_compat_memfd_unmap(uint32_t backing_id) {
    if (backing_id >= LINUX_MEMFD_SLOTS) return;
    memfd_slot_t *slot = &memfd_slots[backing_id];
    if (!slot->used) return;
    if (slot->map_refs) slot->map_refs--;
    memfd_maybe_reset(slot);
}

static futex_slot_t *futex_find(uint32_t *address, int create) {
    futex_slot_t *empty = NULL;
    for (int i = 0; i < LINUX_FUTEX_SLOTS; i++) {
        if (futex_slots[i].address == address) return &futex_slots[i];
        if (!futex_slots[i].address && !empty) empty = &futex_slots[i];
    }
    if (create && empty) {
        memset(empty, 0, sizeof(*empty));
        empty->address = address;
        return empty;
    }
    return NULL;
}

int linux_compat_futex6(uint32_t *address, int operation, uint32_t value,
                        const void *opaque_timeout, uint32_t *address2,
                        uint32_t bitset) {
    struct linux_timespec {
        int64_t tv_sec;
        int64_t tv_nsec;
    };
    const struct linux_timespec *timeout =
        (const struct linux_timespec *)opaque_timeout;

    if (!address || ((uintptr_t)address & (sizeof(uint32_t) - 1)))
        return compat_set_errno(EINVAL);

    (void)address2;
    int command = operation & LINUX_FUTEX_COMMAND_MASK;
    int bitset_wait = command == LINUX_FUTEX_WAIT_BITSET;
    int bitset_wake = command == LINUX_FUTEX_WAKE_BITSET;
    if ((bitset_wait || bitset_wake) && bitset == 0)
        return compat_set_errno(EINVAL);
    if (!bitset_wait && !bitset_wake)
        bitset = LINUX_FUTEX_BITSET_MATCH_ANY;

    if (command == LINUX_FUTEX_WAKE || bitset_wake) {
        futex_slot_t *slot = futex_find(address, 0);
        if (!slot || value == 0) return 0;
        uint32_t waking = 0;
        for (unsigned int i = 0; i < MAX_TASKS && waking < value; i++) {
            futex_waiter_t *waiter = &slot->waiters[i];
            if (!waiter->used || waiter->woken ||
                !(waiter->bitset & bitset))
                continue;
            waiter->woken = 1;
            waking++;
        }
        return (int)waking;
    }
    if (command != LINUX_FUTEX_WAIT && !bitset_wait)
        return compat_set_errno(ENOSYS);
    if ((operation & LINUX_FUTEX_CLOCK_REALTIME) && !bitset_wait)
        return compat_set_errno(ENOSYS);
    if (__atomic_load_n(address, __ATOMIC_ACQUIRE) != value)
        return compat_set_errno(EAGAIN);

    int timeout_ms = -1;
    uint64_t absolute_deadline = UINT64_MAX;
    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
            timeout->tv_nsec >= 1000000000LL)
            return compat_set_errno(EINVAL);
        uint64_t millis = (uint64_t)timeout->tv_sec * 1000ULL +
                          ((uint64_t)timeout->tv_nsec + 999999ULL) / 1000000ULL;
        if (bitset_wait) {
            uint32_t frequency = pit_get_frequency_hz();
            absolute_deadline = frequency ?
                ((uint64_t)timeout->tv_sec * frequency +
                 ((uint64_t)timeout->tv_nsec * frequency +
                  999999999ULL) / 1000000000ULL) : 0;
            timeout_ms = 0;
        } else {
            timeout_ms = millis > INT32_MAX ? INT32_MAX : (int)millis;
        }
    }

    futex_slot_t *slot = futex_find(address, 1);
    if (!slot) return compat_set_errno(ENOMEM);
    futex_waiter_t *waiter = NULL;
    for (unsigned int i = 0; i < MAX_TASKS; i++) {
        if (slot->waiters[i].used) continue;
        waiter = &slot->waiters[i];
        memset(waiter, 0, sizeof(*waiter));
        waiter->used = 1;
        waiter->task_id = compat_task() ? compat_task()->id : 0;
        waiter->bitset = bitset;
        break;
    }
    if (!waiter) return compat_set_errno(EAGAIN);
    uint64_t deadline = timeout_deadline(timeout_ms);

    int result = 0;
    for (;;) {
        if (__atomic_load_n(address, __ATOMIC_ACQUIRE) != value) break;
        if (waiter->woken) break;
        int expired = bitset_wait && timeout ?
            (int64_t)(pit_get_ticks() - absolute_deadline) >= 0 :
            timeout_expired(timeout_ms, deadline);
        if (expired) {
            result = compat_set_errno(ETIMEDOUT);
            break;
        }
        task_yield();
    }

    memset(waiter, 0, sizeof(*waiter));
    int any_waiters = 0;
    for (unsigned int i = 0; i < MAX_TASKS; i++)
        if (slot->waiters[i].used) any_waiters = 1;
    if (!any_waiters) {
        slot->address = NULL;
    }
    return result;
}

int linux_compat_futex(uint32_t *address, int operation, uint32_t value,
                       const void *timeout) {
    return linux_compat_futex6(address, operation, value, timeout, NULL,
                               LINUX_FUTEX_BITSET_MATCH_ANY);
}
