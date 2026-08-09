/* Native Linux x86-64 inotify ABI smoke test for the HBOS VFS bridge. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#define NR_RENAME             82
#define NR_SYMLINK            88
#define NR_OPENAT            257
#define NR_NEWFSTATAT        262
#define NR_RENAMEAT          264
#define NR_SYMLINKAT         266
#define NR_READLINKAT        267
#define NR_INOTIFY_ADD_WATCH 254
#define NR_INOTIFY_RM_WATCH  255
#define NR_INOTIFY_INIT1     294
#define NR_RENAMEAT2         316
#define IN_MODIFY      0x00000002U
#define IN_MOVED_FROM  0x00000040U
#define IN_MOVED_TO    0x00000080U
#define IN_CREATE      0x00000100U
#define IN_DELETE      0x00000200U
#define IN_DELETE_SELF 0x00000400U
#define IN_MOVE_SELF   0x00000800U
#define IN_IGNORED     0x00008000U
#define IN_ISDIR       0x40000000U
#define IN_NONBLOCK    0x00000800
#define IN_CLOEXEC     0x00080000
#define RENAME_NOREPLACE 1U

struct inotify_event_wire {
    int wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
};

struct linux_stat_wire {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    int64_t st_atime;
    int64_t st_atime_nsec;
    int64_t st_mtime;
    int64_t st_mtime_nsec;
    int64_t st_ctime;
    int64_t st_ctime_nsec;
    int64_t reserved[3];
};

static long raw_syscall1(long number, long argument0) {
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(argument0)
                     : "rcx", "r11", "memory");
    return result;
}

static long raw_syscall2(long number, long argument0, long argument1) {
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(argument0), "S"(argument1)
                     : "rcx", "r11", "memory");
    return result;
}

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

static long raw_syscall4(long number, long argument0, long argument1,
                         long argument2, long argument3) {
    register long r10 __asm__("r10") = argument3;
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(argument0), "S"(argument1),
                       "d"(argument2), "r"(r10)
                     : "rcx", "r11", "memory");
    return result;
}

static long raw_syscall5(long number, long argument0, long argument1,
                         long argument2, long argument3, long argument4) {
    register long r10 __asm__("r10") = argument3;
    register long r8 __asm__("r8") = argument4;
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(argument0), "S"(argument1),
                       "d"(argument2), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return result;
}

static int find_event(const unsigned char *buffer, size_t length, int wd,
                      uint32_t mask, const char *name) {
    size_t offset = 0;
    while (offset + sizeof(struct inotify_event_wire) <= length) {
        const struct inotify_event_wire *event =
            (const struct inotify_event_wire *)(buffer + offset);
        size_t record = sizeof(*event) + event->len;
        if (record < sizeof(*event) || offset + record > length) return 0;
        const char *event_name = event->len ? (const char *)(event + 1) : "";
        if (event->wd == wd && (event->mask & mask) == mask &&
            (!name || strcmp(event_name, name) == 0))
            return 1;
        offset += record;
    }
    return 0;
}

static uint32_t find_event_cookie(const unsigned char *buffer, size_t length,
                                  int wd, uint32_t mask, const char *name) {
    size_t offset = 0;
    while (offset + sizeof(struct inotify_event_wire) <= length) {
        const struct inotify_event_wire *event =
            (const struct inotify_event_wire *)(buffer + offset);
        size_t record = sizeof(*event) + event->len;
        if (record < sizeof(*event) || offset + record > length) return 0;
        const char *event_name = event->len ? (const char *)(event + 1) : "";
        if (event->wd == wd && (event->mask & mask) == mask &&
            (!name || strcmp(event_name, name) == 0))
            return event->cookie;
        offset += record;
    }
    return 0;
}

static void dump_events(const unsigned char *buffer, size_t length) {
    size_t offset = 0;
    printf("inotify bytes=%lu\n", (unsigned long)length);
    while (offset + sizeof(struct inotify_event_wire) <= length) {
        const struct inotify_event_wire *event =
            (const struct inotify_event_wire *)(buffer + offset);
        size_t record = sizeof(*event) + event->len;
        if (record < sizeof(*event) || offset + record > length) break;
        printf("  wd=%d mask=%x len=%u name=%s\n", event->wd,
               event->mask, event->len,
               event->len ? (const char *)(event + 1) : "");
        offset += record;
    }
}

int main(void) {
    static const char directory[] = "/tmp/inotify-smoke";
    static const char file[] = "/tmp/inotify-smoke/probe.txt";
    static const char moved[] = "/tmp/inotify-smoke/moved.txt";
    static const char target[] = "/tmp/inotify-smoke/other.txt";
    static const char link[] = "/tmp/inotify-smoke/probe-link";
    static const char link2[] = "/tmp/inotify-smoke/link2";
    static const char dir_link[] = "/tmp/inotify-smoke/dir-link";
    static const char dir_link_file[] =
        "/tmp/inotify-smoke/dir-link/probe.txt";
    static const char tree[] = "/tmp/inotify-smoke/tree";
    static const char moved_tree[] = "/tmp/inotify-smoke/movedir";
    static const char empty_tree[] = "/tmp/inotify-smoke/empty";
    static const char full_tree[] = "/tmp/inotify-smoke/full";
    static const char tree_child[] = "/tmp/inotify-smoke/tree/x";
    static const char moved_child[] = "/tmp/inotify-smoke/movedir/x";
    static const char final_child[] = "/tmp/inotify-smoke/empty/x";
    unsigned char events[1024];
    (void)unlink(file);
    (void)unlink(moved);
    (void)unlink(target);
    (void)unlink(link);
    (void)unlink(link2);
    (void)unlink(dir_link);
    (void)unlink(tree_child);
    (void)unlink(moved_child);
    (void)unlink(final_child);
    (void)unlink("/tmp/inotify-smoke/empty/after");
    (void)unlink("/tmp/inotify-smoke/empty/after2");
    (void)unlink("/tmp/inotify-smoke/full/y");
    (void)rmdir(tree);
    (void)rmdir(moved_tree);
    (void)rmdir(empty_tree);
    (void)rmdir(full_tree);
    (void)rmdir(directory);
    if (mkdir(directory, 0755) < 0) return 1;

    int directory_fd = open(directory, O_RDONLY | O_DIRECTORY, 0);
    int proc_fd = open("/proc/self", O_RDONLY | O_DIRECTORY, 0);
    char executable[64];
    long executable_length = proc_fd < 0 ? -1 : raw_syscall4(
        NR_READLINKAT, proc_fd, (long)"exe", (long)executable,
        sizeof(executable));
    char translated_executable[64];
    long translated_length = proc_fd < 0 ? -1 : syscall(
        SYS_readlinkat, (long)proc_fd, (long)"exe",
        (long)translated_executable, (long)sizeof(translated_executable));
    if (directory_fd < 0 || proc_fd < 0 || executable_length <= 0 ||
        executable_length >= (long)sizeof(executable) ||
        translated_length != executable_length)
        return 26;
    executable[executable_length] = '\0';
    translated_executable[translated_length] = '\0';
    if (executable[0] != '/' ||
        strcmp(executable, translated_executable) != 0 || close(proc_fd) < 0)
        return 27;

    if (raw_syscall1(NR_INOTIFY_INIT1, -1) != -22) return 2;
    int inotify_fd = (int)raw_syscall1(
        NR_INOTIFY_INIT1, IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd < 0) return 3;
    struct stat inotify_stat;
    if (fstat(inotify_fd, &inotify_stat) < 0 ||
        (inotify_stat.st_mode & S_IFMT) != S_IFREG ||
        lseek(inotify_fd, 0, SEEK_SET) != -1)
        return 18;
    int directory_watch = (int)raw_syscall3(
        NR_INOTIFY_ADD_WATCH, inotify_fd, (long)directory,
        IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    if (directory_watch <= 0) return 4;

    struct pollfd poll_fd = {inotify_fd, POLLIN, 0};
    if (poll(&poll_fd, 1, 0) != 0) return 5;
    int epoll_fd = epoll_create1(0);
    struct epoll_event epoll_watch;
    memset(&epoll_watch, 0, sizeof(epoll_watch));
    epoll_watch.events = EPOLLIN;
    epoll_watch.data.fd = inotify_fd;
    if (epoll_fd < 0 ||
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, inotify_fd, &epoll_watch) < 0)
        return 6;

    int file_fd = (int)raw_syscall4(
        NR_OPENAT, directory_fd, (long)"probe.txt",
        O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (file_fd < 0 || write(file_fd, "hbos", 4) != 4 || close(file_fd) < 0)
        return 7;
    struct linux_stat_wire linux_status;
    struct stat translated_status;
    memset(&linux_status, 0, sizeof(linux_status));
    memset(&translated_status, 0, sizeof(translated_status));
    if (raw_syscall4(NR_NEWFSTATAT, directory_fd, (long)"probe.txt",
                     (long)&linux_status, 0) != 0 ||
        (linux_status.st_mode & S_IFMT) != S_IFREG ||
        linux_status.st_size != 4 ||
        raw_syscall4(NR_NEWFSTATAT, -1, (long)file,
                     (long)&linux_status, 0) != 0 ||
        syscall(SYS_newfstatat, (long)directory_fd, (long)"probe.txt",
                (long)&translated_status, 0L) != 0 ||
        (translated_status.st_mode & S_IFMT) != S_IFREG ||
        translated_status.st_size != 4)
        return 28;

    if (raw_syscall3(NR_SYMLINKAT, (long)"probe.txt", directory_fd,
                     (long)"probe-link") != 0 ||
        syscall(SYS_symlinkat, (long)"probe.txt", (long)directory_fd,
                (long)"link2") != 0 ||
        raw_syscall2(NR_SYMLINK, (long)".", (long)dir_link) != 0)
        return 46;
    char link_target[32];
    char translated_target[32];
    long link_length = raw_syscall4(
        NR_READLINKAT, directory_fd, (long)"probe-link",
        (long)link_target, sizeof(link_target));
    long translated_link_length = syscall(
        SYS_readlinkat, (long)directory_fd, (long)"link2",
        (long)translated_target, (long)sizeof(translated_target));
    if (link_length != 9 || translated_link_length != link_length ||
        memcmp(link_target, "probe.txt", 9) != 0 ||
        memcmp(translated_target, link_target, (size_t)link_length) != 0)
        return 47;

    memset(&linux_status, 0, sizeof(linux_status));
    memset(&translated_status, 0, sizeof(translated_status));
    if (raw_syscall4(NR_NEWFSTATAT, directory_fd, (long)"probe-link",
                     (long)&linux_status, AT_SYMLINK_NOFOLLOW) != 0 ||
        (linux_status.st_mode & S_IFMT) != S_IFLNK ||
        linux_status.st_size != 9 ||
        raw_syscall4(NR_NEWFSTATAT, directory_fd, (long)"probe-link",
                     (long)&linux_status, 0) != 0 ||
        (linux_status.st_mode & S_IFMT) != S_IFREG ||
        linux_status.st_size != 4 ||
        syscall(SYS_newfstatat, (long)directory_fd, (long)"link2",
                (long)&translated_status,
                (long)AT_SYMLINK_NOFOLLOW) != 0 ||
        (translated_status.st_mode & S_IFMT) != S_IFLNK ||
        translated_status.st_size != 9 ||
        raw_syscall4(NR_OPENAT, directory_fd, (long)"probe-link",
                     O_RDONLY | O_NOFOLLOW, 0) != -40)
        return 48;

    int link_fd = (int)raw_syscall4(
        NR_OPENAT, directory_fd, (long)"probe-link", O_RDONLY, 0);
    int dir_link_fd = open(dir_link_file, O_RDONLY, 0);
    char linked_data[4];
    memset(&linux_status, 0, sizeof(linux_status));
    memset(&translated_status, 0, sizeof(translated_status));
    if (link_fd < 0 || dir_link_fd < 0 ||
        read(link_fd, linked_data, sizeof(linked_data)) != 4 ||
        memcmp(linked_data, "hbos", 4) != 0 ||
        raw_syscall4(NR_NEWFSTATAT, link_fd, (long)"",
                     (long)&linux_status, AT_EMPTY_PATH) != 0 ||
        (linux_status.st_mode & S_IFMT) != S_IFREG ||
        linux_status.st_size != 4 ||
        syscall(SYS_newfstatat, (long)link_fd, (long)"",
                (long)&translated_status, (long)AT_EMPTY_PATH) != 0 ||
        (translated_status.st_mode & S_IFMT) != S_IFREG ||
        translated_status.st_size != 4 ||
        raw_syscall4(NR_NEWFSTATAT, link_fd, (long)"",
                     (long)&linux_status, 0) != -2 ||
        raw_syscall4(NR_NEWFSTATAT, link_fd, (long)"",
                     (long)&linux_status, 0x2000) != -22 ||
        close(link_fd) < 0 || close(dir_link_fd) < 0)
        return 49;
    int translated_fd = (int)syscall(
        SYS_openat, (long)directory_fd, (long)"probe.txt",
        (long)O_RDONLY, 0L);
    if (translated_fd < 0 || close(translated_fd) < 0) return 30;
    file_fd = open(file, O_RDONLY, 0);
    if (file_fd < 0 ||
        raw_syscall4(NR_OPENAT, file_fd, (long)"child",
                     O_RDONLY, 0) != -20 || close(file_fd) < 0)
        return 29;
    struct epoll_event ready;
    if (epoll_wait(epoll_fd, &ready, 1, 0) != 1 ||
        !(ready.events & EPOLLIN) || ready.data.fd != inotify_fd)
        return 8;
    ssize_t length = read(inotify_fd, events, sizeof(events));
    if (length <= 0 ||
        !find_event(events, (size_t)length, directory_watch,
                    IN_CREATE, "probe.txt") ||
        !find_event(events, (size_t)length, directory_watch,
                    IN_MODIFY, "probe.txt")) {
        if (length > 0) dump_events(events, (size_t)length);
        return 9;
    }

    int file_watch = (int)raw_syscall3(
        NR_INOTIFY_ADD_WATCH, inotify_fd, (long)file,
        IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF);
    if (file_watch <= 0 || file_watch == directory_watch) return 10;
    file_fd = open(file, O_WRONLY, 0);
    if (file_fd < 0 || write(file_fd, "!", 1) != 1 || close(file_fd) < 0)
        return 11;
    length = read(inotify_fd, events, sizeof(events));
    if (length <= 0 ||
        !find_event(events, (size_t)length, file_watch, IN_MODIFY, NULL))
        return 12;

    if (raw_syscall5(NR_RENAMEAT2, directory_fd, (long)"probe.txt",
                     directory_fd, (long)"moved.txt", 0) != 0)
        return 13;
    length = read(inotify_fd, events, sizeof(events));
    uint32_t from_cookie = find_event_cookie(
        events, (size_t)(length > 0 ? length : 0), directory_watch,
        IN_MOVED_FROM, "probe.txt");
    uint32_t to_cookie = find_event_cookie(
        events, (size_t)(length > 0 ? length : 0), directory_watch,
        IN_MOVED_TO, "moved.txt");
    if (length <= 0 || !from_cookie || from_cookie != to_cookie ||
        !find_event(events, (size_t)length, file_watch, IN_MOVE_SELF, NULL) ||
        access(file, F_OK) == 0 || access(moved, F_OK) < 0) {
        if (length > 0) dump_events(events, (size_t)length);
        return 14;
    }

    file_fd = open(target, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (file_fd < 0 || write(file_fd, "old", 3) != 3 || close(file_fd) < 0)
        return 15;
    while (read(inotify_fd, events, sizeof(events)) > 0) {}
    if (raw_syscall5(NR_RENAMEAT2, directory_fd, (long)"moved.txt",
                     directory_fd, (long)"other.txt",
                     RENAME_NOREPLACE) != -17 ||
        access(moved, F_OK) < 0 || access(target, F_OK) < 0)
        return 16;

    if (raw_syscall2(NR_RENAME, (long)moved, (long)target) != 0)
        return 17;
    length = read(inotify_fd, events, sizeof(events));
    from_cookie = find_event_cookie(
        events, (size_t)(length > 0 ? length : 0), directory_watch,
        IN_MOVED_FROM, "moved.txt");
    to_cookie = find_event_cookie(
        events, (size_t)(length > 0 ? length : 0), directory_watch,
        IN_MOVED_TO, "other.txt");
    if (length <= 0 || !from_cookie || from_cookie != to_cookie ||
        !find_event(events, (size_t)length, file_watch, IN_MOVE_SELF, NULL) ||
        access(moved, F_OK) == 0 || access(target, F_OK) < 0) {
        if (length > 0) dump_events(events, (size_t)length);
        return 18;
    }

    if (raw_syscall4(NR_RENAMEAT, directory_fd, (long)"other.txt",
                     directory_fd, (long)"probe.txt") != 0)
        return 19;
    length = read(inotify_fd, events, sizeof(events));
    from_cookie = find_event_cookie(
        events, (size_t)(length > 0 ? length : 0), directory_watch,
        IN_MOVED_FROM, "other.txt");
    to_cookie = find_event_cookie(
        events, (size_t)(length > 0 ? length : 0), directory_watch,
        IN_MOVED_TO, "probe.txt");
    if (length <= 0 || !from_cookie || from_cookie != to_cookie ||
        !find_event(events, (size_t)length, file_watch, IN_MOVE_SELF, NULL)) {
        if (length > 0) dump_events(events, (size_t)length);
        return 20;
    }

    if (unlink(file) < 0) return 21;
    length = read(inotify_fd, events, sizeof(events));
    if (length <= 0 ||
        !find_event(events, (size_t)length, directory_watch,
                    IN_DELETE, "probe.txt") ||
        !find_event(events, (size_t)length, file_watch,
                    IN_DELETE_SELF, NULL) ||
        !find_event(events, (size_t)length, file_watch, IN_IGNORED, NULL))
        return 22;

    if (unlink(link) < 0 || unlink(link2) < 0 || unlink(dir_link) < 0)
        return 50;

    if (mkdir(tree, 0755) < 0) return 31;
    int tree_fd = open(tree, O_RDONLY | O_DIRECTORY, 0);
    int child_fd = tree_fd < 0 ? -1 : (int)raw_syscall4(
        NR_OPENAT, tree_fd, (long)"x", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (tree_fd < 0 || child_fd < 0 || write(child_fd, "x", 1) != 1 ||
        close(child_fd) < 0)
        return 32;
    while (read(inotify_fd, events, sizeof(events)) > 0) {}
    int tree_watch = (int)raw_syscall3(
        NR_INOTIFY_ADD_WATCH, inotify_fd, (long)tree, IN_MOVE_SELF);
    int child_watch = (int)raw_syscall3(
        NR_INOTIFY_ADD_WATCH, inotify_fd, (long)tree_child, IN_MODIFY);
    if (tree_watch <= 0 || child_watch <= 0) return 33;
    if (chdir(tree) < 0 ||
        raw_syscall5(NR_RENAMEAT2, directory_fd, (long)"tree",
                     directory_fd, (long)"movedir", 0) != 0)
        return 34;
    length = read(inotify_fd, events, sizeof(events));
    from_cookie = find_event_cookie(
        events, (size_t)(length > 0 ? length : 0), directory_watch,
        IN_MOVED_FROM | IN_ISDIR, "tree");
    to_cookie = find_event_cookie(
        events, (size_t)(length > 0 ? length : 0), directory_watch,
        IN_MOVED_TO | IN_ISDIR, "movedir");
    char cwd[128];
    if (length <= 0 || !from_cookie || from_cookie != to_cookie ||
        !find_event(events, (size_t)length, tree_watch,
                    IN_MOVE_SELF | IN_ISDIR, NULL) ||
        !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, moved_tree) != 0 ||
        access(tree, F_OK) == 0 || access(moved_child, F_OK) < 0)
        return 35;

    child_fd = open(moved_child, O_WRONLY, 0);
    if (child_fd < 0 || write(child_fd, "!", 1) != 1 ||
        close(child_fd) < 0)
        return 36;
    length = read(inotify_fd, events, sizeof(events));
    if (length <= 0 ||
        !find_event(events, (size_t)length, child_watch, IN_MODIFY, NULL))
        return 37;
    int after_fd = (int)raw_syscall4(
        NR_OPENAT, tree_fd, (long)"after", O_CREAT | O_WRONLY, 0644);
    if (after_fd < 0 || close(after_fd) < 0 ||
        access("/tmp/inotify-smoke/movedir/after", F_OK) < 0 ||
        raw_syscall5(NR_RENAMEAT2, directory_fd, (long)"movedir",
                     tree_fd, (long)"nested", 0) != -22)
        return 38;
    while (read(inotify_fd, events, sizeof(events)) > 0) {}

    if (chdir("/") < 0 || mkdir(full_tree, 0755) < 0) return 39;
    int full_fd = open("/tmp/inotify-smoke/full/y",
                       O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (full_fd < 0 || close(full_fd) < 0 ||
        raw_syscall5(NR_RENAMEAT2, directory_fd, (long)"movedir",
                     directory_fd, (long)"full", 0) != -39 ||
        access(moved_tree, F_OK) < 0 ||
        unlink("/tmp/inotify-smoke/full/y") < 0 || rmdir(full_tree) < 0 ||
        mkdir(empty_tree, 0755) < 0)
        return 45;
    while (read(inotify_fd, events, sizeof(events)) > 0) {}
    int empty_watch = (int)raw_syscall3(
        NR_INOTIFY_ADD_WATCH, inotify_fd, (long)empty_tree,
        IN_DELETE_SELF);
    if (empty_watch <= 0 ||
        raw_syscall5(NR_RENAMEAT2, directory_fd, (long)"movedir",
                     directory_fd, (long)"empty", RENAME_NOREPLACE) != -17 ||
        raw_syscall5(NR_RENAMEAT2, directory_fd, (long)"movedir",
                     directory_fd, (long)"empty", 0) != 0)
        return 40;
    length = read(inotify_fd, events, sizeof(events));
    from_cookie = find_event_cookie(
        events, (size_t)(length > 0 ? length : 0), directory_watch,
        IN_MOVED_FROM | IN_ISDIR, "movedir");
    to_cookie = find_event_cookie(
        events, (size_t)(length > 0 ? length : 0), directory_watch,
        IN_MOVED_TO | IN_ISDIR, "empty");
    if (length <= 0 || !from_cookie || from_cookie != to_cookie ||
        !find_event(events, (size_t)length, tree_watch,
                    IN_MOVE_SELF | IN_ISDIR, NULL) ||
        !find_event(events, (size_t)length, empty_watch,
                    IN_DELETE_SELF | IN_ISDIR, NULL) ||
        !find_event(events, (size_t)length, empty_watch, IN_IGNORED, NULL) ||
        access(final_child, F_OK) < 0)
        return 41;
    int after2_fd = (int)raw_syscall4(
        NR_OPENAT, tree_fd, (long)"after2", O_CREAT | O_WRONLY, 0644);
    if (after2_fd < 0 || close(after2_fd) < 0 ||
        access("/tmp/inotify-smoke/empty/after2", F_OK) < 0)
        return 42;
    if (raw_syscall2(NR_INOTIFY_RM_WATCH, inotify_fd, child_watch) != 0 ||
        raw_syscall2(NR_INOTIFY_RM_WATCH, inotify_fd, tree_watch) != 0)
        return 43;
    while (read(inotify_fd, events, sizeof(events)) > 0) {}
    if (unlink(final_child) < 0 ||
        unlink("/tmp/inotify-smoke/empty/after") < 0 ||
        unlink("/tmp/inotify-smoke/empty/after2") < 0 ||
        close(tree_fd) < 0 || rmdir(empty_tree) < 0)
        return 44;

    if (raw_syscall2(NR_INOTIFY_RM_WATCH, inotify_fd,
                     directory_watch) != 0)
        return 23;
    length = read(inotify_fd, events, sizeof(events));
    if (length <= 0 ||
        !find_event(events, (size_t)length, directory_watch,
                    IN_IGNORED, NULL))
        return 24;

    close(epoll_fd);
    close(inotify_fd);
    close(directory_fd);
    if (rmdir(directory) < 0) return 25;
    puts("LINUX_INOTIFY: PASS");
    return 0;
}
