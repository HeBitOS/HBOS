/**
 * @file    syscall.c
 * @brief   HBOS 系统调用分发器
 *
 * 接收来自 int 0x80 汇编 stub 的系统调用帧，
 * 根据系统调用号分发到对应的 POSIX 实现函数。
 *
 * 返回值约定:
 *   - 成功: 返回非负值（具体含义因调用而异）
 *   - 失败: 返回负的 errno 值（如 -ENOENT）
 */

#include <stdint.h>

#include "errno.h"
#include "fcntl.h"
#include "sys/stat.h"
#include "sys/wait.h"
#include "sys/dirent.h"
#include "syscall.h"
#include "unistd.h"
#include "core/task.h"
#include "core/cpu.h"
#include "core/vmm.h"
#include "core/pmm.h"
#include "fd.h"
#include "string.h"
#include "acpi.h"
#include "rtc.h"
#include "net.h"
#include "signal.h"
#include "fs.h"
#include "graphics/graphics.h"   /* console_puts/console_putchar：syscall 跟踪日志 */
#include "core/heap.h"
#include "elf.h"
#include "user/ldso.h"
#include "user/hax_app.h"
#include "vfs.h"
#include "version.h"
#include "api/gui_service.h"
#include "tls.h"
#include "https.h"
#include "linux_compat.h"

#define SYSCALL_HTTPS_MAX_SIZE (2u * 1024u * 1024u)

/* 跟踪辅助：定义在 linux_syscall_dispatch_frame 之前，clone/mmap 等更早
 * 的路径也要用，这里前置声明。 */
static void trace_put_udec(uint64_t value);
static void trace_put_hex(uint64_t value);


/**
 * 将 POSIX 函数返回值转换为系统调用返回值
 * POSIX 函数通过 errno 全局变量报告错误，
 * 系统调用通过负返回值报告错误。
 */
static uint64_t finish_syscall(long ret) {
    if (ret < 0 && errno > 0)
        return (uint64_t)(-(int64_t)errno);
    return (uint64_t)ret;
}

static uint64_t vm_protection_flags(int protection) {
    uint64_t flags = VMM_P;
    if (protection != 0) flags |= VMM_U;
    if (protection & 0x02) flags |= VMM_W;  /* PROT_WRITE */
    if (!(protection & 0x04)) flags |= VMM_NX; /* !PROT_EXEC */
    return flags;
}

static int protect_user_range(uint64_t address, size_t length,
                              int protection) {
    if (!length || (address & (PAGE_SIZE - 1)) ||
        (protection & ~0x07) ||
        address >= 0x0000800000000000ULL ||
        length > 0x0000800000000000ULL - address)
        return -EINVAL;
    uint64_t rounded = (uint64_t)length;
    if (rounded > UINT64_MAX - (PAGE_SIZE - 1)) return -EINVAL;
    rounded = (rounded + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t offset = 0; offset < rounded; offset += PAGE_SIZE) {
        if (!vmm_get_phys(address + offset)) return -ENOMEM;
    }
    uint64_t flags = vm_protection_flags(protection);
    for (uint64_t offset = 0; offset < rounded; offset += PAGE_SIZE) {
        if (vmm_protect_page(address + offset, flags) < 0)
            return -ENOMEM;
    }
    return 0;
}

/*
 * Structures crossing the native Linux x86-64 syscall boundary must not be
 * passed straight to HBOS.  Keeping these small adapters here avoids a
 * second VFS/socket implementation while preserving the byte-for-byte ABI
 * expected by unmodified musl/glibc binaries.
 */
typedef struct {
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
} linux_x86_stat_t;

typedef struct {
    void *base;
    uint64_t length;
} linux_x86_iovec_t;

typedef struct {
    void *name;
    uint32_t name_length;
    uint32_t name_padding;
    linux_x86_iovec_t *vectors;
    uint64_t vector_count;
    void *control;
    uint64_t control_length;
    int32_t flags;
    uint32_t flags_padding;
} linux_x86_msghdr_t;

typedef struct {
    uint64_t length;
    int32_t level;
    int32_t type;
} linux_x86_cmsghdr_t;

typedef struct {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
} linux_x86_sigaction_t;

/* Linux x86-64 rt_sigframe ABI (glibc-visible layout).  The ucontext below
 * uses the kernel layout: glibc reads uc_flags..uc_sigmask at the same
 * offsets, treats the 120 pad bytes as the growth area of its own 1024-bit
 * sigset_t, and never looks at the trailing __fpregs_mem pointer unless it
 * restores FP state via setcontext (we do not save FPU state, so it stays
 * NULL and uc_flags stays 0).  Offsets were verified against glibc 2.43
 * x86-64 headers and a live host probe. */
typedef struct {
    uint64_t ss_sp;
    uint32_t ss_flags;
    uint32_t ss_pad;
    uint64_t ss_size;
} linux_x86_stack_t;

typedef struct {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t si_pad0;
    union {
        uint8_t raw[112];
        struct {
            int32_t si_pid;
            int32_t si_uid;
        } kill_field;
        struct {
            void *si_addr;
            int16_t si_addr_lsb;
        } fault_field;
    } si_fields;
} linux_x86_siginfo_t;

typedef struct {
    uint64_t gregs[23];   /* REG_R8 .. REG_CR2, see LINUX_REG_* below */
    void *fpregs;         /* always NULL: FPU state is not saved */
    uint64_t reserved[8];
} linux_x86_mcontext_t;

typedef struct {
    uint64_t uc_flags;
    void *uc_link;
    linux_x86_stack_t uc_stack;
    linux_x86_mcontext_t uc_mcontext;
    uint64_t uc_sigmask;  /* kernel sigset (64 signals); pad follows */
    uint8_t uc_unused[120];
    void *uc_fpregs_mem;  /* kernel layout only, unused */
} linux_x86_ucontext_t;

typedef struct {
    void *pretcode;
    linux_x86_ucontext_t uc;
    linux_x86_siginfo_t info;
} linux_x86_rt_sigframe_t;

typedef struct {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint32_t alignment_pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    uint32_t reserved;
} linux_x86_sysinfo_t;

typedef struct {
    uint64_t current;
    uint64_t maximum;
} linux_x86_rlimit64_t;

typedef struct {
    int64_t seconds;
    int64_t microseconds;
} linux_x86_timeval_t;

typedef struct {
    linux_x86_timeval_t user_time;
    linux_x86_timeval_t system_time;
    int64_t max_resident_set;
    int64_t integral_shared_memory;
    int64_t integral_unshared_data;
    int64_t integral_unshared_stack;
    int64_t minor_faults;
    int64_t major_faults;
    int64_t swaps;
    int64_t block_inputs;
    int64_t block_outputs;
    int64_t messages_sent;
    int64_t messages_received;
    int64_t signals_received;
    int64_t voluntary_switches;
    int64_t involuntary_switches;
} linux_x86_rusage_t;

_Static_assert(sizeof(linux_x86_sysinfo_t) == 112,
               "Linux x86-64 sysinfo ABI size");
_Static_assert(sizeof(linux_x86_rusage_t) == 144,
               "Linux x86-64 rusage ABI size");

static uint64_t linux_sysinfo(linux_x86_sysinfo_t *information) {
    if (!information) return (uint64_t)(-EFAULT);
    memset(information, 0, sizeof(*information));
    uint32_t frequency = pit_get_frequency_hz();
    information->uptime = frequency ?
        (int64_t)(pit_get_ticks() / frequency) : 0;
    information->totalram = pmm_get_total_mem();
    information->freeram = pmm_get_free_mem();
    int processes = task_get_count();
    information->procs = processes > UINT16_MAX ? UINT16_MAX :
                         (uint16_t)processes;
    information->mem_unit = 1;
    return 0;
}

static int linux_user_range_writable(uint64_t address, uint64_t length);
static int linux_user_range_mapped(uint64_t address, uint64_t length);

static uint64_t linux_prlimit64(int pid, unsigned int resource,
                                const linux_x86_rlimit64_t *new_limit,
                                linux_x86_rlimit64_t *old_limit) {
    if (pid != 0 && (uint32_t)pid != task_get_process_id())
        return (uint64_t)(-ESRCH);
    if (resource >= 16) return (uint64_t)(-EINVAL);
    /* 目标缓冲区必须"已映射且可写"。这不只是防御性检查：Chromium 的
     * base::internal::CheckMemoryReadOnly() 故意把一个只读页当输出缓冲区
     * 传进来（写成功就说明这页不是只读的），并且要求返回 -1/EFAULT。
     * 内核若不做页权限检查，prlimit64 会返回 0，它的 CHECK 立刻 int3。
     * 顺带一提：CR0.WP 没开，ring0 真正写只读页不会 #PF，所以这类校验
     * 只能落在系统调用入口上。 */
    if (old_limit &&
        !linux_user_range_writable((uint64_t)(uintptr_t)old_limit,
                                   sizeof(*old_limit)))
        return (uint64_t)(-EFAULT);
    if (new_limit &&
        !linux_user_range_mapped((uint64_t)(uintptr_t)new_limit,
                                 sizeof(*new_limit)))
        return (uint64_t)(-EFAULT);
    const uint64_t infinity = UINT64_MAX;
    linux_x86_rlimit64_t value = {infinity, infinity};
    switch (resource) {
        case 2: /* RLIMIT_DATA */
            value.current = value.maximum = TASK_USER_HEAP_SIZE;
            break;
        case 3: /* RLIMIT_STACK */
            value.current = value.maximum = 1024ULL * 1024ULL;
            break;
        case 6: /* RLIMIT_NPROC */
            value.current = value.maximum = MAX_TASKS;
            break;
        case 7: /* RLIMIT_NOFILE */
            value.current = value.maximum = POSIX_MAX_FDS;
            break;
        default:
            break;
    }
    if (old_limit) *old_limit = value;
    if (new_limit && (new_limit->current != value.current ||
                      new_limit->maximum != value.maximum))
        return (uint64_t)(-EPERM);
    return 0;
}

static uint64_t linux_getrusage(int who, linux_x86_rusage_t *usage) {
    if (!usage) return (uint64_t)(-EFAULT);
    if (who != 0 && who != -1 && who != 1)
        return (uint64_t)(-EINVAL);
    memset(usage, 0, sizeof(*usage));
    /* CPU-time accounting is not yet per-task.  Zero is a valid conservative
     * value; context-switch counters can be added without changing the ABI. */
    return 0;
}

static uint64_t linux_prctl(uint64_t option, uint64_t argument2,
                            uint64_t argument3, uint64_t argument4,
                            uint64_t argument5) {
    (void)argument3;
    (void)argument4;
    (void)argument5;
    task_t *task = task_current();
    if (!task) return (uint64_t)(-ESRCH);
    switch (option) {
        case 1: /* PR_SET_PDEATHSIG */
            if (argument2 >= _NSIG) return (uint64_t)(-EINVAL);
            task->parent_death_signal = (uint8_t)argument2;
            return 0;
        case 2: /* PR_GET_PDEATHSIG */
            if (!argument2) return (uint64_t)(-EFAULT);
            *(int *)(uintptr_t)argument2 = task->parent_death_signal;
            return 0;
        case 3: /* PR_GET_DUMPABLE */
            return task->dumpable ? 1 : 0;
        case 4: /* PR_SET_DUMPABLE */
            if (argument2 > 1) return (uint64_t)(-EINVAL);
            task->dumpable = argument2 != 0;
            return 0;
        case 15: { /* PR_SET_NAME */
            if (!argument2) return (uint64_t)(-EFAULT);
            const char *name = (const char *)(uintptr_t)argument2;
            size_t length = strnlen(name, 15);
            memcpy(task->name, name, length);
            task->name[length] = '\0';
            return 0;
        }
        case 16: /* PR_GET_NAME */
            if (!argument2) return (uint64_t)(-EFAULT);
            memset((void *)(uintptr_t)argument2, 0, 16);
            strncpy((char *)(uintptr_t)argument2, task->name, 15);
            return 0;
        case 21: /* PR_GET_SECCOMP */
            return 0;
        case 22: /* PR_SET_SECCOMP: --no-sandbox 下不装过滤器 */
            return 0;
        case 29: /* PR_SET_TIMERSLACK */
            return 0;
        case 30: /* PR_GET_TIMERSLACK */
            return 50000;                       /* 内核默认 50us */
        case 36: /* PR_SET_CHILD_SUBREAPER */
            task->child_subreaper = argument2 != 0;
            return 0;
        case 37: /* PR_GET_CHILD_SUBREAPER */
            if (!argument2) return (uint64_t)(-EFAULT);
            *(int *)(uintptr_t)argument2 = task->child_subreaper ? 1 : 0;
            return 0;
        case 41: /* PR_SET_THP_DISABLE */
        case 42: /* PR_GET_THP_DISABLE */
            return 0;
        case 23: /* PR_CAPBSET_READ: no ambient capabilities */
            return 0;
        case 38: /* PR_SET_NO_NEW_PRIVS */
            if (argument2 != 1 || argument3 || argument4 || argument5)
                return (uint64_t)(-EINVAL);
            task->no_new_privs = true;
            return 0;
        case 39: /* PR_GET_NO_NEW_PRIVS */
            if (argument2 || argument3 || argument4 || argument5)
                return (uint64_t)(-EINVAL);
            return task->no_new_privs ? 1 : 0;
        default:
            return (uint64_t)(-EINVAL);
    }
}

static uint64_t linux_tgkill(int tgid, int tid, int signal_number) {
    if (tgid <= 0 || tid <= 0 || signal_number < 0 ||
        signal_number >= _NSIG)
        return (uint64_t)(-EINVAL);
    const task_t *target = task_get_by_id((uint32_t)tid);
    if (!target || target->state == TASK_TERMINATED ||
        target->thread_group_id != (uint32_t)tgid)
        return (uint64_t)(-ESRCH);
    if (signal_number == 0) return 0;
    return task_kill((uint32_t)tid, signal_number) == 0 ? 0 :
           (uint64_t)(-ESRCH);
}

typedef struct {
    int64_t seconds;
    int64_t nanoseconds;
} linux_x86_timespec_t;

static int linux_clock_now(int clock_id, linux_x86_timespec_t *time) {
    static uint64_t realtime_epoch_base;
    static uint64_t realtime_tick_base;
    static bool realtime_initialized;
    if (!time) return -EFAULT;
    uint32_t frequency = pit_get_frequency_hz();
    uint64_t ticks = pit_get_ticks();
    uint64_t uptime_seconds = frequency ? ticks / frequency : 0;
    uint64_t remainder = frequency ? ticks % frequency : 0;
    uint64_t nanoseconds = frequency ?
        remainder * 1000000000ULL / frequency : 0;
    switch (clock_id) {
        case 0: /* CLOCK_REALTIME */
        case 5: /* CLOCK_REALTIME_COARSE */
            if (!realtime_initialized) {
                realtime_epoch_base = rtc_timestamp();
                realtime_tick_base = ticks;
                realtime_initialized = true;
            }
            if (frequency) {
                uint64_t elapsed = ticks - realtime_tick_base;
                time->seconds = (int64_t)(realtime_epoch_base +
                                           elapsed / frequency);
                time->nanoseconds = (clock_id == 5) ? 0 :
                    (int64_t)((elapsed % frequency) * 1000000000ULL /
                              frequency);
            } else {
                time->seconds = (int64_t)realtime_epoch_base;
                time->nanoseconds = 0;
            }
            return 0;
        case 1: /* CLOCK_MONOTONIC */
        case 2: /* CLOCK_PROCESS_CPUTIME_ID: conservative uptime baseline */
        case 3: /* CLOCK_THREAD_CPUTIME_ID */
        case 4: /* CLOCK_MONOTONIC_RAW */
        case 6: /* CLOCK_MONOTONIC_COARSE */
        case 7: /* CLOCK_BOOTTIME */
            time->seconds = (int64_t)uptime_seconds;
            time->nanoseconds = (clock_id == 6) ? 0 : (int64_t)nanoseconds;
            return 0;
        default:
            return -EINVAL;
    }
}

static uint64_t linux_clock_gettime(int clock_id,
                                    linux_x86_timespec_t *time) {
    int result = linux_clock_now(clock_id, time);
    return result < 0 ? (uint64_t)(int64_t)result : 0;
}

static uint64_t linux_clock_nanosleep(int clock_id, int flags,
                                      const void *request,
                                      void *remaining) {
    const linux_x86_timespec_t *duration =
        (const linux_x86_timespec_t *)request;
    linux_x86_timespec_t *left = (linux_x86_timespec_t *)remaining;
    if (!duration) return (uint64_t)(-EFAULT);
    if ((clock_id != 0 && clock_id != 1) || (flags & ~1) ||
        duration->seconds < 0 || duration->nanoseconds < 0 ||
        duration->nanoseconds >= 1000000000LL)
        return (uint64_t)(-EINVAL);

    uint64_t milliseconds;
    if (flags & 1) {
        linux_x86_timespec_t now;
        int result = linux_clock_now(clock_id, &now);
        if (result < 0) return (uint64_t)(int64_t)result;
        if (duration->seconds < now.seconds ||
            (duration->seconds == now.seconds &&
             duration->nanoseconds <= now.nanoseconds)) {
            milliseconds = 0;
        } else {
            uint64_t seconds = (uint64_t)(duration->seconds - now.seconds);
            int64_t nanos = duration->nanoseconds - now.nanoseconds;
            if (nanos < 0) {
                seconds--;
                nanos += 1000000000LL;
            }
            if (seconds > UINT64_MAX / 1000ULL)
                return (uint64_t)(-EINVAL);
            milliseconds = seconds * 1000ULL +
                           ((uint64_t)nanos + 999999ULL) / 1000000ULL;
        }
    } else {
        if ((uint64_t)duration->seconds > UINT64_MAX / 1000ULL)
            return (uint64_t)(-EINVAL);
        milliseconds = (uint64_t)duration->seconds * 1000ULL +
                       ((uint64_t)duration->nanoseconds + 999999ULL) /
                       1000000ULL;
    }
    while (milliseconds >= 1000) {
        sleep(1);
        milliseconds -= 1000;
    }
    if (milliseconds) usleep((useconds_t)(milliseconds * 1000ULL));
    if (left) memset(left, 0, sizeof(*left));
    return 0;
}

/* 判断一段用户虚拟地址是否还能分配。
 *
 * 只查 vmm_get_phys() 是不够的：Chromium 的 PartitionAlloc 启动时先
 * mmap(NULL, 32 GiB, PROT_NONE) 预留地址池，而我们为了不占 32 GiB 物理
 * 内存，对 PROT_NONE 只登记 VMA、不建页表——于是池子里每一页
 * vmm_get_phys() 都是 0，"看起来全空"。接下来的普通 mmap 从
 * 0x100000000000 往上扫，第一步就落进池子内部，随后 PA 用
 * mprotect(PROT_READ|WRITE) 提交池内页时，同一段 VA 上就有了两份互不
 * 知情的使用者：堆元数据互相踩 → partition_alloc 报
 * DoubleFreeOrCorruptionDetected / OnNoMemory。
 *
 * 正确做法是把已有 VMA 的覆盖范围也算作"已占用"。 */
static int linux_va_range_free(task_t *owner, uint64_t address, size_t pages) {
    if (!pages || address >= 0x0000800000000000ULL) return 0;
    uint64_t span = (uint64_t)pages * PAGE_SIZE;
    if (span > 0x0000800000000000ULL - address) return 0;
    uint64_t end = address + span;
    if (owner && owner->mm) {
        for (vm_area_t *area = owner->mm->areas; area; area = area->next) {
            if (address < area->end && end > area->start) return 0;
        }
    }
    for (uint64_t va = address; va < end; va += PAGE_SIZE)
        if (vmm_get_phys(va)) return 0;
    return 1;
}

/* 在 [0x100000000000, limit) 里找一段没有任何 VMA 覆盖的空洞。
 *
 * 不能逐页扫：PartitionAlloc 那个 32 GiB 预留区就有 840 万页，TCG 下每
 * 次 mmap 扫一遍等于卡死。这里改成"按 VMA 跳跃"：撞到哪个区就跳到它的
 * 末尾，只有确认无 VMA 覆盖时才逐页核对残留页表项。 */
static uint64_t linux_va_find_free(task_t *owner, size_t pages,
                                   uint64_t limit) {
    uint64_t span = (uint64_t)pages * PAGE_SIZE;
    uint64_t candidate = 0x0000100000000000ULL;
    for (int hops = 0; hops < 65536; hops++) {
        if (candidate >= limit || span >= limit - candidate) return 0;
        uint64_t skip_to = 0;
        if (owner && owner->mm) {
            for (vm_area_t *area = owner->mm->areas; area; area = area->next) {
                if (area->start < candidate + span && area->end > candidate) {
                    if (area->end > skip_to) skip_to = area->end;
                }
            }
        }
        if (skip_to) {
            candidate = (skip_to + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
            continue;
        }
        uint64_t mapped = 0;
        for (uint64_t va = candidate; va < candidate + span; va += PAGE_SIZE) {
            if (vmm_get_phys(va)) { mapped = va; break; }
        }
        if (!mapped) return candidate;
        candidate = mapped + PAGE_SIZE;
    }
    return 0;
}

static uint64_t linux_madvise(uint64_t address, uint64_t length, int advice) {
    if (address & (PAGE_SIZE - 1)) return (uint64_t)(-EINVAL);
    if (!length) return 0;
    if (address >= 0x0000800000000000ULL ||
        length > 0x0000800000000000ULL - address ||
        length > UINT64_MAX - (PAGE_SIZE - 1))
        return (uint64_t)(-EINVAL);
    uint64_t rounded = (length + PAGE_SIZE - 1) &
                       ~(uint64_t)(PAGE_SIZE - 1);
    switch (advice) {
        case 0: case 1: case 2: case 3: case 14: case 15:
        case 16: case 17: case 20: case 21: case 22: case 23:
            break;
        case 4:  /* MADV_DONTNEED */
        case 8:  /* MADV_FREE */
            break;
        default:
            return (uint64_t)(-EINVAL);
    }
    /* 不能因为区间里有"没提交的页"就返回 ENOMEM：Linux 的 madvise 允许
     * 区间里有洞(未映射页)，只是忽略它们。PartitionAlloc 的
     * DecommitSystemPages 会对"预留但未提交"的区间调 madvise(DONTNEED)，
     * 返回非 0 直接触发它的 CHECK → int3。 */
    if (advice == 4 || advice == 8) {
        task_t *current = task_current();
        for (uint64_t offset = 0; offset < rounded; offset += PAGE_SIZE) {
            uint64_t virtual_page = address + offset;
            bool file_snapshot = false;
            for (vm_area_t *area = current && current->mm ?
                     current->mm->areas : NULL;
                 area; area = area->next) {
                if (virtual_page >= area->start && virtual_page < area->end) {
                    file_snapshot =
                        area->backing_type == VM_BACKING_VFS_PRIVATE ||
                        area->backing_type == VM_BACKING_VFS_SHARED;
                    break;
                }
            }
            /* Without demand paging there is no safe way to discard a file
             * snapshot and fault it back in. MADV_* is advisory, so retaining
             * those bytes is preferable to silently replacing executable or
             * library contents with zeroes. */
            if (!file_snapshot &&
                (vmm_get_page_flags(virtual_page) & VMM_OWNED)) {
                int private_result = vmm_make_page_private(virtual_page);
                if (private_result < 0) return -ENOMEM;
                uint64_t physical = vmm_get_phys(virtual_page) &
                                    ~(uint64_t)(PAGE_SIZE - 1);
                memset((void *)(uintptr_t)physical, 0, PAGE_SIZE);
            }
        }
    }
    return 0;
}

typedef struct {
    hbos_syscall_frame_t call;
    uint64_t rcx;
    uint64_t r11;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} linux_x86_syscall_return_frame_t;

#define LINUX_SA_RESTORER  0x04000000ULL
#define LINUX_SA_SIGINFO   0x00000004ULL
#define LINUX_SA_ONSTACK   0x08000000ULL
#define LINUX_SA_NODEFER   0x40000000ULL
#define LINUX_SA_RESETHAND 0x80000000ULL

#define LINUX_SS_ONSTACK   1U
#define LINUX_SS_DISABLE   2U
#define LINUX_MINSIGSTKSZ  2048U

#define LINUX_SI_USER      0
#define LINUX_SEGV_MAPERR  1
#define LINUX_SEGV_ACCERR  2

/* mcontext.gregs[] indices, matching glibc's REG_* enum. */
#define LINUX_REG_R8       0
#define LINUX_REG_R9       1
#define LINUX_REG_R10      2
#define LINUX_REG_R11      3
#define LINUX_REG_R12      4
#define LINUX_REG_R13      5
#define LINUX_REG_R14      6
#define LINUX_REG_R15      7
#define LINUX_REG_RDI      8
#define LINUX_REG_RSI      9
#define LINUX_REG_RBP      10
#define LINUX_REG_RBX      11
#define LINUX_REG_RDX      12
#define LINUX_REG_RAX      13
#define LINUX_REG_RCX      14
#define LINUX_REG_RSP      15
#define LINUX_REG_RIP      16
#define LINUX_REG_EFL      17
#define LINUX_REG_CSGSFS   18
#define LINUX_REG_ERR      19
#define LINUX_REG_TRAPNO   20
#define LINUX_REG_OLDMASK  21
#define LINUX_REG_CR2      22

/* rt_sigframe footprint: pretcode + ucontext + siginfo.  The delivery rsp
 * is computed so the handler is entered ABI-aligned (rsp % 16 == 8), like
 * the legacy single-argument path. */
#define LINUX_RT_FRAME_SIZE \
    (sizeof(linux_x86_rt_sigframe_t))
#define LINUX_SIGINFO_NEST_MAX 8

static int linux_user_range_mapped(uint64_t address, uint64_t length) {
    if (!address || !length || address >= 0x0000800000000000ULL ||
        length > 0x0000800000000000ULL - address)
        return 0;
    uint64_t end = address + length - 1;
    for (uint64_t page = address & ~0xfffULL;; page += 0x1000ULL) {
        if (!vmm_get_phys(page)) return 0;
        if (page >= (end & ~0xfffULL)) break;
    }
    return 1;
}

static int linux_user_range_writable(uint64_t address, uint64_t length) {
    if (!linux_user_range_mapped(address, length)) return 0;
    uint64_t end = address + length - 1;
    for (uint64_t page = address & ~0xfffULL;; page += 0x1000ULL) {
        uint64_t flags = vmm_get_page_flags(page);
        if ((flags & (VMM_U | VMM_W)) != (VMM_U | VMM_W)) return 0;
        if (page >= (end & ~0xfffULL)) break;
    }
    return 1;
}

typedef struct {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
    uint64_t set_tid;
    uint64_t set_tid_size;
    uint64_t cgroup;
} linux_x86_clone_args_t;

#define LINUX_CLONE_ARGS_SIZE_VER0 64U
#define LINUX_CLONE_ARGS_SIZE_VER2 88U

static void linux_clone_context_from_frame(
    const linux_x86_syscall_return_frame_t *frame, uint64_t stack,
    hbos_linux_clone_context_t *context) {
    memset(context, 0, sizeof(*context));
    context->rip = frame->rip;
    context->rsp = stack;
    context->rflags = frame->rflags;
    context->rdi = frame->call.a0;
    context->rsi = frame->call.a1;
    context->rdx = frame->call.a2;
    context->r10 = frame->call.a3;
    context->r8 = frame->call.a4;
    context->r9 = frame->call.a5;
    context->rbx = frame->rbx;
    context->rbp = frame->rbp;
    context->r12 = frame->r12;
    context->r13 = frame->r13;
    context->r14 = frame->r14;
    context->r15 = frame->r15;
}

/* clone(clone_args 版) 与 nr=56 clone 共用实现，见函数体内注释。 */
static uint64_t linux_clone_args(linux_x86_syscall_return_frame_t *frame,
                                 uint64_t flags, uint64_t stack,
                                 uint64_t parent_tid, uint64_t child_tid,
                                 uint64_t tls);

static uint64_t linux_clone(linux_x86_syscall_return_frame_t *frame) {
    if (!frame) return (uint64_t)(-EFAULT);
    return linux_clone_args(frame, frame->call.a0, frame->call.a1,
                            frame->call.a2, frame->call.a3, frame->call.a4);
}

static uint64_t linux_fork(linux_x86_syscall_return_frame_t *frame) {
    if (!frame || !linux_user_range_mapped(frame->rip, 1) ||
        !linux_user_range_mapped(frame->rsp, 1))
        return (uint64_t)(-EFAULT);
    hbos_linux_clone_context_t context;
    linux_clone_context_from_frame(frame, frame->rsp, &context);
    int pid = task_fork_linux(&context);
    return pid < 0 ? (uint64_t)(-EAGAIN) : (uint64_t)pid;
}

/* clone/clone3 共用实现。参数显式传入，**不要**写回 frame 的 a0..a4：
 * 子线程恢复时那些槽位会被当成父线程的 rdi/rsi/rdx/r10/r8 还原，glibc 的
 * pthread 包装器把线程入口函数放在这些寄存器里，一冲就跳到野地址。 */
static uint64_t linux_clone_args(linux_x86_syscall_return_frame_t *frame,
                                 uint64_t flags, uint64_t stack,
                                 uint64_t parent_tid, uint64_t child_tid,
                                 uint64_t tls) {
    enum {
        CLONE_VM = 0x00000100,
        CLONE_FS = 0x00000200,
        CLONE_FILES = 0x00000400,
        CLONE_SIGHAND = 0x00000800,
        CLONE_THREAD = 0x00010000,
        CLONE_SYSVSEM = 0x00040000,
        CLONE_SETTLS = 0x00080000,
        CLONE_PARENT_SETTID = 0x00100000,
        CLONE_CHILD_CLEARTID = 0x00200000,
        CLONE_DETACHED = 0x00400000,
        CLONE_CHILD_SETTID = 0x01000000
    };
    const uint64_t required = CLONE_VM | CLONE_FILES |
                              CLONE_SIGHAND | CLONE_THREAD;
    const uint64_t supported = required | CLONE_FS | CLONE_SYSVSEM |
                               CLONE_SETTLS | CLONE_PARENT_SETTID |
                               CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID |
                               CLONE_DETACHED;
    if (!frame) return (uint64_t)(-EFAULT);
    /* 不带 CLONE_VM 的 clone 是**进程 fork**，不是线程（glibc 的
     * fork() 就是用 clone(SIGCHLD|CLONE_CHILD_SETTID|CLEARTID) 实现的）。
     * 原来把 CLONE_VM|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD 全列为必需位，
     * 于是一切 fork 都返回 EINVAL —— crashpad 起子进程第一步就死在这里。
     * posix_spawn 走的是 clone(CLONE_VM|CLONE_VFORK|SIGCHLD)，也没有
     * CLONE_THREAD；同样按 fork 处理（子进程马上 exec，共享地址空间只是
     * vfork 的性能优化，复制一份语义等价）。 */
    if ((flags & CLONE_THREAD) == 0) {
        hbos_linux_clone_context_t context;
        linux_clone_context_from_frame(frame, frame->rsp, &context);
        int forked = task_fork_linux(&context);
        return forked < 0 ? (uint64_t)(-EAGAIN) : (uint64_t)forked;
    }
    if ((flags & required) != required || (flags & ~supported) != 0)
        return (uint64_t)(-EINVAL);
    /* pthread_create 传的是新线程栈的**最高地址**（栈向下生长），
     * 这个地址本身通常在映射之外——第一个 push 才落到映射内。原来直接
     * 校验 stack 是否已映射，于是所有 pthread_create 都返回 EFAULT
     * （Chromium 报 "pthread_create: Bad address (14)"）。校验栈顶下方
     * 一个字即可。 */
    if (!linux_user_range_mapped(frame->rip, 1) ||
        stack < 8 || !linux_user_range_mapped(stack - 8, 8))
        return (uint64_t)(-EFAULT);
    /* 三处参数地址校验分开打日志：pthread_create 报 EFAULT(14) 时，
     * 不打出来根本不知道是哪个指针被拒。 */
    if ((flags & CLONE_SETTLS) &&
        !linux_user_range_mapped(tls, 1)) {
        console_puts("[CLONE] EFAULT tls=0x");
        trace_put_hex(tls);
        console_puts(" flags=0x");
        trace_put_hex(flags);
        console_putchar('\n');
        return (uint64_t)(-EFAULT);
    }
    if ((flags & CLONE_PARENT_SETTID) &&
        !linux_user_range_mapped(parent_tid, sizeof(uint32_t))) {
        console_puts("[CLONE] EFAULT parent_tid=0x");
        trace_put_hex(parent_tid);
        console_putchar('\n');
        return (uint64_t)(-EFAULT);
    }
    if ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) &&
        !linux_user_range_mapped(child_tid, sizeof(uint32_t))) {
        console_puts("[CLONE] EFAULT child_tid=0x");
        trace_put_hex(child_tid);
        console_puts(" flags=0x");
        trace_put_hex(flags);
        console_putchar('\n');
        return (uint64_t)(-EFAULT);
    }

    hbos_clone_request_t request = {
        .version = HBOS_CLONE_REQUEST_VERSION,
        .size = sizeof(request),
        /* CLONE_DETACHED is a historical no-op in Linux and musl still
         * supplies it.  Validate it above, then omit it from HBOS's compact
         * internal clone contract. */
        .flags = flags & ~(uint64_t)CLONE_DETACHED,
        .entry = frame->rip,
        .stack = stack,
        .argument = 0,
        .tls = tls,
        .parent_tid = parent_tid,
        .child_tid = child_tid,
        .clear_child_tid = child_tid
    };
    hbos_linux_clone_context_t context;
    linux_clone_context_from_frame(frame, stack, &context);
    int tid = task_clone_linux_thread(&request, &context);
    return tid < 0 ? (uint64_t)(-EAGAIN) : (uint64_t)tid;
}

static uint64_t linux_clone3(linux_x86_syscall_return_frame_t *frame,
                             const void *arguments, uint64_t size) {
    enum {
        CLONE_VM = 0x00000100,
        CLONE_FS = 0x00000200,
        CLONE_FILES = 0x00000400,
        CLONE_SIGHAND = 0x00000800,
        CLONE_THREAD = 0x00010000,
        CLONE_SYSVSEM = 0x00040000,
        CLONE_SETTLS = 0x00080000,
        CLONE_PARENT_SETTID = 0x00100000,
        CLONE_CHILD_CLEARTID = 0x00200000,
        CLONE_CHILD_SETTID = 0x01000000
    };
    const uint64_t required = CLONE_VM | CLONE_FILES |
                              CLONE_SIGHAND | CLONE_THREAD;
    const uint64_t supported = required | CLONE_FS | CLONE_SYSVSEM |
                               CLONE_SETTLS | CLONE_PARENT_SETTID |
                               CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID;
    if (!frame || !arguments || size < LINUX_CLONE_ARGS_SIZE_VER0)
        return (uint64_t)(-EINVAL);
    if (size > PAGE_SIZE ||
        !linux_user_range_mapped((uint64_t)(uintptr_t)arguments, size))
        return (uint64_t)(-EFAULT);

    linux_x86_clone_args_t args;
    memset(&args, 0, sizeof(args));
    uint64_t copied = size < sizeof(args) ? size : sizeof(args);
    memcpy(&args, arguments, (size_t)copied);
    if (size > LINUX_CLONE_ARGS_SIZE_VER2) {
        const uint8_t *tail =
            (const uint8_t *)arguments + LINUX_CLONE_ARGS_SIZE_VER2;
        for (uint64_t i = LINUX_CLONE_ARGS_SIZE_VER2; i < size; i++)
            if (tail[i - LINUX_CLONE_ARGS_SIZE_VER2] != 0)
                return (uint64_t)(-E2BIG);
    }
    if ((args.flags & required) != required ||
        (args.flags & ~supported) != 0 || args.exit_signal != 0 ||
        args.set_tid || args.set_tid_size || !args.stack ||
        !args.stack_size || args.stack > UINT64_MAX - args.stack_size)
        return (uint64_t)(-EINVAL);
    /* pidfd and cgroup are interpreted only when CLONE_PIDFD or
     * CLONE_INTO_CGROUP is present.  Both flags are excluded by the
     * supported mask above, but glibc legitimately leaves pidfd pointing at
     * parent_tid without CLONE_PIDFD; Linux ignores that inactive field. */
    /* clone3 与 clone 只是参数打包方式不同（clone_args 结构体 vs 寄存器）。
     * 摊平成寄存器形态后复用 linux_clone()——那条路径已经处理过
     * fork/vfork/线程栈顶这三类形态，没必要再维护第二套校验。 */
    uint64_t stack_top = args.stack + args.stack_size;
    return linux_clone_args(frame,
                            args.flags | (args.exit_signal & 0xffULL),
                            stack_top, args.parent_tid, args.child_tid,
                            args.tls);
}

static uint64_t linux_rt_sigaction(int signal_number,
                                   const linux_x86_sigaction_t *action,
                                   linux_x86_sigaction_t *old_action,
                                   uint64_t signal_set_size) {
    if (signal_set_size != sizeof(uint64_t))
        return (uint64_t)(-EINVAL);
    if (signal_number <= 0 || signal_number >= _NSIG ||
        signal_number == SIGKILL || signal_number == SIGSTOP)
        return (uint64_t)(-EINVAL);
    task_t *task = task_current();
    if (!task) return (uint64_t)(-ESRCH);
    if (old_action) {
        memset(old_action, 0, sizeof(*old_action));
        old_action->handler =
            (uint64_t)(uintptr_t)task->sig_handler[signal_number];
        old_action->flags = task->sig_action_flags[signal_number];
        old_action->restorer = task->sig_action_restorer[signal_number];
        old_action->mask = task->sig_action_mask[signal_number];
    }
    if (action) {
        /* Linux 的 rt_sigaction 从不校验 handler/restorer 地址，只原样存。
         * 我们原来对 >= 0x800000000000 的地址返回 EFAULT，Chromium fork 出的
         * 子进程在 ResetChildSignalHandlersToDefaults() 里吃到 -EFAULT，随即
         * RAW_LOG(FATAL,"sigaction (set) failed.") → int3，表现成"每个子进程
         * 起来就崩"。改成只记录不拒绝；前 8 次打出原始字段，顺便确认 32 字节
         * 布局（handler/flags/restorer/mask）与 glibc 一致。 */
        static int sigaction_logs;
        if (sigaction_logs < 8) {
            sigaction_logs++;
            console_puts("[SIGACT] sig=");
            trace_put_udec((uint64_t)signal_number);
            console_puts(" h=");
            trace_put_hex(action->handler);
            console_puts(" f=");
            trace_put_hex(action->flags);
            console_puts(" r=");
            trace_put_hex(action->restorer);
            console_puts(" m=");
            trace_put_hex(action->mask);
            console_putchar('\n');
        }
        task->sig_handler[signal_number] =
            (void (*)(int))(uintptr_t)action->handler;
        task->sig_action_flags[signal_number] = action->flags;
        task->sig_action_restorer[signal_number] = action->restorer;
        task->sig_action_mask[signal_number] = action->mask &
            ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    }
    return 0;
}

/* Whether rsp currently lies inside the installed alternate signal stack. */
static int linux_on_altstack(const task_t *task, uint64_t rsp) {
    if (task->sig_altstack_flags & LINUX_SS_DISABLE) return 0;
    uint64_t sp = task->sig_altstack_sp;
    uint64_t size = task->sig_altstack_size;
    return size > 0 && rsp >= sp && rsp < sp + size;
}
/* sigaltstack(2): query and/or install the alternate signal stack.  The
 * full syscall frame is needed to answer "currently executing on it?" the
 * same way Linux does -- from the user rsp at syscall entry. */
static uint64_t linux_sigaltstack(const linux_x86_stack_t *user_ss,
                                  linux_x86_stack_t *user_old,
                                  linux_x86_syscall_return_frame_t *frame) {
    task_t *task = task_current();
    if (!task) return (uint64_t)(-ESRCH);

    uint32_t current_flags = 0;
    if (task->sig_altstack_flags & LINUX_SS_DISABLE) {
        current_flags = LINUX_SS_DISABLE;
    } else if (linux_on_altstack(task, frame->rsp)) {
        current_flags = LINUX_SS_ONSTACK;
    }

    if (user_old) {
        if (!linux_user_range_writable((uint64_t)(uintptr_t)user_old,
                                       sizeof(*user_old)))
            return (uint64_t)(-EFAULT);
        user_old->ss_sp = task->sig_altstack_sp;
        user_old->ss_flags = current_flags;
        user_old->ss_pad = 0;
        user_old->ss_size = task->sig_altstack_size;
    }

    if (!user_ss) return 0;
    if (!linux_user_range_mapped((uint64_t)(uintptr_t)user_ss,
                                 sizeof(*user_ss)))
        return (uint64_t)(-EFAULT);

    uint32_t new_flags = user_ss->ss_flags;
    if (new_flags == LINUX_SS_DISABLE) {
        if (current_flags & LINUX_SS_ONSTACK)
            return (uint64_t)(-EPERM);
        task->sig_altstack_sp = 0;
        task->sig_altstack_size = 0;
        task->sig_altstack_flags = LINUX_SS_DISABLE;
        return 0;
    }
    if (new_flags != 0)
        return (uint64_t)(-EINVAL);
    if (user_ss->ss_size < LINUX_MINSIGSTKSZ)
        return (uint64_t)(-ENOMEM);
    if (current_flags & LINUX_SS_ONSTACK)
        return (uint64_t)(-EPERM);
    task->sig_altstack_sp = (uint64_t)(uintptr_t)user_ss->ss_sp;
    task->sig_altstack_size = user_ss->ss_size;
    task->sig_altstack_flags = 0;
    return 0;
}


/* Shared post-frame bookkeeping for both delivery paths: consume the
 * pending bit, apply the handler mask (SA_NODEFER) and SA_RESETHAND. */
static void linux_signal_consume(task_t *task, int signal_number,
                                 uint64_t flags) {
    uint64_t pending_bit = 1ULL << (signal_number - 1);
    task->sig_pending.sig[0] &= ~pending_bit;
    task->sig_blocked.sig[0] |= task->sig_action_mask[signal_number];
    if (!(flags & LINUX_SA_NODEFER))
        task->sig_blocked.sig[0] |= pending_bit;
    task->sig_blocked.sig[0] &=
        ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    if (flags & LINUX_SA_RESETHAND) {
        task->sig_handler[signal_number] = SIG_DFL;
        task->sig_action_flags[signal_number] = 0;
        task->sig_action_restorer[signal_number] = 0;
        task->sig_action_mask[signal_number] = 0;
    }
}

typedef struct {
    uint64_t handler_rsp;
    void *siginfo_address;
    void *ucontext_address;
} linux_siginfo_built_t;

/* Build an SA_SIGINFO rt_sigframe and fill the task signal state.  gregs[]
 * holds the interrupted user context (REG_R8..REG_CR2 slots); si_code/cr2/
 * err_code/trapno describe the delivery cause.  On success the caller must
 * rewrite its own return frame so the handler runs with rdi=signo,
 * rsi=&siginfo, rdx=&ucontext, rip=handler, rsp=handler_rsp. */
static int linux_siginfo_deliver(task_t *task, int signal_number,
                                 uint64_t flags, uint64_t restorer,
                                 uint64_t user_rsp, int si_code,
                                 uint64_t cr2, uint64_t err_code,
                                 uint64_t trapno, const uint64_t gregs[23],
                                 linux_siginfo_built_t *out) {
    uint64_t base_rsp = user_rsp;
    if ((flags & LINUX_SA_ONSTACK) &&
        !(task->sig_altstack_flags & LINUX_SS_DISABLE) &&
        task->sig_altstack_size >= LINUX_MINSIGSTKSZ &&
        !linux_on_altstack(task, user_rsp)) {
        base_rsp = task->sig_altstack_sp + task->sig_altstack_size;
    }

    uint64_t signal_rsp =
        ((base_rsp - 128 - LINUX_RT_FRAME_SIZE) & ~15ULL) + 8;
    if (signal_rsp < 128 ||
        !linux_user_range_writable(signal_rsp, LINUX_RT_FRAME_SIZE))
        return -1;

    linux_x86_rt_sigframe_t *sigframe =
        (linux_x86_rt_sigframe_t *)(uintptr_t)signal_rsp;
    memset(sigframe, 0, sizeof(*sigframe));
    sigframe->pretcode = (void *)(uintptr_t)restorer;
    /* uc_stack mirrors Linux: ss_flags reflects the *interrupted* rsp. */
    sigframe->uc.uc_stack.ss_sp = task->sig_altstack_sp;
    sigframe->uc.uc_stack.ss_flags =
        linux_on_altstack(task, user_rsp) ? LINUX_SS_ONSTACK : 0;
    sigframe->uc.uc_stack.ss_size = task->sig_altstack_size;
    /* gregs is an array parameter: it decays to a pointer, so the size
     * must come from the target member, not sizeof(gregs). */
    memcpy(sigframe->uc.uc_mcontext.gregs, gregs,
           sizeof(sigframe->uc.uc_mcontext.gregs));
    sigframe->uc.uc_mcontext.gregs[LINUX_REG_ERR] = err_code;
    sigframe->uc.uc_mcontext.gregs[LINUX_REG_TRAPNO] = trapno;
    sigframe->uc.uc_mcontext.gregs[LINUX_REG_CR2] = cr2;
    sigframe->uc.uc_mcontext.gregs[LINUX_REG_OLDMASK] =
        task->sig_blocked.sig[0];
    sigframe->uc.uc_mcontext.fpregs = NULL;
    sigframe->uc.uc_sigmask = task->sig_blocked.sig[0];
    sigframe->info.si_signo = signal_number;
    sigframe->info.si_errno = 0;
    sigframe->info.si_code = si_code;
    if (signal_number == SIGSEGV) {
        sigframe->info.si_fields.fault_field.si_addr =
            (void *)(uintptr_t)cr2;
        sigframe->info.si_fields.fault_field.si_addr_lsb = 0;
    } else {
        sigframe->info.si_fields.kill_field.si_pid =
            (int32_t)task->sig_last_sender_pid;
        sigframe->info.si_fields.kill_field.si_uid = 0;
    }

    linux_signal_consume(task, signal_number, flags);
    task->sig_siginfo_depth++;

    out->handler_rsp = signal_rsp;
    out->siginfo_address = &sigframe->info;
    out->ucontext_address = &sigframe->uc;
    return 0;
}

static uint64_t linux_rt_sigreturn(hbos_syscall_frame_t *syscall_frame) {
    task_t *task = task_current();
    if (!task) return (uint64_t)(-ESRCH);
    linux_x86_syscall_return_frame_t *frame =
        (linux_x86_syscall_return_frame_t *)syscall_frame;

    if (task->sig_siginfo_depth > 0) {
        /* SA_SIGINFO frame: the handler's ret popped pretcode, so the
         * ucontext begins at the current user rsp.  Validate the whole
         * region before trusting any register it contains; a forged frame
         * must not be able to inject kernel state. */
        uint64_t ucontext_address = frame->rsp;
        if (ucontext_address < PAGE_SIZE ||
            !linux_user_range_writable(ucontext_address,
                                       sizeof(linux_x86_ucontext_t)))
            return (uint64_t)(-EFAULT);
        linux_x86_ucontext_t *uc =
            (linux_x86_ucontext_t *)(uintptr_t)ucontext_address;
        const uint64_t *gregs = uc->uc_mcontext.gregs;
        uint64_t rip = gregs[LINUX_REG_RIP];
        uint64_t rsp = gregs[LINUX_REG_RSP];
        if (!linux_user_range_mapped(rip, 1) || rsp >= 0x0000800000000000ULL)
            return (uint64_t)(-EFAULT);

        frame->call.nr = gregs[LINUX_REG_RAX];
        frame->call.a0 = gregs[LINUX_REG_RDI];
        frame->call.a1 = gregs[LINUX_REG_RSI];
        frame->call.a2 = gregs[LINUX_REG_RDX];
        frame->call.a3 = gregs[LINUX_REG_R10];
        frame->call.a4 = gregs[LINUX_REG_R8];
        frame->call.a5 = gregs[LINUX_REG_R9];
        frame->rbx = gregs[LINUX_REG_RBX];
        frame->rbp = gregs[LINUX_REG_RBP];
        frame->r12 = gregs[LINUX_REG_R12];
        frame->r13 = gregs[LINUX_REG_R13];
        frame->r14 = gregs[LINUX_REG_R14];
        frame->r15 = gregs[LINUX_REG_R15];
        frame->rcx = gregs[LINUX_REG_RCX];
        frame->r11 = gregs[LINUX_REG_R11];
        frame->rip = rip;
        frame->rflags = gregs[LINUX_REG_EFL];
        frame->rsp = rsp;
        /* Never let a user frame claim a privileged code segment.
         * Parenthesize SEL_UCODE: the cpu.h macro ends with `| 3`. */
        if (((gregs[LINUX_REG_CSGSFS] >> 32) & 0xffff) ==
            (SEL_UCODE))
            frame->cs = SEL_UCODE;
        task->sig_blocked.sig[0] = uc->uc_sigmask;
        task->sig_siginfo_depth--;
        task->sig_suppress_delivery = true;
        return gregs[LINUX_REG_RAX];
    }

    if (!task->sig_frame_active)
        return (uint64_t)(-EINVAL);
    frame->call.nr = task->sig_saved_rax;
    frame->call.a0 = task->sig_saved_args[0];
    frame->call.a1 = task->sig_saved_args[1];
    frame->call.a2 = task->sig_saved_args[2];
    frame->call.a3 = task->sig_saved_args[3];
    frame->call.a4 = task->sig_saved_args[4];
    frame->call.a5 = task->sig_saved_args[5];
    frame->rbx = task->sig_saved_callee[0];
    frame->rbp = task->sig_saved_callee[1];
    frame->r12 = task->sig_saved_callee[2];
    frame->r13 = task->sig_saved_callee[3];
    frame->r14 = task->sig_saved_callee[4];
    frame->r15 = task->sig_saved_callee[5];
    frame->rcx = task->sig_saved_rcx;
    frame->r11 = task->sig_saved_r11;
    frame->rip = task->sig_saved_rip;
    frame->rflags = task->sig_saved_rflags;
    frame->rsp = task->sig_saved_rsp;
    task->sig_blocked = task->sig_saved_blocked;
    task->sig_frame_active = false;
    task->sig_suppress_delivery = true;
    return task->sig_saved_rax;
}

/* Called by linux_syscall_entry after the syscall result has been written
 * into frame->call.nr.  This is the first safe place where both the current
 * user return context and the target address space are available. */
void linux_signal_prepare_return(hbos_syscall_frame_t *syscall_frame) {
    task_t *task = task_current();
    if (!task || !syscall_frame) return;
    if (task->sig_suppress_delivery) {
        task->sig_suppress_delivery = false;
        return;
    }
    if (task->sig_frame_active) return;

    uint64_t deliverable =
        task->sig_pending.sig[0] & ~task->sig_blocked.sig[0];
    if (!deliverable) return;
    int signal_number = 0;
    while (deliverable) {
        int signal = __builtin_ctzll(deliverable) + 1;
        if (task->sig_handler[signal] != SIG_DFL &&
            task->sig_handler[signal] != SIG_IGN) {
            signal_number = signal;
            break;
        }
        deliverable &= deliverable - 1;
    }
    if (!signal_number) return;

    uint64_t flags = task->sig_action_flags[signal_number];
    uint64_t restorer = task->sig_action_restorer[signal_number];
    uint64_t handler =
        (uint64_t)(uintptr_t)task->sig_handler[signal_number];
    if (!(flags & LINUX_SA_RESTORER) || !restorer) return;

    linux_x86_syscall_return_frame_t *frame =
        (linux_x86_syscall_return_frame_t *)syscall_frame;
    if (frame->rsp < 136) return;

    if (flags & LINUX_SA_SIGINFO) {
        if (task->sig_siginfo_depth >= LINUX_SIGINFO_NEST_MAX) return;
        uint64_t gregs[23];
        memset(gregs, 0, sizeof(gregs));
        gregs[LINUX_REG_R8]  = frame->call.a4;
        gregs[LINUX_REG_R9]  = frame->call.a5;
        gregs[LINUX_REG_R10] = frame->call.a3;
        gregs[LINUX_REG_R11] = frame->r11;
        gregs[LINUX_REG_R12] = frame->r12;
        gregs[LINUX_REG_R13] = frame->r13;
        gregs[LINUX_REG_R14] = frame->r14;
        gregs[LINUX_REG_R15] = frame->r15;
        gregs[LINUX_REG_RDI] = frame->call.a0;
        gregs[LINUX_REG_RSI] = frame->call.a1;
        gregs[LINUX_REG_RBP] = frame->rbp;
        gregs[LINUX_REG_RBX] = frame->rbx;
        gregs[LINUX_REG_RDX] = frame->call.a2;
        gregs[LINUX_REG_RAX] = frame->call.nr;
        gregs[LINUX_REG_RCX] = frame->rcx;
        gregs[LINUX_REG_RSP] = frame->rsp;
        gregs[LINUX_REG_RIP] = frame->rip;
        gregs[LINUX_REG_EFL] = frame->rflags;
        gregs[LINUX_REG_CSGSFS] = (uint64_t)frame->cs << 32;

        linux_siginfo_built_t built;
        if (linux_siginfo_deliver(task, signal_number, flags, restorer,
                                  frame->rsp, LINUX_SI_USER, 0, 0, 0, gregs,
                                  &built) < 0)
            return;
        frame->call.nr = 0;
        frame->call.a0 = (uint64_t)signal_number;
        frame->call.a1 = (uint64_t)(uintptr_t)built.siginfo_address;
        frame->call.a2 = (uint64_t)(uintptr_t)built.ucontext_address;
        frame->call.a3 = 0;
        frame->call.a4 = 0;
        frame->call.a5 = 0;
        frame->rip = handler;
        frame->rsp = built.handler_rsp;
        return;
    }

    if (task->sig_siginfo_depth > 0) return;
    uint64_t signal_rsp = ((frame->rsp - 128) & ~15ULL) - 8;
    if (!linux_user_range_writable(signal_rsp, sizeof(uint64_t))) return;
    *(uint64_t *)(uintptr_t)signal_rsp = restorer;

    task->sig_saved_rax = frame->call.nr;
    task->sig_saved_args[0] = frame->call.a0;
    task->sig_saved_args[1] = frame->call.a1;
    task->sig_saved_args[2] = frame->call.a2;
    task->sig_saved_args[3] = frame->call.a3;
    task->sig_saved_args[4] = frame->call.a4;
    task->sig_saved_args[5] = frame->call.a5;
    task->sig_saved_callee[0] = frame->rbx;
    task->sig_saved_callee[1] = frame->rbp;
    task->sig_saved_callee[2] = frame->r12;
    task->sig_saved_callee[3] = frame->r13;
    task->sig_saved_callee[4] = frame->r14;
    task->sig_saved_callee[5] = frame->r15;
    task->sig_saved_rcx = frame->rcx;
    task->sig_saved_r11 = frame->r11;
    task->sig_saved_rip = frame->rip;
    task->sig_saved_rflags = frame->rflags;
    task->sig_saved_rsp = frame->rsp;
    task->sig_saved_blocked = task->sig_blocked;
    task->sig_frame_active = true;

    linux_signal_consume(task, signal_number, flags);

    frame->call.nr = 0;
    frame->call.a0 = (uint64_t)signal_number;
    frame->call.a1 = 0;
    frame->call.a2 = 0;
    frame->call.a3 = 0;
    frame->call.a4 = 0;
    frame->call.a5 = 0;
    frame->rip = handler;
    frame->rsp = signal_rsp;
}

int linux_signal_prepare_exception(void *exception_frame,
                                   int signal_number, uint64_t cr2,
                                   uint64_t err_code) {
    isr_regs_t *frame = (isr_regs_t *)exception_frame;
    task_t *task = task_current();
    if (!frame || !task || (frame->cs & 3) != 3 ||
        signal_number <= 0 || signal_number >= _NSIG ||
        task->sig_frame_active)
        return -1;

    uint64_t pending_bit = 1ULL << (signal_number - 1);
    uint64_t flags = task->sig_action_flags[signal_number];
    uint64_t handler =
        (uint64_t)(uintptr_t)task->sig_handler[signal_number];
    uint64_t restorer = task->sig_action_restorer[signal_number];
    if (handler <= 1 || (task->sig_blocked.sig[0] & pending_bit) ||
        !(flags & LINUX_SA_RESTORER) || !restorer ||
        !linux_user_range_mapped(handler, 1) ||
        !linux_user_range_mapped(restorer, 1) || frame->rsp < 136)
        return -1;

    if (flags & LINUX_SA_SIGINFO) {
        if (task->sig_siginfo_depth >= LINUX_SIGINFO_NEST_MAX) return -1;
        uint64_t gregs[23];
        memset(gregs, 0, sizeof(gregs));
        gregs[LINUX_REG_R8]  = frame->r8;
        gregs[LINUX_REG_R9]  = frame->r9;
        gregs[LINUX_REG_R10] = frame->r10;
        gregs[LINUX_REG_R11] = frame->r11;
        gregs[LINUX_REG_R12] = frame->r12;
        gregs[LINUX_REG_R13] = frame->r13;
        gregs[LINUX_REG_R14] = frame->r14;
        gregs[LINUX_REG_R15] = frame->r15;
        gregs[LINUX_REG_RDI] = frame->rdi;
        gregs[LINUX_REG_RSI] = frame->rsi;
        gregs[LINUX_REG_RBP] = frame->rbp;
        gregs[LINUX_REG_RBX] = frame->rbx;
        gregs[LINUX_REG_RDX] = frame->rdx;
        gregs[LINUX_REG_RAX] = frame->rax;
        gregs[LINUX_REG_RCX] = frame->rcx;
        gregs[LINUX_REG_RSP] = frame->rsp;
        gregs[LINUX_REG_RIP] = frame->rip;
        gregs[LINUX_REG_EFL] = frame->rflags;
        gregs[LINUX_REG_CSGSFS] = (uint64_t)frame->cs << 32;
        gregs[LINUX_REG_ERR] = err_code;
        gregs[LINUX_REG_TRAPNO] = 14;
        gregs[LINUX_REG_CR2] = cr2;

        int si_code = (err_code & 0x1) ? LINUX_SEGV_ACCERR
                                       : LINUX_SEGV_MAPERR;
        linux_siginfo_built_t built;
        if (linux_siginfo_deliver(task, signal_number, flags, restorer,
                                  frame->rsp, si_code, cr2, err_code, 14,
                                  gregs, &built) < 0)
            return -1;
        frame->rdi = (uint64_t)signal_number;
        frame->rsi = (uint64_t)(uintptr_t)built.siginfo_address;
        frame->rdx = (uint64_t)(uintptr_t)built.ucontext_address;
        frame->rip = handler;
        frame->rsp = built.handler_rsp;
        return 1;
    }

    if (task->sig_siginfo_depth > 0) return -1;
    uint64_t signal_rsp = ((frame->rsp - 128) & ~15ULL) - 8;
    if (!linux_user_range_writable(signal_rsp, sizeof(uint64_t)))
        return -1;
    *(uint64_t *)(uintptr_t)signal_rsp = restorer;

    task->sig_saved_rax = frame->rax;
    task->sig_saved_args[0] = frame->rdi;
    task->sig_saved_args[1] = frame->rsi;
    task->sig_saved_args[2] = frame->rdx;
    task->sig_saved_args[3] = frame->r10;
    task->sig_saved_args[4] = frame->r8;
    task->sig_saved_args[5] = frame->r9;
    task->sig_saved_callee[0] = frame->rbx;
    task->sig_saved_callee[1] = frame->rbp;
    task->sig_saved_callee[2] = frame->r12;
    task->sig_saved_callee[3] = frame->r13;
    task->sig_saved_callee[4] = frame->r14;
    task->sig_saved_callee[5] = frame->r15;
    task->sig_saved_rcx = frame->rcx;
    task->sig_saved_r11 = frame->r11;
    task->sig_saved_rip = frame->rip;
    task->sig_saved_rflags = frame->rflags;
    task->sig_saved_rsp = frame->rsp;
    task->sig_saved_blocked = task->sig_blocked;
    task->sig_frame_active = true;

    linux_signal_consume(task, signal_number, flags);

    frame->rdi = (uint64_t)signal_number;
    frame->rsi = 0;
    frame->rdx = 0;
    frame->rip = handler;
    frame->rsp = signal_rsp;
    return 1;
}

static void linux_copy_stat(linux_x86_stat_t *output,
                            const struct stat *input) {
    memset(output, 0, sizeof(*output));
    output->st_dev = input->st_dev;
    output->st_ino = input->st_ino;
    output->st_nlink = input->st_nlink;
    output->st_mode = input->st_mode;
    output->st_uid = input->st_uid;
    output->st_gid = input->st_gid;
    output->st_rdev = input->st_rdev;
    output->st_size = input->st_size;
    output->st_blksize = 4096;
    output->st_blocks =
        input->st_size > 0 ? (input->st_size + 511) / 512 : 0;
    output->st_atime = input->st_atime;
    output->st_mtime = input->st_mtime;
    output->st_ctime = input->st_ctime;
}

static uint64_t linux_stat_path(const char *path, linux_x86_stat_t *output) {
    if (!path || !output) return (uint64_t)(-EFAULT);
    struct stat native;
    if (stat(path, &native) < 0)
        return (uint64_t)(-(errno > 0 ? errno : EIO));
    linux_copy_stat(output, &native);
    return 0;
}

static uint64_t linux_lstat_path(const char *path, linux_x86_stat_t *output) {
    if (!path || !output) return (uint64_t)(-EFAULT);
    struct stat native;
    if (lstat(path, &native) < 0)
        return (uint64_t)(-(errno > 0 ? errno : EIO));
    linux_copy_stat(output, &native);
    return 0;
}

static uint64_t linux_fstat_fd(int fd, linux_x86_stat_t *output) {
    if (!output) return (uint64_t)(-EFAULT);
    struct stat native;
    task_t *current = task_current();
    if (current && current->fd_table && fd >= 0 && fd < POSIX_MAX_FDS &&
        current->fd_table->entries[fd].used &&
        (current->fd_table->entries[fd].type == FD_MEMFD ||
         current->fd_table->entries[fd].type == FD_INOTIFY)) {
        if (current->fd_table->entries[fd].type == FD_INOTIFY) {
            memset(&native, 0, sizeof(native));
            native.st_dev = 4;
            native.st_ino =
                (ino_t)(current->fd_table->entries[fd].compat_id + 1);
            native.st_mode = S_IFREG | S_IRUSR | S_IWUSR;
            native.st_nlink = 1;
            linux_copy_stat(output, &native);
            return 0;
        }
        uint64_t size;
        if (linux_compat_memfd_size(fd, &size) < 0)
            return (uint64_t)(-(errno > 0 ? errno : EBADF));
        memset(&native, 0, sizeof(native));
        native.st_dev = 3;
        native.st_ino = (ino_t)(current->fd_table->entries[fd].compat_id + 1);
        native.st_mode = S_IFREG | S_IRUSR | S_IWUSR;
        native.st_nlink = 1;
        native.st_size = (off_t)size;
        linux_copy_stat(output, &native);
        return 0;
    }
    if (fstat(fd, &native) < 0)
        return (uint64_t)(-(errno > 0 ? errno : EIO));
    linux_copy_stat(output, &native);
    return 0;
}

static int64_t linux_native_call(uint64_t number, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5) {
    hbos_syscall_frame_t native = {
        .nr = number, .a0 = a0, .a1 = a1, .a2 = a2,
        .a3 = a3, .a4 = a4, .a5 = a5
    };
    return (int64_t)syscall_dispatch_frame(&native);
}

/* Resolve a Linux *at pathname without adding a second VFS path model.
 * Absolute paths intentionally ignore dirfd, matching Linux.  Relative
 * paths reuse the canonical path already retained by HBOS directory fds. */
static int linux_resolve_at_path(int dirfd, const char *path,
                                 char output[VFS_MAX_NAME]) {
    if (!path) return -EFAULT;
    if (!path[0]) return -ENOENT;
    if (path[0] == '/')
        return vfs_resolve_path("/", path, output, VFS_MAX_NAME) < 0 ?
            -EINVAL : 0;

    const char *base = NULL;
    char cwd[VFS_MAX_NAME];
    if (dirfd == -100) { /* AT_FDCWD */
        if (!getcwd(cwd, sizeof(cwd))) return -EIO;
        base = cwd;
    } else {
        task_t *current = task_current();
        if (!current || !current->fd_table || dirfd < 0 ||
            dirfd >= POSIX_MAX_FDS ||
            !current->fd_table->entries[dirfd].used)
            return -EBADF;
        fd_entry_t *entry = &current->fd_table->entries[dirfd];
        if (!entry->node || entry->node->type != VFS_NODE_DIR)
            return -ENOTDIR;
        if (!entry->path[0]) return -ENOENT;
        base = entry->path;
    }
    return vfs_resolve_path(base, path, output, VFS_MAX_NAME) < 0 ?
        -EINVAL : 0;
}

static uint64_t linux_newfstatat(int dirfd, const char *path,
                                 linux_x86_stat_t *output, int flags) {
    const int supported = AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW | 0x800;
    if (flags & ~supported) return (uint64_t)(-EINVAL);
    if (!path || !output) return (uint64_t)(-EFAULT);
    if (!path[0]) {
        if (!(flags & AT_EMPTY_PATH)) return (uint64_t)(-ENOENT);
        if (dirfd != AT_FDCWD) return linux_fstat_fd(dirfd, output);
        char cwd[VFS_MAX_NAME];
        if (!getcwd(cwd, sizeof(cwd)))
            return (uint64_t)(-(errno > 0 ? errno : EIO));
        return linux_stat_path(cwd, output);
    }
    char resolved[VFS_MAX_NAME];
    int status = linux_resolve_at_path(dirfd, path, resolved);
    if (status < 0) return (uint64_t)(int64_t)status;
    return (flags & AT_SYMLINK_NOFOLLOW) ?
        linux_lstat_path(resolved, output) : linux_stat_path(resolved, output);
}

static uint64_t linux_vector_io(int fd, const linux_x86_iovec_t *vectors,
                                uint64_t count, int write_operation) {
    if ((!vectors && count) || count > 1024)
        return (uint64_t)(count > 1024 ? -EINVAL : -EFAULT);
    int64_t total = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (!vectors[i].base && vectors[i].length)
            return total ? (uint64_t)total : (uint64_t)(-EFAULT);
        if (vectors[i].length > (uint64_t)(INT64_MAX - total))
            return total ? (uint64_t)total : (uint64_t)(-EINVAL);
        int64_t result = linux_native_call(
            write_operation ? HBOS_SYS_WRITE : HBOS_SYS_READ,
            (uint64_t)fd, (uint64_t)(uintptr_t)vectors[i].base,
            vectors[i].length, 0, 0, 0);
        if (result < 0) return total ? (uint64_t)total : (uint64_t)result;
        total += result;
        if ((uint64_t)result < vectors[i].length) break;
    }
    return (uint64_t)total;
}

static uint64_t linux_pread64(int fd, void *buffer, uint64_t count,
                              uint64_t offset) {
    if (!buffer && count) return (uint64_t)(-EFAULT);
    if (count > INT32_MAX) return (uint64_t)(-EINVAL);
    task_t *current = task_current();
    if (!current || !current->fd_table || fd < 0 || fd >= POSIX_MAX_FDS)
        return (uint64_t)(-EBADF);
    fd_entry_t *entry = &current->fd_table->entries[fd];
    if (!entry->used) return (uint64_t)(-EBADF);

    if (entry->type == FD_MEMFD) {
        uint64_t size;
        if (linux_compat_memfd_size(fd, &size) < 0)
            return (uint64_t)(-EBADF);
        if (offset >= size) return 0;
        uint64_t available = size - offset;
        if (count > available) count = available;
        if (linux_compat_memfd_read_at(fd, offset, buffer, (size_t)count) < 0)
            return (uint64_t)(-EIO);
        return count;
    }

    if (entry->type != FD_FILE || !entry->node)
        return (uint64_t)(-ESPIPE);
    if (offset > UINT32_MAX) return 0;
    int result = vfs_read(entry->node, (uint32_t)offset, buffer,
                          (uint32_t)count);
    return result < 0 ? (uint64_t)(-EIO) : (uint64_t)result;
}

/* pwrite64：sqlite 的 xWrite 走的就是它（日志里 nr=18 标 unimplemented
 * 之后，History/UKM 数据库立刻 "disk I/O error"）。与 pread64 对称：memfd
 * 走兼容层，普通文件直接 vfs_write，偏移由参数给、不动 fd 的当前位置。 */
static uint64_t linux_pwrite64(int fd, const void *buffer, uint64_t count,
                               uint64_t offset) {
    if (!buffer && count) return (uint64_t)(-EFAULT);
    if (count > INT32_MAX) return (uint64_t)(-EINVAL);
    task_t *current = task_current();
    if (!current || !current->fd_table || fd < 0 || fd >= POSIX_MAX_FDS)
        return (uint64_t)(-EBADF);
    fd_entry_t *entry = &current->fd_table->entries[fd];
    if (!entry->used) return (uint64_t)(-EBADF);
    if (entry->type == FD_MEMFD) return (uint64_t)(-EIO);
    if (entry->type != FD_FILE || !entry->node)
        return (uint64_t)(-ESPIPE);
    if (offset > UINT32_MAX || count > UINT32_MAX - offset)
        return (uint64_t)(-EFBIG);
    int result = vfs_write(entry->node, (uint32_t)offset, buffer,
                           (uint32_t)count);
    if (result < 0) return (uint64_t)(-EIO);
    return (uint64_t)result;
}

/* fallocate(fd, mode, offset, len)：sqlite/Chromium 用来预分配文件尾部。
 * 只支持"分配/保持大小"这一档；打洞（PUNCH_HOLE）按"读出来就是 0"处理，
 * 不真回收块。 */
static uint64_t linux_fallocate(int fd, int mode, uint64_t offset,
                                uint64_t length) {
    const int KEEP_SIZE = 0x01;
    const int PUNCH_HOLE = 0x02;
    task_t *current = task_current();
    if (!current || !current->fd_table || fd < 0 || fd >= POSIX_MAX_FDS ||
        !current->fd_table->entries[fd].used)
        return (uint64_t)(-EBADF);
    fd_entry_t *entry = &current->fd_table->entries[fd];
    if (length == 0) return (uint64_t)(-EINVAL);
    if (offset > UINT32_MAX || length > UINT32_MAX ||
        offset + length > UINT32_MAX)
        return (uint64_t)(-EFBIG);
    if (mode & PUNCH_HOLE) return 0;
    if (mode != 0 && mode != KEEP_SIZE) return (uint64_t)(-EOPNOTSUPP);
    if (entry->type == FD_MEMFD) {
        uint64_t size = 0;
        if (linux_compat_memfd_size(fd, &size) < 0)
            return (uint64_t)(-EBADF);
        if (!(mode & KEEP_SIZE) && offset + length > size)
            return linux_compat_memfd_truncate(fd, offset + length) < 0
                ? (uint64_t)(-EIO) : 0;
        return 0;
    }
    if (entry->type != FD_FILE || !entry->node)
        return (uint64_t)(-ESPIPE);
    if (mode & KEEP_SIZE) return 0;
    if (offset + length > entry->node->size &&
        vfs_truncate(entry->node, (uint32_t)(offset + length)) < 0)
        return (uint64_t)(-EIO);
    return 0;
}

static uint64_t linux_message_io(int fd, linux_x86_msghdr_t *message,
                                 int flags, int send_operation) {
    if (!message || (!message->vectors && message->vector_count))
        return (uint64_t)(-EFAULT);
    if (message->vector_count > 1024) return (uint64_t)(-EINVAL);
    int rights[4];
    size_t rights_count = 0;
    uint64_t control_capacity = message->control_length;
    task_t *current = task_current();
    int unix_fd = current && current->fd_table && fd >= 0 &&
        fd < POSIX_MAX_FDS && current->fd_table->entries[fd].used &&
        current->fd_table->entries[fd].type == FD_UNIX;
    if (send_operation && message->control && message->control_length) {
        if (message->control_length < sizeof(linux_x86_cmsghdr_t))
            return (uint64_t)(-EINVAL);
        linux_x86_cmsghdr_t *header =
            (linux_x86_cmsghdr_t *)message->control;
        if (header->length < sizeof(*header) ||
            header->length > message->control_length ||
            header->level != 1 || header->type != 1 ||
            (header->length - sizeof(*header)) % sizeof(int))
            return (uint64_t)(-EOPNOTSUPP);
        rights_count =
            (size_t)((header->length - sizeof(*header)) / sizeof(int));
        if (!rights_count || rights_count > 4)
            return (uint64_t)(-EINVAL);
        memcpy(rights, (uint8_t *)message->control + sizeof(*header),
               rights_count * sizeof(int));
        if (linux_compat_unix_send_rights(fd, rights, rights_count) < 0)
            return (uint64_t)(-(errno > 0 ? errno : EINVAL));
    } else if (send_operation && message->control_length) {
        return (uint64_t)(-EFAULT);
    }
    if (send_operation && message->name && message->name_length) {
        int64_t connected = linux_native_call(
            HBOS_SYS_CONNECT, (uint64_t)fd,
            (uint64_t)(uintptr_t)message->name, message->name_length,
            0, 0, 0);
        if (connected < 0 && connected != -EISCONN)
            return (uint64_t)connected;
    }
    if (!send_operation) {
        message->flags = 0;
        message->name_length = 0;
    }

    int64_t total = 0;
    for (uint64_t i = 0; i < message->vector_count; i++) {
        linux_x86_iovec_t *vector = &message->vectors[i];
        if (!vector->base && vector->length)
            return total ? (uint64_t)total : (uint64_t)(-EFAULT);
        int vector_flags = flags;
        if (!send_operation && total) vector_flags |= 0x40; /* MSG_DONTWAIT */
        int64_t result = linux_native_call(
            send_operation ? HBOS_SYS_SEND : HBOS_SYS_RECV,
            (uint64_t)fd, (uint64_t)(uintptr_t)vector->base,
            vector->length, (uint64_t)vector_flags, 0, 0);
        if (result < 0) {
            if (!send_operation && total && result == -EAGAIN) break;
            return total ? (uint64_t)total : (uint64_t)result;
        }
        total += result;
        if ((uint64_t)result < vector->length) break;
    }
    if (!send_operation && unix_fd) {
        size_t received = 0;
        int truncated = 0;
        int *output_fds = NULL;
        size_t fd_capacity = 0;
        if (message->control &&
            control_capacity >= sizeof(linux_x86_cmsghdr_t)) {
            output_fds = (int *)((uint8_t *)message->control +
                                 sizeof(linux_x86_cmsghdr_t));
            fd_capacity =
                (size_t)((control_capacity -
                          sizeof(linux_x86_cmsghdr_t)) / sizeof(int));
            if (fd_capacity > 4) fd_capacity = 4;
        }
        if (linux_compat_unix_recv_rights(
                fd, output_fds, fd_capacity, &received, &truncated) < 0)
            return total ? (uint64_t)total
                         : (uint64_t)(-(errno > 0 ? errno : EINVAL));
        if (received && message->control) {
            linux_x86_cmsghdr_t *header =
                (linux_x86_cmsghdr_t *)message->control;
            header->length = sizeof(*header) + received * sizeof(int);
            header->level = 1;
            header->type = 1;
            message->control_length =
                (header->length + 7) & ~(uint64_t)7;
            if (message->control_length > control_capacity)
                message->control_length = control_capacity;
        } else {
            message->control_length = 0;
        }
        if (truncated) message->flags |= 0x08; /* MSG_CTRUNC */
    } else if (!send_operation) {
        message->control_length = 0;
    }
    return (uint64_t)total;
}

static uint64_t linux_getdents64(int fd, void *buffer, uint64_t count) {
    if (!buffer) return (uint64_t)(-EFAULT);
    task_t *current = task_current();
    if (!current || !current->fd_table || fd < 0 || fd >= POSIX_MAX_FDS ||
        !current->fd_table->entries[fd].used)
        return (uint64_t)(-EBADF);
    fd_entry_t *entry = &current->fd_table->entries[fd];
    if (!entry->node || entry->node->type != VFS_NODE_DIR)
        return (uint64_t)(-ENOTDIR);
    if (!entry->path[0]) return 0;

    uint64_t written = 0;
    int is_virtual_dir =
        strcmp(entry->path, "/") == 0 ||
        strcmp(entry->path, "/dev") == 0 ||
        strcmp(entry->path, "/proc") == 0 ||
        strcmp(entry->path, "/sys") == 0 ||
        strncmp(entry->path, "/proc/", 6) == 0 ||
        strncmp(entry->path, "/sys/", 5) == 0;
    if (is_virtual_dir) {
        for (;;) {
            char name[VFS_MAX_NAME];
            uint32_t type;
            if (vfs_readdir_at(entry->path, entry->offset, name, &type) < 0)
                break;
            size_t name_length = strlen(name) + 1;
            uint64_t record_length = (19 + name_length + 7) & ~(uint64_t)7;
            if (written + record_length > count) break;
            uint8_t *record = (uint8_t *)buffer + written;
            memset(record, 0, (size_t)record_length);
            *(uint64_t *)(record + 0) = entry->offset + 1;
            *(int64_t *)(record + 8) = (int64_t)(entry->offset + 1);
            *(uint16_t *)(record + 16) = (uint16_t)record_length;
            record[18] = type == VFS_NODE_DIR ? DT_DIR :
                         (type == VFS_NODE_SYMLINK ? DT_LNK : DT_REG);
            memcpy(record + 19, name, name_length);
            entry->offset++;
            written += record_length;
        }
        return written;
    }

    /*
     * 顺序游标读取，避免 O(n²)。
     *
     * 原实现每轮调用 vfs_readdir_at(entry->path, entry->offset)，而该函数
     * 内部为取第 offset 项要从 0 重扫到 offset（每次还各自 opendir/closedir）。
     * 列一个 n 项目录就是 O(n²) 次底层 readdir。改为只 opendir 一次、用
     * vfs_readdir 顺序推进：整体 O(n)。
     *
     * entry->offset 语义不变——它是"下一条目的序号"，与 Linux d_off 一致，
     * 因此跨多次 getdents64 调用仍可正确续读。重新进入时先把游标快进到
     * 当前 offset（seek 场景），之后顺序消费。
     */
    if (vfs_opendir(entry->path) < 0) return 0;
    for (uint32_t skip = 0; skip < entry->offset; skip++) {
        char scratch[VFS_MAX_NAME];
        uint32_t scratch_type;
        if (vfs_readdir(entry->path, scratch, &scratch_type) < 0) {
            vfs_closedir(entry->path);
            return written;
        }
    }
    for (;;) {
        char name[VFS_MAX_NAME];
        uint32_t type;
        if (vfs_readdir(entry->path, name, &type) < 0)
            break;
        size_t name_length = strlen(name) + 1;
        uint64_t record_length = (19 + name_length + 7) & ~(uint64_t)7;
        if (written + record_length > count) break;
        uint8_t *record = (uint8_t *)buffer + written;
        memset(record, 0, (size_t)record_length);
        *(uint64_t *)(record + 0) = entry->offset + 1;
        *(int64_t *)(record + 8) = (int64_t)(entry->offset + 1);
        *(uint16_t *)(record + 16) = (uint16_t)record_length;
        record[18] = type == VFS_NODE_DIR ? DT_DIR :
                     (type == VFS_NODE_SYMLINK ? DT_LNK : DT_REG);
        memcpy(record + 19, name, name_length);
        entry->offset++;
        written += record_length;
    }
    vfs_closedir(entry->path);   /* 与上面的 vfs_opendir 配对 */
    return written;
}

/* ── 未实现 Linux 系统调用的串口跟踪（C3 起步工具）─────────────────
 *
 * 大型程序（Chromium / glib / NSS）不能靠猜补 syscall：先让内核把它调用了
 * 什么号、带什么参数打到串口，再按"出现顺序 → 致命性"逐个补。每个号最多
 * 打 4 次，避免 poll 循环把串口刷爆；同号后续调用静默（仍返回 -ENOSYS）。
 * 需要完整轨迹时把 HBOS_TRACE_ALL_LINUX_SYSCALLS 打开（音量很大）。 */
#define HBOS_TRACE_ALL_LINUX_SYSCALLS 0

static void trace_put_udec(uint64_t value) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = (char)('0' + (int)(value % 10));
        value /= 10;
    } while (value);
    while (n--) console_putchar(digits[n]);
}

static void trace_put_hex(uint64_t value) {
    static const char hex[] = "0123456789abcdef";
    char digits[16];
    int n = 0;
    do {
        digits[n++] = hex[value & 0xf];
        value >>= 4;
    } while (value);
    console_puts("0x");
    while (n--) console_putchar(digits[n]);
}

/* 静默 exit(0) 时只有"谁调用的"能定位。Linux syscall 返回帧的栈布局
 * （见 src/core/interrupt_asm.asm linux_syscall_entry）在 a0..a5 之后还
 * 依次保存 rcx/r11/rbx/rbp/r12..r15/rip/cs/rflags/rsp/ss，所以能从这里
 * 取回用户态 rip/rbp 并走一遍 RBP 链。Chromium 的 x86_64 版本带帧指针，
 * 链基本完整；链断掉时退化为扫描用户栈上的代码地址。 */
static void linux_user_backtrace(hbos_syscall_frame_t *frame,
                                 const char *tag) {
    const uint64_t *slots = (const uint64_t *)frame;
    uint64_t rip = slots[15];
    uint64_t rbp = slots[10];
    uint64_t rsp = slots[18];

    console_puts("  [");
    console_puts(tag);
    console_puts("-BT] rip=");
    trace_put_hex(rip);
    console_putchar('\n');

    int depth = 0;
    uint64_t cursor = rbp;
    while (depth < 24 && linux_user_range_mapped(cursor, 16)) {
        uint64_t next = *(volatile uint64_t *)(uintptr_t)cursor;
        uint64_t ret = *(volatile uint64_t *)(uintptr_t)(cursor + 8);
        if (!ret) break;
        console_puts("  [");
        console_puts(tag);
        console_puts("-BT] bt[");
        trace_put_udec((uint64_t)depth);
        console_puts("]=");
        trace_put_hex(ret);
        console_putchar('\n');
        depth++;
        if (next <= cursor) break;
        cursor = next;
    }
    if (depth >= 2) return;

    /* 帧指针缺失时的兜底：用户栈上落在 PIE 代码段的值当返回地址看。
     * Chromium 主程序映射在 0x1100000000 起的地址空洞里。 */
    uint64_t sp = rsp & ~7ULL;
    int shown = 0;
    for (int i = 0; i < 192 && shown < 16; i++, sp += 8) {
        if (!linux_user_range_mapped(sp, 8)) break;
        uint64_t value = *(volatile uint64_t *)(uintptr_t)sp;
        if (value < 0x1100000000ULL || value >= 0x1200000000ULL) continue;
        console_puts("  [");
        console_puts(tag);
        console_puts("-SCAN] sp+");
        trace_put_udec((uint64_t)i * 8);
        console_puts("=");
        trace_put_hex(value);
        console_putchar('\n');
        shown++;
    }
}

static void linux_trace_unimplemented(uint64_t number,
                                      const hbos_syscall_frame_t *frame) {
    static uint16_t seen[512];
    if (number < sizeof(seen) / sizeof(seen[0])) {
        if (seen[number] >= 4) return;
        seen[number]++;
    }
    console_puts("[LINUX_SYSCALL] unimplemented nr=");
    trace_put_udec(number);
    console_puts(" a0=");  trace_put_hex(frame->a0);
    console_puts(" a1=");  trace_put_hex(frame->a1);
    console_puts(" a2=");  trace_put_hex(frame->a2);
    console_puts(" a3=");  trace_put_hex(frame->a3);
    console_putchar('\n');
}

/* 文件映射失败的诊断输出。Chromium 的 MemoryMappedFile 只报
 * "Invalid file descriptor to ICU data received."，具体是 mmap 的哪一步
 * 失败看不出来；把参数原样打出来，一眼能对上是 fd 无效、文件类型不对、
 * offset 越界，还是分配物理页失败。 */
static void mmap_log_failure(int fd, size_t length, int prot, int flags,
                             uint64_t offset, uint64_t file_size, int error) {
    console_puts("[MMAP] fail err=");
    trace_put_udec((uint64_t)(-(int64_t)error));
    console_puts(" fd=");
    trace_put_udec((uint64_t)(unsigned)fd);
    console_puts(" len=");
    trace_put_udec(length);
    console_puts(" prot=");
    trace_put_udec((uint64_t)(unsigned)prot);
    console_puts(" flags=");
    trace_put_hex((uint64_t)(unsigned)flags);
    console_puts(" off=");
    trace_put_udec(offset);
    console_puts(" size=");
    trace_put_udec(file_size);
    console_putchar('\n');
}

typedef struct {
    int64_t  f_type;
    int64_t  f_bsize;    int64_t  f_blocks;
    int64_t  f_bfree;
    int64_t  f_bavail;
    int64_t  f_files;
    int64_t  f_ffree;
    int32_t  f_fsid[2];
    int64_t  f_namelen;
    int64_t  f_frsize;
    int64_t  f_flags;
    int64_t  f_spare[4];
} linux_x86_statfs_t;

/* Linux fcntl 语义适配（见下方 case 72 的说明）。
 * 与 HBOS 原生 fcntl 的编号不同，必须单独实现：
 *   0=F_DUPFD 1=F_GETFD 2=F_SETFD 3=F_GETFL 4=F_SETFL 1030=F_DUPFD_CLOEXEC */
static uint64_t linux_fcntl(int fd, int cmd, long argument) {
    task_t *cur = task_current();
    if (!cur || !cur->fd_table || fd < 0 || fd >= POSIX_MAX_FDS ||
        !cur->fd_table->entries[fd].used)
        return (uint64_t)(-EBADF);
    fd_entry_t *entry = &cur->fd_table->entries[fd];
    switch (cmd) {
        case 0:                                       /* F_DUPFD */
        case 1030: {                                  /* F_DUPFD_CLOEXEC */
            int lowest = argument > 0 ? (int)argument : 0;
            if (lowest < 3) lowest = 3;
            for (int i = lowest; i < POSIX_MAX_FDS; i++) {
                if (!cur->fd_table->entries[i].used) {
                    cur->fd_table->entries[i] = *entry;
                    linux_compat_retain(i);
                    return (uint64_t)i;
                }
            }
            return (uint64_t)(-EMFILE);
        }
        case 1:                                       /* F_GETFD */
        case 2:                                       /* F_SETFD */
            return 0;                                 /* 未跟踪 FD_CLOEXEC */
        case 3:                                       /* F_GETFL */
            return (uint64_t)(unsigned)entry->flags;
        case 4:                                       /* F_SETFL */
            entry->flags = (entry->flags & ~O_NONBLOCK) |
                           ((int)argument & O_NONBLOCK);
            return 0;
        /* 记录锁：sqlite（Chromium 的 History/UKM/Cookies 全是 sqlite）在
         * xLock 里必须拿到 F_SETLK 的成功返回，否则 unixError 把 EINVAL
         * 折成 SQLITE_IOERR，日志里就是 "disk I/O error"，紧接着
         * HistoryService::Init 失败 → TerminateCurrentProcessImmediately(0)
         * （单进程模式"静默 exit(0)"的真凶）。HBOS 目前没有跨进程文件锁
         * 语义，先按"总能拿到锁"处理；F_GETLK 报告无人持锁。 */
        case 5:                                       /* F_GETLK */
        case 36: {                                    /* F_OFD_GETLK */
            /* struct flock: short type/whence; off_t start/len; pid_t pid */
            uint32_t *lock = (uint32_t *)argument;
            if (!lock ||
                !linux_user_range_writable((uint64_t)(uintptr_t)lock, 32))
                return (uint64_t)(-EFAULT);
            *(uint16_t *)lock = 2;                    /* F_UNLCK */
            return 0;
        }
        case 6:                                       /* F_SETLK */
        case 7:                                       /* F_SETLKW */
        case 37:                                      /* F_OFD_SETLK */
        case 38:                                      /* F_OFD_SETLKW */
            if (!argument ||
                !linux_user_range_mapped((uint64_t)(uintptr_t)argument, 32))
                return (uint64_t)(-EFAULT);
            return 0;
        default:
            return (uint64_t)(-EINVAL);
    }
}

static uint64_t linux_dispatch_body(hbos_syscall_frame_t *linux_frame) {
    if (!linux_frame) return (uint64_t)(-EFAULT);
    hbos_syscall_frame_t native = *linux_frame;

    switch (linux_frame->nr) {
        case 0:   native.nr = HBOS_SYS_READ; break;
        case 1:   native.nr = HBOS_SYS_WRITE; break;
        case 2:   native.nr = HBOS_SYS_OPEN; break;
        case 3:   native.nr = HBOS_SYS_CLOSE; break;
        case 4:
            return linux_stat_path(
                (const char *)linux_frame->a0,
                (linux_x86_stat_t *)linux_frame->a1);
        case 5:
            return linux_fstat_fd(
                (int)linux_frame->a0,
                (linux_x86_stat_t *)linux_frame->a1);
        case 6:
            return linux_lstat_path(
                (const char *)linux_frame->a0,
                (linux_x86_stat_t *)linux_frame->a1);
        case 7:   native.nr = HBOS_SYS_POLL; break;
        case 8:   native.nr = HBOS_SYS_LSEEK; break;
        case 9:   native.nr = HBOS_SYS_MMAP; break;
        case 10:  native.nr = HBOS_SYS_MPROTECT; break;
        case 11:  native.nr = HBOS_SYS_MUNMAP; break;
        case 12:  native.nr = HBOS_SYS_BRK; break;
        case 13:
            return linux_rt_sigaction(
                (int)linux_frame->a0,
                (const linux_x86_sigaction_t *)linux_frame->a1,
                (linux_x86_sigaction_t *)linux_frame->a2,
                linux_frame->a3);
        case 14:
            if (linux_frame->a3 != sizeof(uint64_t))
                return (uint64_t)(-EINVAL);
            native.nr = HBOS_SYS_SIGPROCMASK;
            break;
        case 15:
            return linux_rt_sigreturn(linux_frame);
        case 16:  native.nr = HBOS_SYS_IOCTL; break;
        case 17:
            return linux_pread64((int)linux_frame->a0,
                                 (void *)linux_frame->a1,
                                 linux_frame->a2, linux_frame->a3);
        case 18:
            return linux_pwrite64((int)linux_frame->a0,
                                  (const void *)linux_frame->a1,
                                  linux_frame->a2, linux_frame->a3);
        case 19:
            return linux_vector_io(
                (int)linux_frame->a0,
                (const linux_x86_iovec_t *)linux_frame->a1,
                linux_frame->a2, 0);
        case 20:
            return linux_vector_io(
                (int)linux_frame->a0,
                (const linux_x86_iovec_t *)linux_frame->a1,
                linux_frame->a2, 1);
        case 21:  native.nr = HBOS_SYS_ACCESS; break;
        case 22:  native.nr = HBOS_SYS_PIPE; break;
        case 24:  native.nr = HBOS_SYS_SCHED_YIELD; break;
        /* mremap(old, old_size, new_size, flags, new_addr)：Linux 原生参数
         * 顺序与 HBOS_SYS_MREMAP 完全一致，直接透传。glibc 的大块
         * realloc/calloc 会走这里。 */
        case 25:  native.nr = HBOS_SYS_MREMAP; break;
        case 28:
            return linux_madvise(linux_frame->a0, linux_frame->a1,
                                 (int)linux_frame->a2);
        case 32:  native.nr = HBOS_SYS_DUP; break;
        case 33:  native.nr = HBOS_SYS_DUP2; break;
        case 35:  native.nr = HBOS_SYS_NANOSLEEP; break;
        case 39:  native.nr = HBOS_SYS_GETPID; break;
        case 41:  native.nr = HBOS_SYS_SOCKET; break;
        case 42:  native.nr = HBOS_SYS_CONNECT; break;
        case 43:  native.nr = HBOS_SYS_ACCEPT; break;
        case 44:  native.nr = HBOS_SYS_SEND; break;
        case 45:  native.nr = HBOS_SYS_RECV; break;
        case 46:
            return linux_message_io(
                (int)linux_frame->a0,
                (linux_x86_msghdr_t *)linux_frame->a1,
                (int)linux_frame->a2, 1);
        case 47:
            return linux_message_io(
                (int)linux_frame->a0,
                (linux_x86_msghdr_t *)linux_frame->a1,
                (int)linux_frame->a2, 0);
        case 48:  native.nr = HBOS_SYS_SHUTDOWN; break;
        case 49:  native.nr = HBOS_SYS_BIND; break;
        case 50:  native.nr = HBOS_SYS_LISTEN; break;
        case 51:  native.nr = HBOS_SYS_GETSOCKNAME; break;
        case 52:  native.nr = HBOS_SYS_GETPEERNAME; break;
        case 53:  native.nr = HBOS_SYS_SOCKETPAIR; break;
        case 54:  native.nr = HBOS_SYS_SETSOCKOPT; break;
        case 55:  native.nr = HBOS_SYS_GETSOCKOPT; break;
        case 56:
            return linux_clone(
                (linux_x86_syscall_return_frame_t *)linux_frame);
        case 57:
            return linux_fork(
                (linux_x86_syscall_return_frame_t *)linux_frame);
        case 59:  native.nr = HBOS_SYS_EXECVE; break;
        case 60:
        case 231:
            /* 静默退出最难查：不知道是哪个线程、拿什么码退的。主线程
             * 退出时连最近的系统调用轨迹一起打出来。 */
            console_puts("[EXIT] pid=");
            trace_put_udec(task_get_id());
            console_puts(" code=");
            trace_put_udec(linux_frame->a0 & 0xFF);
            console_putchar('\n');
            if (task_get_id() == task_get_process_id()) {
                linux_user_backtrace(linux_frame, "EXIT");
                linux_syscall_trace_dump();
            }
            native.nr = HBOS_SYS_EXIT; break;
        case 61:  native.nr = HBOS_SYS_WAITPID; break;
        case 62:  native.nr = HBOS_SYS_KILL; break;
        case 63:  native.nr = HBOS_SYS_UNAME; break;
        case 72:
            return linux_fcntl((int)linux_frame->a0, (int)linux_frame->a1,
                               (long)linux_frame->a2);
        case 77:  native.nr = HBOS_SYS_FTRUNCATE; break;
        case 79:  native.nr = HBOS_SYS_GETCWD; break;
        case 80:  native.nr = HBOS_SYS_CHDIR; break;
        case 82:
            native.nr = HBOS_SYS_RENAME;
            native.a2 = 0;
            break;
        case 83:  native.nr = HBOS_SYS_MKDIR; break;
        case 84:  native.nr = HBOS_SYS_RMDIR; break;
        case 87:  native.nr = HBOS_SYS_UNLINK; break;
        case 88:  native.nr = HBOS_SYS_SYMLINK; break;
        case 89:  native.nr = HBOS_SYS_READLINK; break;
        case 90:  native.nr = HBOS_SYS_CHMOD; break;
        case 91:                                       /* fchmod(fd, mode) */
        case 95:                                       /* fchmodat 的 fd 形态 */
            return 0;                                  /* 权限模型暂不区分 */
        case 92:  native.nr = HBOS_SYS_CHOWN; break;
        /* fchown/lchown：HBOS 的权限模型只有单用户，属主一律 root 且不可改；
         * 直接成功比 ENOSYS 安全（Chromium 装扩展/复制 profile 时会调）。 */
        case 93:
        case 94:
            return 0;
        case 96:  native.nr = HBOS_SYS_GETTOD; break;
        case 285:
            return linux_fallocate((int)linux_frame->a0,
                                   (int)linux_frame->a1,
                                   linux_frame->a2, linux_frame->a3);
        case 98:
            return linux_getrusage(
                (int)linux_frame->a0,
                (linux_x86_rusage_t *)linux_frame->a1);
        case 99:
            return linux_sysinfo((linux_x86_sysinfo_t *)linux_frame->a0);
        case 102: native.nr = HBOS_SYS_GETUID; break;
        case 104: native.nr = HBOS_SYS_GETGID; break;
        case 107: native.nr = HBOS_SYS_GETEUID; break;
        case 108: native.nr = HBOS_SYS_GETEGID; break;
        case 110: native.nr = HBOS_SYS_GETPPID; break;
        case 115: native.nr = HBOS_SYS_GETGROUPS; break;
        case 116: native.nr = HBOS_SYS_SETGROUPS; break;
        case 121: native.nr = HBOS_SYS_GETPGID; break;
        case 131:
            return linux_sigaltstack(
                (const linux_x86_stack_t *)linux_frame->a0,
                (linux_x86_stack_t *)linux_frame->a1,
                (linux_x86_syscall_return_frame_t *)linux_frame);
        case 157:
            return linux_prctl(linux_frame->a0, linux_frame->a1,
                               linux_frame->a2, linux_frame->a3,
                               linux_frame->a4);
        case 158: native.nr = HBOS_SYS_ARCH_PRCTL; break;
        case 186: native.nr = HBOS_SYS_GETTID; break;
        case 201: {
            linux_x86_timespec_t now;
            int result = linux_clock_now(0, &now);
            if (result < 0) return (uint64_t)(int64_t)result;
            if (linux_frame->a0)
                *(int64_t *)(uintptr_t)linux_frame->a0 = now.seconds;
            return (uint64_t)now.seconds;
        }
        case 202: native.nr = HBOS_SYS_FUTEX; break;
        case 217:
            return linux_getdents64(
                (int)linux_frame->a0, (void *)linux_frame->a1,
                linux_frame->a2);
        case 218: native.nr = HBOS_SYS_SET_TID_ADDRESS; break;
        case 228:
            return linux_clock_gettime(
                (int)linux_frame->a0,
                (linux_x86_timespec_t *)linux_frame->a1);
        case 230:
            return linux_clock_nanosleep(
                (int)linux_frame->a0, (int)linux_frame->a1,
                (const void *)linux_frame->a2,
                (void *)linux_frame->a3);
        case 232: native.nr = HBOS_SYS_EPOLL_WAIT; break;
        case 233: native.nr = HBOS_SYS_EPOLL_CTL; break;
        case 234:
            return linux_tgkill((int)linux_frame->a0,
                                (int)linux_frame->a1,
                                (int)linux_frame->a2);
        case 253: {
            int result = linux_compat_inotify_init1(0);
            return result < 0 ? (uint64_t)(-(errno > 0 ? errno : EIO)) :
                                (uint64_t)result;
        }
        case 254: {
            int result = linux_compat_inotify_add_watch(
                (int)linux_frame->a0, (const char *)linux_frame->a1,
                (uint32_t)linux_frame->a2);
            return result < 0 ? (uint64_t)(-(errno > 0 ? errno : EIO)) :
                                (uint64_t)result;
        }
        case 255: {
            int result = linux_compat_inotify_rm_watch(
                (int)linux_frame->a0, (int)linux_frame->a1);
            return result < 0 ? (uint64_t)(-(errno > 0 ? errno : EIO)) :
                                (uint64_t)result;
        }
        case 257: {
            char path[VFS_MAX_NAME];
            int resolved = linux_resolve_at_path(
                (int)linux_frame->a0, (const char *)linux_frame->a1, path);
            if (resolved < 0) return (uint64_t)(int64_t)resolved;
            return (uint64_t)linux_native_call(
                HBOS_SYS_OPEN, (uint64_t)path, linux_frame->a2,
                linux_frame->a3, 0, 0, 0);
        }
        case 262: {
            return linux_newfstatat(
                (int)linux_frame->a0, (const char *)linux_frame->a1,
                (linux_x86_stat_t *)linux_frame->a2,
                (int)linux_frame->a3);
        }
        case 264: {
            char old_path[VFS_MAX_NAME];
            char new_path[VFS_MAX_NAME];
            int resolved = linux_resolve_at_path(
                (int)linux_frame->a0, (const char *)linux_frame->a1,
                old_path);
            if (resolved < 0) return (uint64_t)(int64_t)resolved;
            resolved = linux_resolve_at_path(
                (int)linux_frame->a2, (const char *)linux_frame->a3,
                new_path);
            if (resolved < 0) return (uint64_t)(int64_t)resolved;
            return (uint64_t)linux_native_call(
                HBOS_SYS_RENAME, (uint64_t)old_path, (uint64_t)new_path,
                0, 0, 0, 0);
        }
        case 266: {
            char link_path[VFS_MAX_NAME];
            int resolved = linux_resolve_at_path(
                (int)linux_frame->a1, (const char *)linux_frame->a2,
                link_path);
            if (resolved < 0) return (uint64_t)(int64_t)resolved;
            return (uint64_t)linux_native_call(
                HBOS_SYS_SYMLINK, linux_frame->a0, (uint64_t)link_path,
                0, 0, 0, 0);
        }
        case 267: {
            char path[VFS_MAX_NAME];
            int resolved = linux_resolve_at_path(
                (int)linux_frame->a0, (const char *)linux_frame->a1, path);
            if (resolved < 0) return (uint64_t)(int64_t)resolved;
            return (uint64_t)linux_native_call(
                HBOS_SYS_READLINK, (uint64_t)path, linux_frame->a2,
                linux_frame->a3, 0, 0, 0);
        }
        case 273: native.nr = HBOS_SYS_SET_ROBUST_LIST; break;
        case 274: native.nr = HBOS_SYS_GET_ROBUST_LIST; break;
        case 281: native.nr = HBOS_SYS_EPOLL_WAIT; break;
        case 288: {
            int flags = (int)linux_frame->a3;
            if (flags & ~(0x800 | 0x80000))
                return (uint64_t)(-EINVAL);
            int64_t accepted = linux_native_call(
                HBOS_SYS_ACCEPT, linux_frame->a0, linux_frame->a1,
                linux_frame->a2, 0, 0, 0);
            if (accepted < 0) return (uint64_t)accepted;
            task_t *current = task_current();
            if (current && current->fd_table &&
                accepted < POSIX_MAX_FDS) {
                current->fd_table->entries[accepted].flags |= flags;
            }
            return (uint64_t)accepted;
        }
        case 290: native.nr = HBOS_SYS_EVENTFD2; break;
        case 291: native.nr = HBOS_SYS_EPOLL_CREATE1; break;
        case 292:
            if (linux_frame->a0 == linux_frame->a1 ||
                (linux_frame->a2 & ~O_CLOEXEC))
                return (uint64_t)(-EINVAL);
            native.nr = HBOS_SYS_DUP2;
            break;
        case 293: native.nr = HBOS_SYS_PIPE2; break;
        case 294: {
            int result = linux_compat_inotify_init1((int)linux_frame->a0);
            return result < 0 ? (uint64_t)(-(errno > 0 ? errno : EIO)) :
                                (uint64_t)result;
        }
        case 302:
            return linux_prlimit64(
                (int)linux_frame->a0, (unsigned int)linux_frame->a1,
                (const linux_x86_rlimit64_t *)linux_frame->a2,
                (linux_x86_rlimit64_t *)linux_frame->a3);
        case 316: {
            if (linux_frame->a4 & ~1ULL)
                return (uint64_t)(-EOPNOTSUPP);
            char old_path[VFS_MAX_NAME];
            char new_path[VFS_MAX_NAME];
            int resolved = linux_resolve_at_path(
                (int)linux_frame->a0, (const char *)linux_frame->a1,
                old_path);
            if (resolved < 0) return (uint64_t)(int64_t)resolved;
            resolved = linux_resolve_at_path(
                (int)linux_frame->a2, (const char *)linux_frame->a3,
                new_path);
            if (resolved < 0) return (uint64_t)(int64_t)resolved;
            return (uint64_t)linux_native_call(
                HBOS_SYS_RENAME, (uint64_t)old_path, (uint64_t)new_path,
                linux_frame->a4, 0, 0, 0);
        }
        case 318: native.nr = HBOS_SYS_GETRANDOM; break;
        case 319:
            return (uint64_t)(int64_t)linux_compat_memfd_create(
                (const char *)linux_frame->a0,
                (unsigned int)linux_frame->a1);
        /* ── C3：Chromium/glib/NSS 启动路径上常见但此前缺失的调用 ─────
         * 这些要么是"查询能力"，要么是"无实际语义的加固操作"。在
         * --no-sandbox 的单进程场景里，返回合理的兼容值比返回 -ENOSYS
         * 更接近 Linux 行为（后者会让 glib 直接 abort）。 */

        /* 文件锁：HBOS 单用户、无并发写者，直接成功 */
        case 73: native.nr = HBOS_SYS_SYNC; break;   /* flock: 走同步后返回 0 */
        case 74:
        case 75: return 0;                            /* fsync / fdatasync */

        case 303:                                     /* name_to_handle_at */
        case 304:                                     /* open_by_handle_at */
            return (uint64_t)(-EOPNOTSUPP);
        case 85: {                                    /* creat(path, mode) */
            char path[VFS_MAX_NAME];
            if (linux_resolve_at_path(-100, (const char *)linux_frame->a0,
                                      path) < 0)
                return (uint64_t)(-EFAULT);
            return (uint64_t)linux_native_call(
                HBOS_SYS_OPEN, (uint64_t)path,
                O_CREAT | O_WRONLY | O_TRUNC, linux_frame->a1, 0, 0, 0);
        }
        case 140:                                     /* getpriority */
        case 141:                                     /* setpriority */
            return 0;

        case 97: {                                    /* getrlimit */
            linux_x86_rlimit64_t *out =
                (linux_x86_rlimit64_t *)linux_frame->a1;
            return linux_prlimit64(0, (unsigned int)linux_frame->a0,
                                   NULL, out);
        }
        case 160:                                     /* setrlimit */
            return linux_prlimit64(0, (unsigned int)linux_frame->a0,
                                   (const linux_x86_rlimit64_t *)
                                       linux_frame->a1, NULL);

        /* 能力集：没有 capability 模型，报告"零能力" */
        case 125: {                                   /* capget */
            struct cap_header {
                uint32_t version;
                int32_t pid;
            } *header = (struct cap_header *)(uintptr_t)linux_frame->a0;
            uint32_t *data = (uint32_t *)(uintptr_t)linux_frame->a1;
            if (!header) return (uint64_t)(-EFAULT);
            if (header->version != 0x20080522) {      /* _LINUX_CAPABILITY_VERSION_3 */
                header->version = 0x20080522;
                return (uint64_t)(-EINVAL);
            }
            if (!data) return (uint64_t)(-EFAULT);
            for (int i = 0; i < 6; i++) data[i] = 0;  /* 2 × cap_data */
            return 0;
        }
        case 126:                                     /* capset */
            return 0;
        case 117:                                     /* setresuid */
        case 119:                                     /* setresgid */
            return 0;
        case 118: {                                   /* getresuid */
            uint32_t *ruid = (uint32_t *)(uintptr_t)linux_frame->a0;
            uint32_t *euid = (uint32_t *)(uintptr_t)linux_frame->a1;
            uint32_t *suid = (uint32_t *)(uintptr_t)linux_frame->a2;
            if (ruid) *ruid = 0;
            if (euid) *euid = 0;
            if (suid) *suid = 0;
            return 0;
        }
        case 120: {                                   /* getresgid */
            uint32_t *rgid = (uint32_t *)(uintptr_t)linux_frame->a0;
            uint32_t *egid = (uint32_t *)(uintptr_t)linux_frame->a1;
            uint32_t *sgid = (uint32_t *)(uintptr_t)linux_frame->a2;
            if (rgid) *rgid = 0;
            if (egid) *egid = 0;
            if (sgid) *sgid = 0;
            return 0;
        }
        /* 进程组 / 会话：crashpad 起子进程后立刻 setsid()，zygote 也依赖
         * 它把子进程从父会话摘出去。HBOS 没有真正的会话模型，返回一个
         * 稳定的标识即可（调用方只用返回值判错）。 */
        case 109: return 0;                           /* setpgid */
        case 111: return (uint64_t)task_get_id();     /* getpgrp */
        case 112: return (uint64_t)task_get_id();     /* setsid */
        case 122: return (uint64_t)(uint32_t)linux_frame->a0;  /* setfsuid */
        case 123: return (uint64_t)(uint32_t)linux_frame->a0;  /* setfsgid */

        /* seccomp 在 --no-sandbox 下没有过滤器要装；返回成功比 ENOSYS
         * 更安全（调用方通常不检查返回值，但 glib 会记录错误）。 */
        case 317: return 0;

        case 324: return 0;                           /* membarrier: 单核等价 no-op */

        /* sched_* 一族：HBOS 无调度策略/优先级概念，返回"成功 + 默认值"。
         * 返回 ENOSYS 会让 glib 的 thread priority 设置直接失败，进而
         * 在初始化线程池时 CHECK。 */
        case 143: return 0;                           /* sched_setparam */
        case 144: {                                   /* sched_getparam */
            uint32_t *param = (uint32_t *)(uintptr_t)linux_frame->a1;
            if (!param) return (uint64_t)(-EFAULT);
            *param = 0;                               /* sched_priority = 0 */
            return 0;
        }
        case 145: return 0;                           /* sched_setscheduler */
        case 146: return 0;                           /* sched_getscheduler -> SCHED_OTHER */
        case 147: return 0;                           /* sched_get_priority_max */
        case 148: return 0;                           /* sched_get_priority_min */
        case 229: return 0;                           /* sched_getattr */
        case 203: return 0;                           /* sched_setaffinity */
        case 204: {                                   /* sched_getaffinity */
            size_t size = (size_t)linux_frame->a1;
            uint8_t *mask = (uint8_t *)(uintptr_t)linux_frame->a2;
            if (!mask || size == 0) return (uint64_t)(-EINVAL);
            if (size > 128) size = 128;
            memset(mask, 0, size);
            mask[0] = 0x01;                           /* 只有一个 CPU0 */
            return 8;
        }

        case 137:                                     /* statfs */
        case 138: {                                   /* fstatfs */
            /* 两个调用的缓冲区都在 a1：statfs(path, buf) / fstatfs(fd, buf) */
            linux_x86_statfs_t *out =
                (linux_x86_statfs_t *)(uintptr_t)linux_frame->a1;
            if (!out) return (uint64_t)(-EFAULT);
            memset(out, 0, sizeof(*out));
            uint32_t capacity = fs_capacity_bytes();
            out->f_type = 0xEF53;                     /* EXT2_SUPER_MAGIC */
            out->f_bsize = 4096;
            out->f_frsize = 4096;
            out->f_blocks = capacity / 4096;
            out->f_bfree = (capacity / 4096) / 2;
            out->f_bavail = out->f_bfree;
            out->f_files = MAX_FILES;
            out->f_ffree = MAX_FILES / 2;
            out->f_namelen = MAX_FILENAME - 1;
            return 0;
        }

        case 263: {                                   /* unlinkat */
            char path[VFS_MAX_NAME];
            int resolved = linux_resolve_at_path(
                (int)linux_frame->a0, (const char *)linux_frame->a1, path);
            if (resolved < 0) return (uint64_t)(int64_t)resolved;
            uint64_t flags = linux_frame->a2;
            if (flags & ~(uint64_t)0x200)
                return (uint64_t)(-EINVAL);
            return (uint64_t)linux_native_call(
                flags & 0x200 ? HBOS_SYS_RMDIR : HBOS_SYS_UNLINK,
                (uint64_t)path, 0, 0, 0, 0, 0);
        }
        case 269: {                                   /* faccessat */
            char path[VFS_MAX_NAME];
            int resolved = linux_resolve_at_path(
                (int)linux_frame->a0, (const char *)linux_frame->a1, path);
            if (resolved < 0) return (uint64_t)(int64_t)resolved;
            return (uint64_t)linux_native_call(
                HBOS_SYS_ACCESS, (uint64_t)path, linux_frame->a2,
                linux_frame->a3, 0, 0, 0);
        }

        case 441: {                                   /* epoll_pwait2 */
            const linux_x86_timespec_t *timeout =
                (const linux_x86_timespec_t *)(uintptr_t)linux_frame->a3;
            int milliseconds = -1;
            if (timeout) {
                int64_t seconds = timeout->seconds;
                int64_t nanos = timeout->nanoseconds;
                if (seconds > 1000000) milliseconds = 1000000000;
                else {
                    milliseconds = (int)(seconds * 1000 +
                                         nanos / 1000000);
                    if (milliseconds < 0) milliseconds = 0;
                }
            }
            native.nr = HBOS_SYS_EPOLL_WAIT;
            native.a3 = (uint64_t)(int64_t)milliseconds;
            break;
        }

        case 435:
            return linux_clone3(
                (linux_x86_syscall_return_frame_t *)linux_frame,
                (const void *)linux_frame->a0, linux_frame->a1);
        default:
            linux_trace_unimplemented(linux_frame->nr, linux_frame);
            return (uint64_t)(-ENOSYS);
    }
    return syscall_dispatch_frame(&native);
}

static uint64_t align_page_up(uint64_t value) {
    return (value + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

static void vm_area_drop_backing(vm_area_t *area) {
    if (area && area->backing_type == VM_BACKING_MEMFD)
        linux_compat_memfd_unmap(area->backing_id);
}

/* Remove every VMA fragment intersecting [start,end), splitting a VMA when
 * the hole is strictly inside it.  Linux munmap accepts already-unmapped
 * pages, so absence of metadata is not an error. */
static int vm_unmap_area_range(task_mm_t *mm, uint64_t start, uint64_t end) {
    if (!mm || start >= end) return 0;

    size_t splits = 0;
    for (vm_area_t *area = mm->areas; area; area = area->next)
        if (start > area->start && end < area->end)
            splits++;

    vm_area_t *spares = NULL;
    while (splits--) {
        vm_area_t *spare = (vm_area_t *)kmalloc(sizeof(*spare));
        if (!spare) {
            while (spares) {
                vm_area_t *next = spares->next;
                kfree(spares);
                spares = next;
            }
            return -ENOMEM;
        }
        spare->next = spares;
        spares = spare;
    }

    vm_area_t **link = &mm->areas;
    while (*link) {
        vm_area_t *area = *link;
        if (end <= area->start || start >= area->end) {
            link = &area->next;
            continue;
        }
        uint64_t cut_start = start > area->start ? start : area->start;
        uint64_t cut_end = end < area->end ? end : area->end;
        for (uint64_t page = cut_start; page < cut_end; page += PAGE_SIZE)
            vmm_release_page(page);

        if (cut_start == area->start && cut_end == area->end) {
            *link = area->next;
            vm_area_drop_backing(area);
            kfree(area);
        } else if (cut_start == area->start) {
            area->backing_offset += cut_end - area->start;
            area->start = cut_end;
            link = &area->next;
        } else if (cut_end == area->end) {
            area->end = cut_start;
            link = &area->next;
        } else {
            vm_area_t *right = spares;
            spares = spares->next;
            *right = *area;
            right->start = cut_end;
            right->backing_offset += cut_end - area->start;
            right->next = area->next;
            if (right->backing_type == VM_BACKING_MEMFD &&
                linux_compat_memfd_retain_map(right->backing_id) < 0) {
                kfree(right);
                while (spares) {
                    vm_area_t *next = spares->next;
                    kfree(spares);
                    spares = next;
                }
                return -EINVAL;
            }
            area->end = cut_start;
            area->next = right;
            link = &right->next;
        }
    }
    while (spares) {
        vm_area_t *next = spares->next;
        kfree(spares);
        spares = next;
    }
    return 0;
}

static int vm_allocate_owned_pages(uint64_t address, size_t pages) {
    size_t mapped = 0;
    for (; mapped < pages; mapped++) {
        uint64_t page = address + mapped * PAGE_SIZE;
        if (!vmm_alloc_page_at(page, VMM_P | VMM_U | VMM_W)) break;
        memset((void *)(uintptr_t)page, 0, PAGE_SIZE);
    }
    if (mapped == pages) return 0;
    while (mapped)
        vmm_release_page(address + --mapped * PAGE_SIZE);
    return -ENOMEM;
}

static int vm_fill_vfs_snapshot(vfs_node_t *node, uint64_t address,
                                size_t length, uint64_t file_offset) {
    if (!node || node->type != VFS_NODE_FILE) return -ENODEV;
    size_t copied = 0;
    while (copied < length && file_offset + copied < node->size) {
        size_t chunk = length - copied;
        uint64_t available = node->size - (file_offset + copied);
        if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;
        if ((uint64_t)chunk > available) chunk = (size_t)available;
        int got = vfs_read(node, (uint32_t)(file_offset + copied),
                           (void *)(uintptr_t)(address + copied),
                           (uint32_t)chunk);
        if (got < 0) return -EIO;
        if ((size_t)got != chunk) return -EIO;
        copied += chunk;
    }
    return 0;
}

static int vm_fill_memfd_snapshot(int fd, uint64_t address,
                                  size_t length, uint64_t file_offset,
                                  uint64_t file_size) {
    size_t copy = length;
    if (file_offset >= file_size) copy = 0;
    else if ((uint64_t)copy > file_size - file_offset)
        copy = (size_t)(file_size - file_offset);
    return copy && linux_compat_memfd_read_at(
                       fd, file_offset, (void *)(uintptr_t)address, copy) < 0
        ? -(errno > 0 ? errno : EIO) : 0;
}

/* mprotect 同时承担"提交"语义：Linux 上对 PROT_NONE 预留区做
 * mprotect(RW) 只是改权限，等第一次写才分配物理页；HBOS 没有按需缺页
 * 分配，于是在这里一次性补齐该范围内的页。VMA 记录了类型，所以匿名页
 * 清零、文件页按 backing_offset 回填，语义与 mmap 时立即建页一致。 */
static int commit_and_protect_user_range(task_mm_t *mm, uint64_t address,
                                         size_t length, int protection) {
    if (!mm) return -ESRCH;
    if (!length || (address & (PAGE_SIZE - 1)) || (protection & ~0x07) ||
        address >= 0x0000800000000000ULL ||
        length > 0x0000800000000000ULL - address)
        return -EINVAL;

    uint64_t end = address + length;
    for (uint64_t page = address; page < end; page += PAGE_SIZE) {
        if (vmm_get_phys(page)) continue;
        vm_area_t *area = NULL;
        for (vm_area_t *candidate = mm->areas; candidate;
             candidate = candidate->next) {
            if (page >= candidate->start && page < candidate->end) {
                area = candidate;
                break;
            }
        }
        if (!area || area->backing_type == VM_BACKING_MEMFD)
            return -ENOMEM;
        if (!vmm_alloc_page_at(page, VMM_P | VMM_U | VMM_W))
            return -ENOMEM;
        memset((void *)(uintptr_t)page, 0, PAGE_SIZE);
        if ((area->backing_type == VM_BACKING_VFS_PRIVATE ||
             area->backing_type == VM_BACKING_VFS_SHARED) &&
            area->backing_node) {
            uint64_t file_offset =
                area->backing_offset + (page - area->start);
            if (vm_fill_vfs_snapshot(area->backing_node, page, PAGE_SIZE,
                                     file_offset) < 0)
                return -EIO;
        }
    }
    return protect_user_range(address, length, protection);
}

static int map_user_heap_growth(uint64_t old_brk, uint64_t new_brk) {
    if (new_brk <= old_brk) return 0;
    if (new_brk > UINT64_MAX - (PAGE_SIZE - 1)) return -1;

    uint64_t va_start = old_brk & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t va_end = align_page_up(new_brk);
    uint64_t first_allocated = 0;
    for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE) {
        if (!vmm_get_phys(va)) {
            if (!first_allocated) first_allocated = va;
        } else if (first_allocated) {
            /* A foreign mapping punched into the future heap would make OOM
             * rollback ambiguous. Reject it before changing any PTE. */
            return -1;
        }
    }
    if (!first_allocated) return 0;
    for (uint64_t va = first_allocated; va < va_end; va += PAGE_SIZE) {
        if (!vmm_alloc_page_at(va, VMM_P | VMM_W | VMM_U)) {
            for (uint64_t rollback = first_allocated;
                 rollback < va; rollback += PAGE_SIZE)
                vmm_release_page(rollback);
            return -1;
        }
    }
    return 0;
}

static void unmap_user_heap_shrink(uint64_t old_brk, uint64_t new_brk) {
    if (new_brk >= old_brk) return;
    uint64_t release_start = align_page_up(new_brk);
    uint64_t release_end = align_page_up(old_brk);
    for (uint64_t va = release_start; va < release_end; va += PAGE_SIZE)
        vmm_release_page(va);
}

static uint64_t user_sbrk(intptr_t increment) {
    task_t *cur = task_current();
    task_mm_t *mm = cur ? cur->mm : NULL;
    if (!mm || !mm->user_heap_start ||
        mm->user_heap_limit <= mm->user_heap_start)
        return (uint64_t)(-ENOMEM);

    uint64_t old_brk = mm->user_brk ? mm->user_brk : mm->user_heap_start;
    uint64_t new_brk = old_brk;

    if (increment > 0) {
        uint64_t inc = (uint64_t)increment;
        if (inc > mm->user_heap_limit - old_brk) return (uint64_t)(-ENOMEM);
        new_brk = old_brk + inc;
    } else if (increment < 0) {
        uint64_t dec = (uint64_t)(-(increment + 1)) + 1;
        if (dec > old_brk - mm->user_heap_start) return (uint64_t)(-ENOMEM);
        new_brk = old_brk - dec;
    }

    if (map_user_heap_growth(old_brk, new_brk) != 0)
        return (uint64_t)(-ENOMEM);
    unmap_user_heap_shrink(old_brk, new_brk);
    mm->user_brk = new_brk;
    return old_brk;
}

static uint64_t user_brk(uint64_t new_brk) {
    task_t *cur = task_current();
    task_mm_t *mm = cur ? cur->mm : NULL;
    if (!mm || !mm->user_heap_start ||
        mm->user_heap_limit <= mm->user_heap_start)
        return (uint64_t)(-ENOMEM);
    if (!new_brk) return mm->user_brk ? mm->user_brk : mm->user_heap_start;
    if (new_brk < mm->user_heap_start || new_brk > mm->user_heap_limit)
        return (uint64_t)(-ENOMEM);

    uint64_t old_brk = mm->user_brk ? mm->user_brk : mm->user_heap_start;
    if (map_user_heap_growth(old_brk, new_brk) != 0)
        return (uint64_t)(-ENOMEM);
    unmap_user_heap_shrink(old_brk, new_brk);
    mm->user_brk = new_brk;
    return new_brk;
}

/**
 * 系统调用主分发函数
 * 由 interrupt_asm.asm 中的 syscall_int80_stub 调用
 *
 * @param f  系统调用帧（包含调用号和 6 个参数）
 * @return 系统调用返回值
 */
uint64_t syscall_dispatch_frame(hbos_syscall_frame_t *f) {
    if (!f) return (uint64_t)(-EFAULT);

    switch (f->nr) {
        // ============================================================
        // 文件 I/O (0-11)
        // ============================================================
        case HBOS_SYS_READ:
            if (task_current() && (int)f->a0 >= 0 &&
                (int)f->a0 < POSIX_MAX_FDS &&
                task_current()->fd_table->entries[(int)f->a0].used &&
                (task_current()->fd_table->entries[(int)f->a0].type == FD_EVENT ||
                 task_current()->fd_table->entries[(int)f->a0].type == FD_UNIX ||
                 task_current()->fd_table->entries[(int)f->a0].type == FD_MEMFD ||
                 task_current()->fd_table->entries[(int)f->a0].type == FD_INOTIFY ||
                 task_current()->fd_table->entries[(int)f->a0].type == FD_TIMER ||
                 task_current()->fd_table->entries[(int)f->a0].type == FD_SIGNALFD))
                return (uint64_t)linux_compat_read(
                    (int)f->a0, (void *)f->a1, (size_t)f->a2);
            return finish_syscall((long)read((int)f->a0, (void *)f->a1, (size_t)f->a2));

        case HBOS_SYS_WRITE:
            if (task_current() && (int)f->a0 >= 0 &&
                (int)f->a0 < POSIX_MAX_FDS &&
                task_current()->fd_table->entries[(int)f->a0].used &&
                (task_current()->fd_table->entries[(int)f->a0].type == FD_EVENT ||
                 task_current()->fd_table->entries[(int)f->a0].type == FD_UNIX ||
                 task_current()->fd_table->entries[(int)f->a0].type == FD_MEMFD ||
                 task_current()->fd_table->entries[(int)f->a0].type == FD_INOTIFY))
                return (uint64_t)linux_compat_write(
                    (int)f->a0, (const void *)f->a1, (size_t)f->a2);
            return finish_syscall((long)write((int)f->a0, (const void *)f->a1, (size_t)f->a2));

        case HBOS_SYS_OPEN:
            return finish_syscall((long)open((const char *)f->a0, (int)f->a1, (int)f->a2));

        case HBOS_SYS_CLOSE:
            (void)linux_compat_close((int)f->a0);
            return finish_syscall((long)close((int)f->a0));

        case HBOS_SYS_LSEEK: {
            int fd = (int)f->a0;
            task_t *current = task_current();
            if (current && current->fd_table && fd >= 0 &&
                fd < POSIX_MAX_FDS &&
                current->fd_table->entries[fd].used &&
                current->fd_table->entries[fd].type == FD_INOTIFY)
                return (uint64_t)(-ESPIPE);
            if (current && current->fd_table && fd >= 0 &&
                fd < POSIX_MAX_FDS &&
                current->fd_table->entries[fd].used &&
                current->fd_table->entries[fd].type == FD_MEMFD) {
                uint64_t size;
                if (linux_compat_memfd_size(fd, &size) < 0)
                    return (uint64_t)(-EBADF);
                fd_entry_t *entry = &current->fd_table->entries[fd];
                int64_t base = (int)f->a2 == SEEK_SET ? 0 :
                    ((int)f->a2 == SEEK_CUR ? entry->offset :
                     ((int)f->a2 == SEEK_END ? (int64_t)size : -1));
                if (base < 0) return (uint64_t)(-EINVAL);
                int64_t next = base + (int64_t)f->a1;
                if (next < 0 || (uint64_t)next > size)
                    return (uint64_t)(-EINVAL);
                entry->offset = (uint32_t)next;
                return (uint64_t)next;
            }
            return finish_syscall((long)lseek((int)f->a0, (off_t)f->a1, (int)f->a2));
        }

        case HBOS_SYS_FSTAT: {
            int fd = (int)f->a0;
            task_t *current = task_current();
            if (current && current->fd_table && fd >= 0 &&
                fd < POSIX_MAX_FDS &&
                current->fd_table->entries[fd].used &&
                current->fd_table->entries[fd].type == FD_INOTIFY) {
                struct stat *output = (struct stat *)f->a1;
                if (!output) return (uint64_t)(-EFAULT);
                memset(output, 0, sizeof(*output));
                output->st_dev = 4;
                output->st_ino = (ino_t)(
                    current->fd_table->entries[fd].compat_id + 1);
                output->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
                output->st_nlink = 1;
                return 0;
            }
            if (current && current->fd_table && fd >= 0 &&
                fd < POSIX_MAX_FDS &&
                current->fd_table->entries[fd].used &&
                current->fd_table->entries[fd].type == FD_MEMFD) {
                struct stat *output = (struct stat *)f->a1;
                uint64_t size;
                if (!output) return (uint64_t)(-EFAULT);
                if (linux_compat_memfd_size(fd, &size) < 0)
                    return (uint64_t)(-EBADF);
                memset(output, 0, sizeof(*output));
                output->st_dev = 3;
                output->st_ino = (ino_t)(current->fd_table->entries[fd].compat_id + 1);
                output->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
                output->st_nlink = 1;
                output->st_size = (off_t)size;
                return 0;
            }
            return finish_syscall((long)fstat((int)f->a0, (struct stat *)f->a1));
        }

        case HBOS_SYS_STAT:
            return finish_syscall((long)(f->a2 ?
                lstat((const char *)f->a0, (struct stat *)f->a1) :
                stat((const char *)f->a0, (struct stat *)f->a1)));

        case HBOS_SYS_UNLINK:
            return finish_syscall((long)unlink((const char *)f->a0));

        case HBOS_SYS_ISATTY:
            return finish_syscall((long)isatty((int)f->a0));

        case HBOS_SYS_GETPID:
            return (uint64_t)getpid();

        case HBOS_SYS_SBRK:
            return user_sbrk((intptr_t)f->a0);

        case HBOS_SYS_EXIT: {
            // 实际终止当前任务
            int status = (int)f->a0;
            task_set_exit_status(status);
            task_exit();
            return 0;  // 不会到达这里
        }

        // ============================================================
        // 进程控制 (12-14)
        // ============================================================
        case HBOS_SYS_GETPPID:
            return (uint64_t)getppid();

        case HBOS_SYS_SLEEP:
            return finish_syscall((long)sleep((unsigned int)f->a0));

        case HBOS_SYS_USLEEP:
            return finish_syscall((long)usleep((useconds_t)f->a0));

        // ============================================================
        // 系统信息 (15-16)
        // ============================================================
        case HBOS_SYS_UNAME: {
            // uname: 返回系统信息到 utsname 结构
            // 结构定义在 sys/utsname.h 中
            struct utsname {
                char sysname[65];
                char nodename[65];
                char release[65];
                char version[65];
                char machine[65];
            };
            struct utsname *buf = (struct utsname *)f->a0;
            if (!buf) return (uint64_t)(-EFAULT);
            memset(buf, 0, sizeof(struct utsname));
            memcpy(buf->sysname, "HBOS", 5);
            memcpy(buf->nodename, "hbos", 5);
            {
                const char *rel = HBOS_VERSION_REL;
                const char *ver = "HBOS " HBOS_VERSION_REL;
                memcpy(buf->release, rel, strlen(rel) + 1);
                memcpy(buf->version, ver, strlen(ver) + 1);
            }
            memcpy(buf->machine, "x86_64", 7);
            return 0;
        }

        case HBOS_SYS_STATFS:
        case HBOS_SYS_FSTATFS: {
            /* Linux x86-64 struct statfs：12 个 long + 4 long spare = 128 字节 */
            long *buf = (long *)f->a1;
            if (!buf) return (uint64_t)(-EFAULT);
            memset(buf, 0, 128);
            buf[0] = 0x01021994L;      /* RAMFS_MAGIC */
            buf[1] = 4096;             /* f_bsize */
            buf[2] = 1;                /* f_blocks */
            buf[3] = 1;                /* f_bfree */
            buf[4] = 1;                /* f_bavail */
            buf[8] = 255;              /* f_namelen */
            return 0;
        }

        case HBOS_SYS_SIGNALFD:
            return finish_syscall(linux_compat_signalfd(
                (int)f->a0, (const void *)f->a1, (int)f->a3));

        case HBOS_SYS_SCHED_GETAFFINITY: {
            int pid = (int)f->a0;
            size_t len = (size_t)f->a1;
            unsigned long *mask = (unsigned long *)f->a2;
            if (!mask || !len) return (uint64_t)(-EINVAL);
            if (pid != 0) return (uint64_t)(-ESRCH);
            extern int smp_cpu_count(void);
            int ncpu = smp_cpu_count();
            if (ncpu < 1) ncpu = 1;
            memset(mask, 0, len);
            for (int i = 0; i < ncpu; i++) {
                if ((size_t)(i / (int)(sizeof(long) * 8)) * sizeof(long) < len)
                    mask[i / (int)(sizeof(long) * 8)] |=
                        1UL << (i % (int)(sizeof(long) * 8));
            }
            return (uint64_t)(len < (size_t)sizeof(long) * 8 ? len : (size_t)sizeof(long) * 8);
        }

        case HBOS_SYS_GETTOD: {
            struct timeval {
                uint64_t tv_sec;
                uint64_t tv_usec;
            };
            struct timeval *tv = (struct timeval *)f->a0;
            if (!tv) return (uint64_t)(-EFAULT);
            linux_x86_timespec_t now;
            int result = linux_clock_now(0, &now);
            if (result < 0) return (uint64_t)(int64_t)result;
            tv->tv_sec = (uint64_t)now.seconds;
            tv->tv_usec = (uint64_t)now.nanoseconds / 1000ULL;
            return 0;
        }

        // ============================================================
        // 文件系统扩展 (17-22)
        // ============================================================
        case HBOS_SYS_ACCESS: {
            return finish_syscall((long)access((const char *)f->a0, (int)f->a1));
        }

        case HBOS_SYS_FTRUNCATE: {
            int fd = (int)f->a0;
            task_t *current = task_current();
            if (current && current->fd_table && fd >= 0 &&
                fd < POSIX_MAX_FDS &&
                current->fd_table->entries[fd].used &&
                current->fd_table->entries[fd].type == FD_MEMFD) {
                int result = linux_compat_memfd_truncate(fd, f->a1);
                return result < 0
                    ? (uint64_t)(-(errno > 0 ? errno : EINVAL)) : 0;
            }
            return finish_syscall((long)ftruncate((int)f->a0, (off_t)f->a1));
        }

        case HBOS_SYS_MKDIR: {
            return finish_syscall((long)mkdir((const char *)f->a0, (mode_t)f->a1));
        }

        case HBOS_SYS_RMDIR: {
            return finish_syscall((long)rmdir((const char *)f->a0));
        }

        case HBOS_SYS_GETCWD: {
            char *buf = (char *)f->a0;
            size_t size = (size_t)f->a1;
            char *ret = getcwd(buf, size);
            if (!ret) return (uint64_t)(-(int64_t)errno);
            return (uint64_t)strlen(ret);
        }

        case HBOS_SYS_CHDIR: {
            return finish_syscall((long)chdir((const char *)f->a0));
        }

        // ============================================================
        // 文件描述符操作和时间 (23-26)
        // ============================================================
        case HBOS_SYS_DUP: {
            int oldfd = (int)f->a0;
            if (oldfd < 0 || oldfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table->entries[oldfd].used)
                return (uint64_t)(-EBADF);
            int newfd = -1;
            for (int i = 0; i < POSIX_MAX_FDS; i++) {
                if (!cur->fd_table->entries[i].used) { newfd = i; break; }
            }
            if (newfd < 0) return (uint64_t)(-EMFILE);
            cur->fd_table->entries[newfd] = cur->fd_table->entries[oldfd];
            linux_compat_retain(newfd);
            return (uint64_t)newfd;
        }

        case HBOS_SYS_GETEUID:
            return (uint64_t)geteuid();

        case HBOS_SYS_GETEGID:
            return (uint64_t)getegid();

        case HBOS_SYS_GETTID:
            // gettid: 返回线程 ID（当前 = 任务 ID）
            return (uint64_t)task_get_id();

        // ============================================================
        // 文件描述符操作扩展 (27-31)
        // ============================================================
        case HBOS_SYS_DUP2: {
            int oldfd = (int)f->a0;
            int newfd = (int)f->a1;
            if (oldfd < 0 || newfd < 0 || oldfd >= POSIX_MAX_FDS || newfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table->entries[oldfd].used)
                return (uint64_t)(-EBADF);
            if (oldfd == newfd) return (uint64_t)newfd;
            if (cur->fd_table->entries[newfd].used) {
                (void)linux_compat_close(newfd);
                close(newfd);
            }
            cur->fd_table->entries[newfd] = cur->fd_table->entries[oldfd];
            linux_compat_retain(newfd);
            return (uint64_t)newfd;
        }

        case HBOS_SYS_PIPE: {
            int *pipefd = (int *)f->a0;
            if (!pipefd) return (uint64_t)(-EFAULT);
            extern int pipe(int pipefd[2]);
            int ret = pipe(pipefd);
            if (ret < 0) return (uint64_t)(-errno);
            return 0;
        }

        case HBOS_SYS_FCNTL: {
            int fd = (int)f->a0;
            int cmd = (int)f->a1;
            long arg = (long)f->a2;
            if (fd < 0 || fd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table->entries[fd].used)
                return (uint64_t)(-EBADF);
            switch (cmd) {
                case 0:   return cur->fd_table->entries[fd].flags;
                case 1:   cur->fd_table->entries[fd].flags = (int)arg; return 0;
                case 2:   cur->fd_table->entries[fd].flags |= (int)arg; return 0;
                case 3:   cur->fd_table->entries[fd].flags &= ~(int)arg; return 0;
                default:  return (uint64_t)(-EINVAL);
            }
        }

        case HBOS_SYS_IOCTL: {
            int fd = (int)f->a0;
            if (fd < 0 || fd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table->entries[fd].used)
                return (uint64_t)(-EBADF);
            return (uint64_t)(-ENOTTY);
        }

        case HBOS_SYS_READLINK: {
            return finish_syscall((long)readlink(
                (const char *)f->a0, (char *)f->a1, (size_t)f->a2));
        }

        // ============================================================
        // 进程管理扩展 (32-38)
        // ============================================================
        case HBOS_SYS_FORK: {
            int pid = task_fork();
            if (pid < 0) return (uint64_t)(-EAGAIN);
            return (uint64_t)pid;
        }

        case HBOS_SYS_EXECVE: {
            const char *path = (const char *)f->a0;
            char *const *argv = (char *const *)f->a1;
            char *const *envp = (char *const *)f->a2;
            if (!path) return (uint64_t)(-EFAULT);
            vfs_node_t *node = vfs_lookup(path);
            if (!node) return (uint64_t)(-ENOENT);
            if (node->type != VFS_NODE_FILE) return (uint64_t)(-EACCES);
            int ret = elf64_load_vfs_and_exec(node, argv, envp);
            if (ret < 0) return (uint64_t)(-ENOEXEC);
            return 0;
        }

        case HBOS_SYS_WAITPID: {
            pid_t pid = (pid_t)f->a0;
            int *status = (int *)f->a1;
            int options = (int)f->a2;
            if (pid <= 0) return (uint64_t)(-ECHILD);
            int st = 0;
            int ret = task_wait((uint32_t)pid, &st);
            if (ret < 0) return (uint64_t)(-ECHILD);
            if (options & WNOHANG) {
                const task_t *t = task_get_by_id((uint32_t)pid);
                if (t && t->state != TASK_TERMINATED) return 0;
            }
            if (status)
                *status = W_EXITCODE(st, 0);
            return (uint64_t)pid;
        }

        case HBOS_SYS_KILL: {
            pid_t pid = (pid_t)f->a0;
            int sig = (int)f->a1;
            if (pid <= 0) return (uint64_t)(-ESRCH);
            if (task_kill((uint32_t)pid, sig) < 0)
                return (uint64_t)(-ESRCH);
            return 0;
        }

        case HBOS_SYS_GETUID:
            return (uint64_t)getuid();

        case HBOS_SYS_GETGID:
            return (uint64_t)getgid();

        case HBOS_SYS_SETUID:
            return finish_syscall((long)setuid((uid_t)f->a0));

        // ============================================================
        // 信号处理 (39-42)
        // ============================================================
        case HBOS_SYS_SIGNAL: {
            int sig = (int)f->a0;
            void (*handler)(int) = (void (*)(int))f->a1;
            if (sig <= 0 || sig >= _NSIG) {
                errno = EINVAL;
                return (uint64_t)(-EINVAL);
            }
            task_t *cur = task_current();
            if (!cur) return (uint64_t)(-ESRCH);
            void (*old)(int) = cur->sig_handler[sig];
            cur->sig_handler[sig] = handler;
            return (uint64_t)(uintptr_t)old;
        }

        case HBOS_SYS_SIGACTION: {
            int sig = (int)f->a0;
            void (*handler)(int) = (void (*)(int))f->a1;
            if (sig <= 0 || sig >= _NSIG) {
                errno = EINVAL;
                return (uint64_t)(-EINVAL);
            }
            task_t *cur = task_current();
            if (!cur) return (uint64_t)(-ESRCH);
            void (*old)(int) = cur->sig_handler[sig];
            cur->sig_handler[sig] = handler;
            void **oact = (void **)f->a2;
            if (oact) *oact = (void *)old;
            return 0;
        }

        case HBOS_SYS_SIGPROCMASK: {
            int how = (int)f->a0;
            const sigset_t *set = (const sigset_t *)f->a1;
            sigset_t *old_set = (sigset_t *)f->a2;
            size_t set_size = (size_t)f->a3;
            if (set_size && set_size != sizeof(sigset_t))
                return (uint64_t)(-EINVAL);
            if (how != SIG_BLOCK && how != SIG_UNBLOCK &&
                how != SIG_SETMASK)
                return (uint64_t)(-EINVAL);
            task_t *current = task_current();
            if (!current) return (uint64_t)(-ESRCH);
            if (old_set) *old_set = current->sig_blocked;
            if (set) {
                if (how == SIG_BLOCK)
                    current->sig_blocked.sig[0] |= set->sig[0];
                else if (how == SIG_UNBLOCK)
                    current->sig_blocked.sig[0] &= ~set->sig[0];
                else
                    current->sig_blocked = *set;
                /* SIGKILL and SIGSTOP can never be blocked. */
                current->sig_blocked.sig[0] &=
                    ~((1ULL << (SIGKILL - 1)) |
                      (1ULL << (SIGSTOP - 1)));
            }
            return 0;
        }

        case HBOS_SYS_PAUSE: {
            return (uint64_t)(-EINTR);
        }

        // ============================================================
        // 内存管理 (43-47)
        // ============================================================
        case HBOS_SYS_MMAP: {
            void *addr = (void *)f->a0;
            size_t len = (size_t)f->a1;
            int prot = (int)f->a2;
            int flags = (int)f->a3;
            int fd = (int)f->a4;
            off_t off = (off_t)f->a5;
            if (len == 0) {
                /* 申请 0 字节通常意味着调用方 fstat 拿到的 size 是 0 */
                console_puts("[MMAP] fail len=0 fd=");
                trace_put_udec((uint64_t)(unsigned)fd);
                console_puts(" flags=");
                trace_put_hex((uint64_t)(unsigned)flags);
                console_putchar('\n');
                return (uint64_t)(-EINVAL);
            }
            int mapping_kind = flags & 0x03;
            if (mapping_kind != 0x01 && mapping_kind != 0x02) {
                console_puts("[MMAP] fail bad kind fd=");
                trace_put_udec((uint64_t)(unsigned)fd);
                console_puts(" flags=");
                trace_put_hex((uint64_t)(unsigned)flags);
                console_putchar('\n');
                return (uint64_t)(-EINVAL);
            }
            if (off < 0 || ((uint64_t)off & (PAGE_SIZE - 1))) {
                console_puts("[MMAP] fail bad off fd=");
                trace_put_udec((uint64_t)(unsigned)fd);
                console_puts(" off=");
                trace_put_udec((uint64_t)off);
                console_putchar('\n');
                return (uint64_t)(-EINVAL);
            }
            if (len > UINT64_MAX - (PAGE_SIZE - 1))
                return (uint64_t)(-ENOMEM);
            size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
            uint64_t span = (uint64_t)pages * PAGE_SIZE;
            void *p = addr;
            int fixed = flags & 0x10;
            int fixed_noreplace = flags & 0x100000;
            if ((fixed || fixed_noreplace) &&
                ((uint64_t)p & (PAGE_SIZE - 1)))
                return (uint64_t)(-EINVAL);
            if (p && !fixed && !fixed_noreplace)
                p = (void *)((uint64_t)p & ~(uint64_t)(PAGE_SIZE - 1));
            if (p && ((uint64_t)p >= 0x0000800000000000ULL ||
                      span > 0x0000800000000000ULL - (uint64_t)p))
                return (uint64_t)(-ENOMEM);
            task_t *va_owner = task_current();
            if (p && !fixed && !fixed_noreplace) {
                if (!linux_va_range_free(va_owner, (uint64_t)p, pages))
                    p = NULL;
            }
            if (!p) {
                uint64_t hole = linux_va_find_free(va_owner, pages,
                                                   0x0000200000000000ULL);
                if (hole) p = (void *)hole;
            }
            if (!p) return (uint64_t)(-ENOMEM);
            if (fixed_noreplace) {
                for (size_t i = 0; i < pages; i++) {
                    if (vmm_get_phys((uint64_t)p + i * PAGE_SIZE))
                        return (uint64_t)(-EEXIST);
                }
            }
            task_t *cur = task_current();
            if (!cur || !cur->mm) return (uint64_t)(-ESRCH);
            int anonymous = (flags & 0x20) != 0;
            fd_entry_t *mapping_entry = NULL;
            uint64_t mapping_file_size = 0;

            /* PROT_NONE 是"预留地址空间"，不是"立刻占满物理内存"。
             *
             * Chromium 的 PartitionAlloc 启动时先 mmap(NULL, 32 GiB,
             * PROT_NONE) 划出虚拟地址池，再用 mprotect(PROT_READ|WRITE)
             * 按需提交。Linux 对这种预留不消耗任何物理页；我们原来对
             * 每一页都 vmm_alloc_page_at()，32 GiB 必然 ENOMEM，随后
             * Chromium 的 CHECK 失败 → int3。
             *
             * 这里对 prot==0 只登记 VMA、不碰物理页；真正提交发生在
             * mprotect（见 commit_and_protect_user_range）。 */
            int reserve_only = (prot == 0);
            if (!anonymous) {
                if (fd < 0 || fd >= POSIX_MAX_FDS || !cur->fd_table ||
                    !cur->fd_table->entries[fd].used) {
                    mmap_log_failure(fd, len, prot, flags, (uint64_t)off, 0,
                                     -EBADF);
                    return (uint64_t)(-EBADF);
                }
                mapping_entry = &cur->fd_table->entries[fd];
                if (mapping_entry->type == FD_MEMFD) {
                    if (linux_compat_memfd_size(fd, &mapping_file_size) < 0) {
                        mmap_log_failure(fd, len, prot, flags, (uint64_t)off,
                                         0, -EBADF);
                        return (uint64_t)(-EBADF);
                    }
                } else if (mapping_entry->type == FD_FILE &&
                           mapping_entry->node &&
                           mapping_entry->node->type == VFS_NODE_FILE) {
                    mapping_file_size = mapping_entry->node->size;
                } else {
                    mmap_log_failure(fd, len, prot, flags, (uint64_t)off, 0,
                                     -ENODEV);
                    console_puts("  (fd type=");
                    trace_put_udec((uint64_t)mapping_entry->type);
                    console_puts(")\n");
                    return (uint64_t)(-ENODEV);
                }
                if ((uint64_t)off >= mapping_file_size) {
                    mmap_log_failure(fd, len, prot, flags, (uint64_t)off,
                                     mapping_file_size, -EINVAL);
                    return (uint64_t)(-EINVAL);
                }
                /* 可写 MAP_SHARED 文件映射：以前直接 EOPNOTSUPP，结果
                 * Chromium 的 base::SharedMemory（/dev/shm 下的文件）建不
                 * 出来，visitedlink::VisitedLinkMaster::Init 失败，进而
                 * history::ContentVisitDelegate::Init 返回 false，最后
                 * HistoryService::Init 主动 TerminateCurrentProcessImmediately(0)
                 * ——单进程模式"浏览器初始化完就静默 exit(0)"就是这么来的。
                 * 这里退化成"共享文件的私有快照"：页表是私有的，但同一进程
                 * 内所有使用者看到的是同一份页面，够单进程跑通。真正的
                 * 跨进程可见性/回写以后再做（VMA 已标 VM_BACKING_VFS_SHARED）。 */
            }
            vm_area_t *vma = (vm_area_t *)kmalloc(sizeof(vm_area_t));
            if (!vma) return (uint64_t)(-ENOMEM);
            memset(vma, 0, sizeof(*vma));
            if (fixed) {
                int unmap_result = vm_unmap_area_range(
                    cur->mm, (uint64_t)p, (uint64_t)p + span);
                if (unmap_result < 0) {
                    kfree(vma);
                    return (uint64_t)unmap_result;
                }
                for (size_t i = 0; i < pages; i++)
                    vmm_release_page((uint64_t)p + i * PAGE_SIZE);
            }

            if (reserve_only) {
                vma->backing_type = anonymous ? VM_BACKING_ANONYMOUS :
                    (mapping_kind == 0x01 ? VM_BACKING_VFS_SHARED
                                          : VM_BACKING_VFS_PRIVATE);
                vma->backing_node =
                    (!anonymous && mapping_entry->type == FD_FILE)
                        ? mapping_entry->node : NULL;
                vma->backing_offset = (uint64_t)off;
                vma->start = (uint64_t)p;
                vma->end = (uint64_t)p + pages * PAGE_SIZE;
                vma->next = cur->mm->areas;
                cur->mm->areas = vma;
                return (uint64_t)p;
            }

            if (!anonymous && mapping_entry->type == FD_MEMFD &&
                mapping_kind == 0x01) {
                uint32_t backing_id = 0;
                if (linux_compat_memfd_map(
                        fd, (uint64_t)p, len, (uint64_t)off,
                        (prot & 0x02) != 0, &backing_id) < 0) {
                    int saved = errno;
                    kfree(vma);
                    return (uint64_t)(-(saved > 0 ? saved : EINVAL));
                }
                vma->backing_type = VM_BACKING_MEMFD;
                vma->backing_id = backing_id;
            } else {
                int map_result = vm_allocate_owned_pages((uint64_t)p, pages);
                if (map_result < 0) {
                    kfree(vma);
                    mmap_log_failure(fd, len, prot, flags, (uint64_t)off,
                                     mapping_file_size, (int)map_result);
                    return (uint64_t)map_result;
                }
                if (!anonymous) {
                    int fill_result;
                    if (mapping_entry->type == FD_MEMFD) {
                        fill_result = vm_fill_memfd_snapshot(
                            fd, (uint64_t)p, len, (uint64_t)off,
                            mapping_file_size);
                    } else {
                        fill_result = vm_fill_vfs_snapshot(
                            mapping_entry->node, (uint64_t)p, len,
                            (uint64_t)off);
                    }
                    if (fill_result < 0) {
                        for (size_t i = 0; i < pages; i++)
                            vmm_release_page((uint64_t)p + i * PAGE_SIZE);
                        kfree(vma);
                        mmap_log_failure(fd, len, prot, flags, (uint64_t)off,
                                         mapping_file_size, (int)fill_result);
                        return (uint64_t)fill_result;
                    }
                    vma->backing_type = mapping_kind == 0x01 ?
                        VM_BACKING_VFS_SHARED : VM_BACKING_VFS_PRIVATE;
                    vma->backing_node = mapping_entry->type == FD_FILE ?
                        mapping_entry->node : NULL;
                }
            }
            int protect_result = protect_user_range(
                (uint64_t)(uintptr_t)p, pages * PAGE_SIZE, prot);
            if (protect_result < 0) {
                for (size_t i = 0; i < pages; i++)
                    vmm_release_page((uint64_t)(uintptr_t)p +
                                     i * PAGE_SIZE);
                vm_area_drop_backing(vma);
                kfree(vma);
                return (uint64_t)protect_result;
            }
            vma->start = (uint64_t)p;
            vma->end   = (uint64_t)p + pages * PAGE_SIZE;
            vma->backing_offset = (uint64_t)off;
            vma->next  = cur->mm->areas;
            cur->mm->areas = vma;
            return (uint64_t)p;
        }

        case HBOS_SYS_MUNMAP: {
            void *addr = (void *)f->a0;
            size_t len = (size_t)f->a1;
            if (!addr || len == 0 ||
                ((uint64_t)addr & (PAGE_SIZE - 1)) ||
                len > UINT64_MAX - (PAGE_SIZE - 1))
                return (uint64_t)(-EINVAL);
            size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
            task_t *cur = task_current();
            if (!cur || !cur->mm) return (uint64_t)(-ESRCH);
            uint64_t span = (uint64_t)pages * PAGE_SIZE;
            if ((uint64_t)addr >= 0x0000800000000000ULL ||
                span > 0x0000800000000000ULL - (uint64_t)addr)
                return (uint64_t)(-EINVAL);
            int result = vm_unmap_area_range(
                cur->mm, (uint64_t)addr, (uint64_t)addr + span);
            if (result < 0) return (uint64_t)result;
            for (size_t i = 0; i < pages; i++)
                vmm_release_page((uint64_t)addr + i * PAGE_SIZE);
            return 0;
        }

        case HBOS_SYS_MREMAP: {
            /* mremap(old_addr, old_size, new_size, flags, new_addr) */
            uint64_t old_addr = (uint64_t)f->a0;
            size_t old_size = (size_t)f->a1;
            size_t new_size = (size_t)f->a2;
            int mr_flags = (int)f->a3;
            uint64_t new_addr = (uint64_t)f->a4;
            task_t *cur = task_current();
            if (!cur || !cur->mm) return (uint64_t)(-ESRCH);
            if ((old_addr & (PAGE_SIZE - 1)) || old_size == 0 ||
                new_size == 0)
                return (uint64_t)(-EINVAL);
            if (mr_flags & ~(0x1 /* MREMAP_MAYMOVE */ | 0x2 /* FIXED */))
                return (uint64_t)(-EINVAL);
            if ((mr_flags & 0x2) && !(mr_flags & 0x1))
                return (uint64_t)(-EINVAL); /* FIXED 需要 MAYMOVE */
            vm_area_t *vma = NULL;
            for (vm_area_t *a = cur->mm->areas; a; a = a->next) {
                if (old_addr >= a->start && old_addr < a->end) {
                    vma = a;
                    break;
                }
            }
            if (!vma || old_addr != vma->start)
                return (uint64_t)(-EFAULT);
            size_t old_pages = (size_t)((vma->end - vma->start) / PAGE_SIZE);
            if (old_size != vma->end - vma->start)
                return (uint64_t)(-EFAULT);
            size_t new_pages = (new_size + PAGE_SIZE - 1) / PAGE_SIZE;
            uint64_t old_flags = vmm_get_page_flags(old_addr);
            if (!old_flags) old_flags = VMM_P | VMM_U | VMM_W;
            uint32_t mv_backing_type = vma->backing_type;
            uint32_t mv_backing_id = vma->backing_id;
            uint64_t mv_backing_offset = vma->backing_offset;
            vfs_node_t *mv_backing_node = vma->backing_node;

            if (new_pages <= old_pages) {
                /* 收缩：释放尾部页 */
                for (size_t i = new_pages; i < old_pages; i++)
                    vmm_release_page(old_addr + i * PAGE_SIZE);
                vma->end = old_addr + new_pages * PAGE_SIZE;
                return old_addr;
            }

            uint64_t old_end = vma->end;
            uint64_t grow_bytes =
                (new_pages - old_pages) * PAGE_SIZE;
            int can_extend = 1;
            if (!(mr_flags & 0x2)) {
                if (!linux_va_range_free(cur, old_end,
                                         (size_t)(grow_bytes / PAGE_SIZE)))
                    can_extend = 0;
                if (can_extend &&
                    vm_allocate_owned_pages(old_end,
                                            new_pages - old_pages) < 0)
                    can_extend = 0;
                if (can_extend) {
                    vma->end = old_addr + new_pages * PAGE_SIZE;
                    return old_addr;
                }
            }
            if (!(mr_flags & 0x1)) return (uint64_t)(-ENOMEM);

            /* 移动：目标地址 */
            uint64_t dst;
            if (mr_flags & 0x2) {
                if (new_addr & (PAGE_SIZE - 1))
                    return (uint64_t)(-EINVAL);
                dst = new_addr;
                (void)vm_unmap_area_range(cur->mm, dst,
                                          dst + new_pages * PAGE_SIZE);
            } else {
                dst = linux_va_find_free(cur, new_pages,
                                         0x0000200000000000ULL);
                if (!dst) return (uint64_t)(-ENOMEM);
            }
            if (vm_allocate_owned_pages(dst, new_pages) < 0)
                return (uint64_t)(-ENOMEM);
            for (size_t i = 0; i < old_pages; i++) {
                uint64_t sp = vmm_get_phys(old_addr + i * PAGE_SIZE);
                uint64_t dp = vmm_get_phys(dst + i * PAGE_SIZE);
                if (sp && dp)
                    memcpy((void *)(uintptr_t)dp,
                           (void *)(uintptr_t)sp, PAGE_SIZE);
            }
            /* 解除旧映射（会释放 vma 与旧页） */
            (void)vm_unmap_area_range(cur->mm, old_addr, old_end);
            /* 新建 vma 记录移动后的区域 */
            vm_area_t *nv = (vm_area_t *)kmalloc(sizeof(*nv));
            if (!nv) return (uint64_t)(-ENOMEM);
            memset(nv, 0, sizeof(*nv));
            nv->start = dst;
            nv->end = dst + new_pages * PAGE_SIZE;
            nv->backing_type = mv_backing_type;
            nv->backing_id = mv_backing_id;
            nv->backing_offset = mv_backing_offset;
            nv->backing_node = mv_backing_node;
            nv->next = cur->mm->areas;
            cur->mm->areas = nv;
            return dst;
        }

        case HBOS_SYS_MPROTECT: {
            task_t *cur = task_current();
            return (uint64_t)commit_and_protect_user_range(
                cur ? cur->mm : NULL, (uint64_t)f->a0, (size_t)f->a1,
                (int)f->a2);
        }

        case HBOS_SYS_BRK: {
            return user_brk((uint64_t)f->a0);
        }

        case HBOS_SYS_SETGID:
            return finish_syscall((long)setgid((gid_t)f->a0));

        // ============================================================
        // 文件系统扩展 II (48-49)
        // ============================================================
        case HBOS_SYS_SYMLINK: {
            const char *target = (const char *)f->a0;
            const char *linkpath = (const char *)f->a1;
            return finish_syscall((long)symlink(target, linkpath));
        }

        case HBOS_SYS_CHMOD: {
            const char *path = (const char *)f->a0;
            if (!path) return (uint64_t)(-EFAULT);
            return finish_syscall((long)chmod(path, (mode_t)f->a1));
        }

        // ============================================================
        // 用户/组 ID (50-53)
        // ============================================================
        case HBOS_SYS_CHOWN: {
            const char *path = (const char *)f->a0;
            if (!path) return (uint64_t)(-EFAULT);
            return finish_syscall((long)chown(path, (uid_t)f->a1,
                                              (gid_t)f->a2));
        }

        case HBOS_SYS_GETGROUPS:
            return finish_syscall((long)getgroups((int)f->a0,
                                                  (gid_t *)f->a1));

        case HBOS_SYS_SETGROUPS:
            return finish_syscall((long)setgroups((int)f->a0,
                                                  (const gid_t *)f->a1));

        case HBOS_SYS_GETPGID: {
            pid_t pid = (pid_t)f->a0;
            if (pid == 0) pid = (pid_t)task_get_id();
            return (uint64_t)pid;
        }

        // ============================================================
        // 时间操作扩展 (54-56)
        // ============================================================
        case HBOS_SYS_NANOSLEEP: {
            const struct timespec_req {
                uint64_t tv_sec;
                uint64_t tv_nsec;
            } *req = (const struct timespec_req *)f->a0;
            if (!req) return (uint64_t)(-EFAULT);
            if (req->tv_sec > 0)
                sleep((unsigned int)req->tv_sec);
            else if (req->tv_nsec > 0)
                usleep((useconds_t)(req->tv_nsec / 1000));
            return 0;
        }

        case HBOS_SYS_CLOCK_GETTIME: {
            int clockid = (int)f->a0;
            linux_x86_timespec_t *tp = (linux_x86_timespec_t *)f->a1;
            if (!tp) return (uint64_t)(-EFAULT);
            int result = linux_clock_now(clockid, tp);
            return result < 0 ? (uint64_t)(int64_t)result : 0;
        }

        case HBOS_SYS_TIMES: {
            void *buf = (void *)f->a0;
            (void)buf;
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            uint64_t tsc = ((uint64_t)hi << 32) | lo;
            return (uint64_t)(tsc / 10000);
        }

        // ============================================================
        // 网络套接字 (57-63)
        // ============================================================
        case HBOS_SYS_SOCKET: {
            int domain = (int)f->a0;
            int type = (int)f->a1;
            int protocol = (int)f->a2;
            if (domain == 1)
                return finish_syscall(
                    linux_compat_unix_socket(type, protocol));
            if (domain != 2) return (uint64_t)(-EAFNOSUPPORT);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table) return (uint64_t)(-ESRCH);
            /* 0/1/2 是隐式控制台 fd（posix.c fd_get 对 fd<3 返回 NULL），
             * 从 3 开始分配，否则第一个 socket 会抢占 stdout 导致后续
             * write(1) 写进 socket。open()/compat_fd_alloc 都从 3 开始。 */
            int fd = -1;
            for (int i = 3; i < POSIX_MAX_FDS; i++) {
                if (!cur->fd_table->entries[i].used) { fd = i; break; }
            }
            if (fd < 0) return (uint64_t)(-EMFILE);
            cur->fd_table->entries[fd].used = true;
            cur->fd_table->entries[fd].node = NULL;
            cur->fd_table->entries[fd].offset = 0;
            /* SOCK_NONBLOCK(0x800)=O_NONBLOCK：非阻塞 connect 用 */
            cur->fd_table->entries[fd].flags = O_RDWR |
                ((type & 0x800) ? O_NONBLOCK : 0);
            cur->fd_table->entries[fd].type = FD_SOCKET;
            cur->fd_table->entries[fd].local_port = 0;
            return (uint64_t)fd;
        }

        case HBOS_SYS_BIND: {
            int sockfd = (int)f->a0;
            const void *addr = (const void *)f->a1;
            size_t addrlen = (size_t)f->a2;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            if (!addr) return (uint64_t)(-EINVAL);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table ||
                !cur->fd_table->entries[sockfd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[sockfd].type == FD_UNIX)
                return finish_syscall(linux_compat_unix_bind(
                    sockfd, addr, addrlen));
            if (addrlen < 8) return (uint64_t)(-EINVAL);
            const uint8_t *addr_bytes = (const uint8_t *)addr;
            uint16_t port = ((uint16_t)addr_bytes[2] << 8) | addr_bytes[3];
            cur->fd_table->entries[sockfd].local_port = port;
            return 0;
        }

        case HBOS_SYS_LISTEN: {
            int sockfd = (int)f->a0;
            int backlog = (int)f->a1;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            (void)backlog;
            task_t *cur = task_current();
            if (!cur || !cur->fd_table ||
                !cur->fd_table->entries[sockfd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[sockfd].type == FD_UNIX)
                return finish_syscall(
                    linux_compat_unix_listen(sockfd, backlog));
            uint16_t port = cur->fd_table->entries[sockfd].local_port;
            net_tcp_listen(port);
            return 0;
        }

        case HBOS_SYS_ACCEPT: {
            int sockfd = (int)f->a0;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table ||
                !cur->fd_table->entries[sockfd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[sockfd].type == FD_UNIX)
                return finish_syscall(linux_compat_unix_accept(
                    sockfd, (void *)f->a1, (uint32_t *)f->a2));
            net_tcp_conn_t *conn = (net_tcp_conn_t *)kmalloc(sizeof(net_tcp_conn_t));
            if (!conn) return (uint64_t)(-ENOMEM);
            int ret = net_tcp_accept(cur->fd_table->entries[sockfd].local_port, conn, 3000);
            if (ret < 0) {
                kfree(conn);
                return (uint64_t)(-EAGAIN);
            }
            int newfd = -1;
            for (int i = 0; i < POSIX_MAX_FDS; i++) {
                if (!cur->fd_table->entries[i].used) { newfd = i; break; }
            }
            if (newfd < 0) { kfree(conn); return (uint64_t)(-EMFILE); }
            cur->fd_table->entries[newfd].used  = true;
            cur->fd_table->entries[newfd].type  = FD_SOCKET;
            cur->fd_table->entries[newfd].node  = (vfs_node_t *)conn;
            cur->fd_table->entries[newfd].flags = 0;
            return (uint64_t)newfd;
        }

        case HBOS_SYS_CONNECT: {
            int sockfd = (int)f->a0;
            const void *addr = (const void *)f->a1;
            size_t addrlen = (size_t)f->a2;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            if (!addr) return (uint64_t)(-EINVAL);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table ||
                !cur->fd_table->entries[sockfd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[sockfd].type == FD_UNIX)
                return finish_syscall(linux_compat_unix_connect(
                    sockfd, addr, addrlen));
            if (addrlen < 8) return (uint64_t)(-EINVAL);
            const uint8_t *addr_bytes = (const uint8_t *)addr;
            uint32_t ip = ((uint32_t)addr_bytes[4] << 24) |
                          ((uint32_t)addr_bytes[5] << 16) |
                          ((uint32_t)addr_bytes[6] << 8) |
                          (uint32_t)addr_bytes[7];
            uint16_t port = ((uint16_t)addr_bytes[2] << 8) | addr_bytes[3];
            net_tcp_conn_t *conn = (net_tcp_conn_t *)kmalloc(sizeof(net_tcp_conn_t));
            if (!conn) return (uint64_t)(-ENOMEM);
            if (cur->fd_table->entries[sockfd].flags & O_NONBLOCK) {
                /* 非阻塞 connect：发 SYN 立即返回 EINPROGRESS，连接在
                 * 槽里推进；poll POLLOUT / getsockopt(SO_ERROR) / send 会
                 * 调 net_tcp_finish_connect 完成收尾。 */
                int r2 = net_tcp_connect_start(ip, port, conn);
                if (r2 < 0) {
                    kfree(conn);
                    return (uint64_t)(-ECONNREFUSED);
                }
                cur->fd_table->entries[sockfd].node = (vfs_node_t *)conn;
                return (uint64_t)(-EINPROGRESS);
            }
            int ret = net_tcp_connect(ip, port, conn);
            if (ret < 0) {
                kfree(conn);
                return (uint64_t)(-ECONNREFUSED);
            }
            cur->fd_table->entries[sockfd].node = (vfs_node_t *)conn;
            return 0;
        }

        case HBOS_SYS_SEND: {
            int sockfd = (int)f->a0;
            const void *buf = (const void *)f->a1;
            size_t len = (size_t)f->a2;
            int flags = (int)f->a3;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            if (!buf) return (uint64_t)(-EFAULT);
            (void)flags;
            task_t *cur = task_current();
            if (!cur || !cur->fd_table ||
                !cur->fd_table->entries[sockfd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[sockfd].type == FD_UNIX)
                return (uint64_t)linux_compat_unix_send(
                    sockfd, buf, len, flags);
            net_tcp_conn_t *conn = (net_tcp_conn_t *)cur->fd_table->entries[sockfd].node;
            if (!conn) return (uint64_t)(-ENOTCONN);
            /* net_tcp_send() returns a status code (0=success, <0=failure),
             * not a byte count -- POSIX send() must return the number of
             * bytes sent on success. It's all-or-nothing per call (any
             * len over TCP_MSS is rejected outright, no partial sends),
             * so on success the byte count is simply len. Returning the
             * raw status code here made every successful send() look like
             * "0 bytes sent" to callers, which breaks the common
             * `while (total<len) total += send(...)` retry pattern. */
            /* 非阻塞 connect 未完成时：先推进握手，仍进行则 EINPROGRESS */
            if (net_tcp_finish_connect(conn) == 1)
                return (uint64_t)(-EINPROGRESS);
            int ret = net_tcp_send(conn, (const uint8_t *)buf, (uint32_t)len);
            if (ret < 0) return (uint64_t)(-ECONNRESET);
            return (uint64_t)len;
        }

        case HBOS_SYS_RECV: {
            int sockfd = (int)f->a0;
            void *buf = (void *)f->a1;
            size_t len = (size_t)f->a2;
            int flags = (int)f->a3;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            if (!buf) return (uint64_t)(-EFAULT);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table ||
                !cur->fd_table->entries[sockfd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[sockfd].type == FD_UNIX)
                return (uint64_t)linux_compat_unix_recv(
                    sockfd, buf, len, flags);
            net_tcp_conn_t *conn = (net_tcp_conn_t *)cur->fd_table->entries[sockfd].node;
            if (!conn) return (uint64_t)(-ENOTCONN);
            /* 非阻塞 connect 未完成时：先推进握手，仍进行则 EINPROGRESS */
            if (net_tcp_finish_connect(conn) == 1)
                return (uint64_t)(-EINPROGRESS);
            uint32_t recv_len = 0;
            int ret = net_tcp_recv(conn, (uint8_t *)buf, (uint32_t)len, &recv_len, 10);
            if (ret < 0) return (uint64_t)(-ECONNRESET);
            return (uint64_t)recv_len;
        }

        // ============================================================
        // 系统管理 (64-67)
        // ============================================================
        case HBOS_SYS_REBOOT: {
            int cmd = (int)f->a0;
            if (cmd != 0x1234567 && cmd != 0x01234567)
                return (uint64_t)(-EINVAL);
            acpi_poweroff();
            while (1) __asm__ volatile("hlt");
            return 0;
        }

        case HBOS_SYS_SYNC: {
            extern int fs_sync(void);
            fs_sync();
            return 0;
        }

        case HBOS_SYS_MOUNT: {
            const char *src = (const char *)f->a0;
            const char *tgt = (const char *)f->a1;
            if (!tgt) return (uint64_t)(-EFAULT);
            if (src && src[0]) {
                extern int fs_mount_disk(void);
                if (fs_mount_disk() < 0) return (uint64_t)(-ENODEV);
            }
            (void)f->a2; (void)f->a3; (void)f->a4;
            return 0;
        }

        case HBOS_SYS_UMOUNT: {
            const char *tgt = (const char *)f->a0;
            if (!tgt) return (uint64_t)(-EFAULT);
            struct stat st;
            if (stat(tgt, &st) < 0) return (uint64_t)(-ENOENT);
            return 0;
        }

        // ============================================================
        // I/O 多路复用 & 目录遍历 (68-69)
        // ============================================================
        case HBOS_SYS_SELECT: {
            int nfds = (int)f->a0;
            uint64_t *readfds = (uint64_t *)f->a1;
            uint64_t *writefds = (uint64_t *)f->a2;
            int count = 0;
            if (nfds > POSIX_MAX_FDS) nfds = POSIX_MAX_FDS;
            task_t *cur = task_current();
            if (!cur) return (uint64_t)(-ESRCH);
            uint64_t rfds = readfds ? *readfds : 0;
            uint64_t wfds = writefds ? *writefds : 0;
            for (int fd = 0; fd < nfds; fd++) {
                if ((rfds & (1ULL << fd)) && cur->fd_table->entries[fd].used) {
                    if (readfds) rfds |= (1ULL << fd);
                    else rfds &= ~(1ULL << fd);
                    count++;
                }
                if ((wfds & (1ULL << fd)) && cur->fd_table->entries[fd].used) {
                    if (writefds) wfds |= (1ULL << fd);
                    else wfds &= ~(1ULL << fd);
                    count++;
                }
            }
            if (readfds) *readfds = rfds;
            if (writefds) *writefds = wfds;
            return (uint64_t)count;
        }

        case HBOS_SYS_GETDENTS: {
            int fd = (int)f->a0;
            struct dirent *dirp = (struct dirent *)f->a1;
            unsigned int count = (unsigned int)f->a2;
            if (fd < 0 || fd >= POSIX_MAX_FDS || !dirp)
                return (uint64_t)(-EINVAL);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table->entries[fd].used)
                return (uint64_t)(-EBADF);
            vfs_node_t *dir = cur->fd_table->entries[fd].node;
            if (!dir || dir->type != VFS_NODE_DIR)
                return (uint64_t)(-ENOTDIR);
            /* 之前这里完全没用到上面解析出来的 dir 节点，而是走
             * fs_get_count()/fs_get_file() 遍历整个文件系统的扁平文件表——
             * 相当于不管打开的是哪个目录，getdents 永远把全盘所有文件都
             * 当作"这个目录的项"返回，d_name 里存的还是完整路径而不是
             * 单个文件名。vfs_node_t 本身没有父子链接（见 vfs.h），真正能
             * 列出"这一个目录下有什么"的只有 vfs.c 给 ls 用的那套按路径
             * 字符串 + 顺序游标的 vfs_opendir/vfs_readdir/vfs_closedir——
             * 所以这里改用同一套，配合新加的 fds[fd].path（fd_alloc 时
             * open() 存下来的解析后路径，见 src/fd.h/src/lib/posix.c）。 */
            const char *path = cur->fd_table->entries[fd].path;
            if (!path[0] || vfs_opendir(path) < 0)
                return 0;
            unsigned int bytes_written = 0;
            for (uint32_t i = 0; ; i++) {
                char name[VFS_MAX_NAME];
                uint32_t type;
                if (vfs_readdir(path, name, &type) < 0) break;
                struct dirent dent;
                dent.d_ino = (uint64_t)i;
                dent.d_off = (int64_t)i;
                dent.d_type = type == VFS_NODE_DIR ? DT_DIR :
                              (type == VFS_NODE_SYMLINK ? DT_LNK : DT_REG);
                size_t name_len = strlen(name);
                if (name_len > NAME_MAX) name_len = NAME_MAX;
                memcpy(dent.d_name, name, name_len);
                dent.d_name[name_len] = '\0';
                dent.d_reclen = sizeof(struct dirent) - NAME_MAX - 1 + name_len + 1;
                unsigned int reclen = sizeof(dent);
                if (bytes_written + reclen > count) break;
                memcpy((uint8_t *)dirp + bytes_written, &dent, reclen);
                bytes_written += reclen;
            }
            vfs_closedir(path);
            return (uint64_t)bytes_written;
        }

        case HBOS_SYS_OPENDIR: {
            const char *path = (const char *)f->a0;
            if (!path) return (uint64_t)(-EFAULT);
            int ret = vfs_opendir(path);
            if (ret < 0) return (uint64_t)(-ENOENT);
            return 0;
        }

        case HBOS_SYS_READDIR: {
            const char *path = (const char *)f->a0;
            char *out_name = (char *)f->a1;
            uint32_t *out_type = (uint32_t *)f->a2;
            if (!path || !out_name || !out_type) return (uint64_t)(-EFAULT);
            int ret = vfs_readdir(path, out_name, out_type);
            if (ret < 0) return (uint64_t)(-ENOENT);
            return 0;
        }

        case HBOS_SYS_CLOSEDIR: {
            const char *path = (const char *)f->a0;
            vfs_closedir(path);
            return 0;
        }

        case HBOS_SYS_SHMGET: {
            extern int shmget(int, size_t, int);
            int key = (int)f->a0;
            size_t size = (size_t)f->a1;
            int flags = (int)f->a2;
            int ret = shmget(key, size, flags);
            if (ret < 0) return (uint64_t)(-ENOSPC);
            return (uint64_t)ret;
        }

        case HBOS_SYS_SHMAT: {
            extern void *shmat(int, const void *, int);
            int shmid = (int)f->a0;
            const void *shmaddr = (const void *)f->a1;
            int flags = (int)f->a2;
            void *ret = shmat(shmid, shmaddr, flags);
            if (ret == (void *)-1) return (uint64_t)(-EINVAL);
            return (uint64_t)(uintptr_t)ret;
        }

        case HBOS_SYS_SHMDT: {
            extern int shmdt(const void *);
            const void *shmaddr = (const void *)f->a0;
            int ret = shmdt(shmaddr);
            if (ret < 0) return (uint64_t)(-EINVAL);
            return 0;
        }

        case HBOS_SYS_SHMCTL: {
            extern int shmctl(int, int, void *);
            int shmid = (int)f->a0;
            int cmd = (int)f->a1;
            void *buf = (void *)f->a2;
            int ret = shmctl(shmid, cmd, buf);
            if (ret < 0) return (uint64_t)(-EINVAL);
            return 0;
        }

        // ============================================================
        // GUI 窗体画布 (77-83)
        // ============================================================
        case HBOS_SYS_GUI_INFO:
            return (uint64_t)gui_service_canvas_info((int *)f->a0, (int *)f->a1);

        case HBOS_SYS_GUI_CLEAR:
            gui_service_canvas_clear((uint32_t)f->a0);
            return 0;

        case HBOS_SYS_GUI_RECT:
            gui_service_canvas_rect((int)f->a0, (int)f->a1, (int)f->a2,
                                    (int)f->a3, (uint32_t)f->a4);
            return 0;

        case HBOS_SYS_GUI_TEXT:
            gui_service_canvas_text((int)f->a0, (int)f->a1, (const char *)f->a2,
                                    (uint32_t)f->a3, (int)f->a4);
            return 0;

        case HBOS_SYS_GUI_PRESENT:
            gui_service_canvas_present();
            return 0;

        case HBOS_SYS_GUI_POLLKEY:
            return (uint64_t)(long)gui_service_canvas_pollkey();

        case HBOS_SYS_GUI_POLLMOUSE:
            return (uint64_t)(long)gui_service_canvas_pollmouse((int *)f->a0,
                                                               (int *)f->a1);

        // ============================================================
        // 并发窗口服务 (84-91)
        // ============================================================
        case HBOS_SYS_WIN_OPEN: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            return (uint64_t)(long)gui_service_window_open(
                tid, (const char *)f->a0, (int)f->a1, (int)f->a2);
        }
        case HBOS_SYS_WIN_INFO: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            return (uint64_t)(long)gui_service_window_info(
                tid, (int *)f->a0, (int *)f->a1);
        }
        case HBOS_SYS_WIN_CLEAR: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            gui_service_window_clear(tid, (uint32_t)f->a0);
            return 0;
        }
        case HBOS_SYS_WIN_FILL: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            gui_service_window_fill(tid, (int)f->a0, (int)f->a1,
                                    (int)f->a2, (int)f->a3, (uint32_t)f->a4);
            return 0;
        }
        case HBOS_SYS_WIN_TEXT: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            gui_service_window_text(tid, (int)f->a0, (int)f->a1,
                                    (const char *)f->a2, (uint32_t)f->a3);
            return 0;
        }
        case HBOS_SYS_WIN_PRESENT: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            gui_service_window_present(tid);
            return 0;
        }
        case HBOS_SYS_WIN_POLL: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            return (uint64_t)(long)gui_service_window_poll(tid, (int *)f->a0);
        }
        case HBOS_SYS_WIN_CLOSE: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            gui_service_window_close(tid);
            return 0;
        }

        case HBOS_SYS_DLOPEN: {
            const char *path = (const char *)f->a0;
            if (!path) return (uint64_t)(-EFAULT);
            vfs_node_t *node = vfs_lookup(path);
            if (!node || node->type != VFS_NODE_FILE) return 0;
            void *handle = ldso_load_vfs_path(node, path);
            return (uint64_t)(uintptr_t)handle;
        }

        case HBOS_SYS_DLSYM: {
            void *handle = (void *)f->a0;
            const char *name = (const char *)f->a1;
            const char *version = (const char *)f->a2;
            void *addr = version ?
                ldso_dlsym_version(handle, name, version) :
                ldso_dlsym(handle, name);
            return (uint64_t)(uintptr_t)addr;
        }

        case HBOS_SYS_DLCLOSE: {
            void *handle = (void *)f->a0;
            return (uint64_t)(long)ldso_close(handle);
        }

        case HBOS_SYS_DLINIT_NEXT:
            return (uint64_t)ldso_init_next((void *)f->a0, f->a1);

        case HBOS_SYS_DLFINI_NEXT:
            return (uint64_t)ldso_fini_next((void *)f->a0, f->a1);

        case HBOS_SYS_DLTLS_GET:
            return (uint64_t)ldso_tls_get_addr((uint32_t)f->a0, f->a1);

        case HBOS_SYS_DLIFUNC_NEXT:
            return (uint64_t)ldso_ifunc_next((void *)f->a0, f->a1);

        case HBOS_SYS_DLIFUNC_APPLY:
            return (uint64_t)(long)ldso_ifunc_apply(
                (void *)f->a0, f->a1, (uintptr_t)f->a2);

        case HBOS_SYS_RENAME:
            return finish_syscall(posix_rename(
                (const char *)f->a0, (const char *)f->a1,
                (unsigned int)f->a2));

        case HBOS_SYS_HAX_EXISTS: {
            const char *name = (const char *)f->a0;
            if (!name) return 0;
            return hax_app_find(name) ? 1 : 0;
        }

        case HBOS_SYS_WIN2: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            return (uint64_t)(long)gui_service_window_v2(
                tid, (uint32_t)f->a0, (int)f->a1, (void *)f->a2);
        }

        case HBOS_SYS_WIN_BLIT: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            gui_service_window_blit(tid, (int)f->a0, (int)f->a1,
                                    (int)f->a2, (int)f->a3,
                                    (const uint32_t *)f->a4, (int)f->a5);
            return 0;
        }

        case HBOS_SYS_HTTPS_GET: {
            const char *host = (const char *)f->a0;
            const char *path = (const char *)f->a3;
            char *out = (char *)f->a4;
            uint32_t out_cap = (uint32_t)f->a5;
            if (!host || !host[0] || !path || path[0] != '/' || !out ||
                out_cap < 2 || out_cap > SYSCALL_HTTPS_MAX_SIZE)
                return (uint64_t)(-EINVAL);
            uint32_t out_len = 0;
            int status = tls_https_get(
                host, (uint32_t)f->a1, (uint16_t)f->a2, path,
                out, out_cap, &out_len);
            return status < 0 ? (uint64_t)(-EIO) : (uint64_t)out_len;
        }

        case HBOS_SYS_HTTPS_GET_V2: {
            const hbos_https_request_v2_t *request =
                (const hbos_https_request_v2_t *)f->a0;
            if (!request ||
                request->version != HBOS_HTTPS_REQUEST_V2_VERSION ||
                request->ca_pem_length == 0 ||
                request->ca_pem_length > 128U * 1024U ||
                request->output_capacity < 2 ||
                request->output_capacity > SYSCALL_HTTPS_MAX_SIZE)
                return (uint64_t)(-EINVAL);
            uint32_t out_len = 0;
            int status = secure_https_get(request, &out_len);
            return status < 0 ? (uint64_t)(-EIO) : (uint64_t)out_len;
        }

        case HBOS_SYS_POLL: {
            int ret = linux_compat_poll((linux_pollfd_t *)f->a0,
                                        (uint32_t)f->a1, (int)f->a2);
            return finish_syscall(ret);
        }

        case HBOS_SYS_PIPE2: {
            int ret = linux_compat_pipe2((int *)f->a0, (int)f->a1);
            return finish_syscall(ret);
        }

        case HBOS_SYS_EVENTFD2: {
            int ret = linux_compat_eventfd2((uint32_t)f->a0, (int)f->a1);
            return finish_syscall(ret);
        }

        case HBOS_SYS_EPOLL_CREATE1: {
            int ret = linux_compat_epoll_create1((int)f->a0);
            return finish_syscall(ret);
        }

        case HBOS_SYS_EPOLL_CTL: {
            int ret = linux_compat_epoll_ctl(
                (int)f->a0, (int)f->a1, (int)f->a2,
                (const linux_epoll_event_t *)f->a3);
            return finish_syscall(ret);
        }

        case HBOS_SYS_EPOLL_WAIT: {
            int ret = linux_compat_epoll_wait(
                (int)f->a0, (linux_epoll_event_t *)f->a1,
                (int)f->a2, (int)f->a3);
            return finish_syscall(ret);
        }

        case HBOS_SYS_SCHED_YIELD:
            task_yield();
            return 0;

        case HBOS_SYS_GETRANDOM:
            return (uint64_t)linux_compat_getrandom(
                (void *)f->a0, (size_t)f->a1, (unsigned int)f->a2);

        case HBOS_SYS_FUTEX: {
            int ret = linux_compat_futex6((uint32_t *)f->a0, (int)f->a1,
                                          (uint32_t)f->a2,
                                          (const void *)f->a3,
                                          (uint32_t *)f->a4,
                                          (uint32_t)f->a5);
            return finish_syscall(ret);
        }

        case HBOS_SYS_ARCH_PRCTL: {
            const int ARCH_SET_FS = 0x1002;
            const int ARCH_GET_FS = 0x1003;
            int operation = (int)f->a0;
            /* 栈 canary 存在 TCB(fs:0x28)；__stack_chk_fail 说明 canary 对不上。
             * 记录每次 ARCH_SET_FS 的值，确认是否有谁把 fs_base 改到了
             * 别的线程/别的地址（前 8 次即可，正常程序只会设一两次）。 */
            if (operation == ARCH_SET_FS) {
                static int fs_logs;
                if (fs_logs < 8) {
                    fs_logs++;
                    console_puts("[TLSC] ARCH_SET_FS pid=");
                    trace_put_udec(task_get_id());
                    console_puts(" fs=0x");
                    trace_put_hex(f->a1);
                    console_putchar('\n');
                }
            }
            if (operation == ARCH_SET_FS)
                return task_set_fs_base(f->a1) == 0 ? 0 :
                       (uint64_t)(-EINVAL);
            if (operation == ARCH_GET_FS) {
                uint64_t *output = (uint64_t *)f->a1;
                if (!output) return (uint64_t)(-EFAULT);
                *output = task_get_fs_base();
                return 0;
            }
            return (uint64_t)(-EINVAL);
        }

        case HBOS_SYS_CLONE_THREAD: {
            int tid = task_clone_user_thread(
                (const hbos_clone_request_t *)f->a0);
            return tid < 0 ? (uint64_t)(-EINVAL) : (uint64_t)tid;
        }

        case HBOS_SYS_SET_TID_ADDRESS: {
            int tid = task_set_tid_address((uint32_t *)f->a0);
            return tid < 0 ? (uint64_t)(-EFAULT) : (uint64_t)tid;
        }

        case HBOS_SYS_SET_ROBUST_LIST:
            return task_set_robust_list((void *)f->a0, (size_t)f->a1) == 0 ?
                   0 : (uint64_t)(-EINVAL);

        case HBOS_SYS_GET_ROBUST_LIST:
            return task_get_robust_list((int)f->a0, (void **)f->a1,
                                        (size_t *)f->a2) == 0 ?
                   0 : (uint64_t)(-ESRCH);

        case HBOS_SYS_SOCKETPAIR: {
            int domain = (int)f->a0;
            if (domain != 1) return (uint64_t)(-EAFNOSUPPORT);
            int ret = linux_compat_unix_socketpair(
                (int)f->a1, (int)f->a2, (int *)f->a3);
            return finish_syscall(ret);
        }

        case HBOS_SYS_GETSOCKOPT: {
            int fd = (int)f->a0;
            task_t *cur = task_current();
            if (!cur || !cur->fd_table || fd < 0 ||
                fd >= POSIX_MAX_FDS ||
                !cur->fd_table->entries[fd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[fd].type == FD_UNIX)
                return finish_syscall(linux_compat_unix_getsockopt(
                    fd, (int)f->a1, (int)f->a2,
                    (void *)f->a3, (uint32_t *)f->a4));
            if ((int)f->a1 != 1 || !f->a3 || !f->a4)
                return (uint64_t)(-ENOPROTOOPT);
            uint32_t *length = (uint32_t *)f->a4;
            if (*length < sizeof(int)) return (uint64_t)(-EINVAL);
            /* Chromium 会查询这些选项来定尺寸；TCP_NODELAY 在 level 6 */
            switch ((int)f->a2) {
                case 3: /* SO_TYPE */
                    *(int *)f->a3 = 1;
                    break;
                case 4: { /* SO_ERROR：非阻塞 connect 完成报告 */
                    net_tcp_conn_t *conn = (net_tcp_conn_t *)
                        cur->fd_table->entries[fd].node;
                    int err = 0;
                    if (conn) {
                        int fc = net_tcp_finish_connect(conn);
                        err = (fc == 1) ? EINPROGRESS : 0;
                    }
                    *(int *)f->a3 = err;
                    break;
                }
                case 7: /* SO_SNDBUF */
                    *(int *)f->a3 = 16384;
                    break;
                case 8: /* SO_RCVBUF */
                    *(int *)f->a3 = 8192;
                    break;
                case 2:  /* SO_REUSEADDR */
                case 9:  /* SO_KEEPALIVE */
                case 13: /* SO_OOBINLINE */
                default:
                    *(int *)f->a3 = 0;
                    break;
            }
            *length = sizeof(int);
            return 0;
        }

        case HBOS_SYS_SETSOCKOPT: {
            int fd = (int)f->a0;
            task_t *cur = task_current();
            if (!cur || !cur->fd_table || fd < 0 ||
                fd >= POSIX_MAX_FDS ||
                !cur->fd_table->entries[fd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[fd].type == FD_UNIX)
                return finish_syscall(linux_compat_unix_setsockopt(
                    fd, (int)f->a1, (int)f->a2,
                    (const void *)f->a3, (uint32_t)f->a4));
            /* AF_INET：SO_REUSEADDR/SO_KEEPALIVE/SO_SNDBUF/SO_RCVBUF
             * (level 1) 与 TCP_NODELAY (level 6) 接受为 no-op 成功 */
            if (!f->a3) return (uint64_t)(-EFAULT);
            return 0;
        }

        case HBOS_SYS_SHUTDOWN: {
            int fd = (int)f->a0;
            task_t *cur = task_current();
            if (!cur || !cur->fd_table || fd < 0 ||
                fd >= POSIX_MAX_FDS ||
                !cur->fd_table->entries[fd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[fd].type == FD_UNIX)
                return finish_syscall(
                    linux_compat_unix_shutdown(fd, (int)f->a1));
            return 0;
        }

        case HBOS_SYS_GETSOCKNAME:
        case HBOS_SYS_GETPEERNAME: {
            int fd = (int)f->a0;
            task_t *cur = task_current();
            if (!cur || !cur->fd_table || fd < 0 ||
                fd >= POSIX_MAX_FDS ||
                !cur->fd_table->entries[fd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[fd].type != FD_UNIX)
                return (uint64_t)(-EOPNOTSUPP);
            int result = f->nr == HBOS_SYS_GETSOCKNAME ?
                linux_compat_unix_getsockname(
                    fd, (void *)f->a1, (uint32_t *)f->a2) :
                linux_compat_unix_getpeername(
                    fd, (void *)f->a1, (uint32_t *)f->a2);
            return finish_syscall(result);
        }

        case HBOS_SYS_WEB_FETCH: {
            const hbos_web_fetch_request_t *request =
                (const hbos_web_fetch_request_t *)f->a0;
            if (!request || request->version != HBOS_WEB_FETCH_VERSION ||
                !request->host || !request->path || !request->output ||
                request->path[0] != '/' ||
                request->output_capacity < 2 ||
                request->output_capacity > SYSCALL_HTTPS_MAX_SIZE ||
                strnlen(request->host, 256) >= 256 ||
                strnlen(request->path, 2048) >= 2048)
                return (uint64_t)(-EINVAL);

            uint32_t ip = 0;
            if (net_dns_resolve(request->host, &ip) < 0)
                return (uint64_t)(-EHOSTUNREACH);

            uint32_t out_len = 0;
            int status;
            if (request->flags & HBOS_WEB_FETCH_HTTPS) {
                status = tls_https_get_with_idle_limit(
                    request->host, ip, request->port ? request->port : 443,
                    request->path, request->output,
                    request->output_capacity, &out_len, 80);
            } else {
                status = net_http_request(
                    "GET", request->host, ip,
                    request->port ? request->port : 80,
                    request->path, request->output,
                    request->output_capacity, &out_len);
            }
            return status < 0 ? (uint64_t)(-EIO) : (uint64_t)out_len;
        }

        case HBOS_SYS_DNS_RESOLVE: {
            /* getaddrinfo/gethostbyname 的底层：net_dns_resolve(name, ip4) */
            const char *name = (const char *)f->a0;
            uint32_t *out_ip = (uint32_t *)f->a1;
            if (!name || !out_ip) return (uint64_t)(-EINVAL);
            if (strnlen(name, 256) >= 256) return (uint64_t)(-EINVAL);
            if (net_dns_resolve(name, out_ip) < 0)
                return (uint64_t)(-EHOSTUNREACH);
            return 0;
        }

        case HBOS_SYS_TIMERFD_CREATE:
            return finish_syscall(linux_compat_timerfd_create(
                (int)f->a0, (int)f->a1));

        case HBOS_SYS_TIMERFD_SETTIME:
            return finish_syscall(linux_compat_timerfd_settime(
                (int)f->a0, (int)f->a1, (const void *)f->a2,
                (void *)f->a3));

        case HBOS_SYS_TIMERFD_GETTIME:
            return finish_syscall(linux_compat_timerfd_gettime(
                (int)f->a0, (void *)f->a1));

        case HBOS_SYS_MEMFD_CREATE: {
            int fd = linux_compat_memfd_create(
                (const char *)f->a0, (unsigned int)f->a1);
            return fd < 0
                ? (uint64_t)(-(errno > 0 ? errno : EINVAL))
                : (uint64_t)fd;
        }

        case HBOS_SYS_SENDMSG:
            return linux_message_io(
                (int)f->a0, (linux_x86_msghdr_t *)f->a1,
                (int)f->a2, 1);

        case HBOS_SYS_RECVMSG:
            return linux_message_io(
                (int)f->a0, (linux_x86_msghdr_t *)f->a1,
                (int)f->a2, 0);

        default:
            return (uint64_t)(-ENOSYS);
    }
}

/* ── 最近 Linux 系统调用环形缓冲（调试用）────────────────────────────
 *
 * 大型程序失败时，"最后做了什么"比"崩在哪"更有用。这里把每条 Linux
 * 调用的 (nr, a0, a1, 返回值) 记进 128 项环，用户态异常时由
 * isr_handler 调 linux_syscall_trace_dump() 打出来。open/openat 额外
 * 打印路径字符串，足以判断是"没找到文件"还是"打开了但后续失败"。
 * 只在诊断时输出，正常路径开销是两条 16 字节写入。 */
#define LINUX_TRACE_DEPTH 128
typedef struct {
    uint16_t number;
    uint16_t flags;      /* bit0: 负返回值 */
    uint64_t a0;
    uint64_t a1;
    int64_t  result;
    char     text[120];   /* open/openat/readlink 的路径：调用当场复制，
                          * 事后用户缓冲可能已被释放，不能在 dump 时再读 */
} linux_trace_entry_t;

static linux_trace_entry_t g_linux_trace[LINUX_TRACE_DEPTH];
static uint32_t g_linux_trace_head;
static uint32_t g_linux_trace_count;

static void linux_trace_push(uint64_t number, uint64_t a0, uint64_t a1,
                             int64_t result) {
    linux_trace_entry_t *slot = &g_linux_trace[g_linux_trace_head];
    slot->number = (uint16_t)number;
    slot->flags = result < 0 ? 1 : 0;
    slot->a0 = a0;
    slot->a1 = a1;
    slot->result = result;
    slot->text[0] = '\0';
    if (number == 2 || number == 257 || number == 89 || number == 21) {
        uint64_t pointer = (number == 257 || number == 21) ? a1 : a0;
        if (pointer && pointer < 0x0000800000000000ULL &&
            vmm_get_phys(pointer & ~(uint64_t)(PAGE_SIZE - 1))) {
            for (uint32_t i = 0; i + 1 < sizeof(slot->text); i++) {
                char c = *(volatile char *)(uintptr_t)(pointer + i);
                slot->text[i] = c;
                if (!c) break;
            }
            slot->text[sizeof(slot->text) - 1] = '\0';
        }
    }
    g_linux_trace_head = (g_linux_trace_head + 1) % LINUX_TRACE_DEPTH;
    if (g_linux_trace_count < LINUX_TRACE_DEPTH) g_linux_trace_count++;
}

/* 只在确认映射存在后逐字符读用户串，避免野指针把内核读崩。 */
static void linux_trace_put_string(uint64_t address, uint32_t limit) {
    if (!address) return;
    console_putchar('"');
    for (uint32_t i = 0; i < limit; i++) {
        char c = *(volatile char *)(uintptr_t)(address + i);
        if (!c) break;
        console_putchar((c >= 0x20 && c < 0x7f) ? c : '.');
    }
    console_putchar('"');
}

void linux_syscall_trace_dump(void) {
    if (!g_linux_trace_count) return;
    console_puts("--- last linux syscalls (oldest first) ---\n");
    uint32_t start = (g_linux_trace_head + LINUX_TRACE_DEPTH -
                      g_linux_trace_count) % LINUX_TRACE_DEPTH;
    for (uint32_t i = 0; i < g_linux_trace_count; i++) {
        const linux_trace_entry_t *entry =
            &g_linux_trace[(start + i) % LINUX_TRACE_DEPTH];
        console_puts("  nr=");
        trace_put_udec(entry->number);
        console_puts(" ret=");
        if (entry->flags & 1) {
            console_putchar('-');
            trace_put_udec((uint64_t)(-entry->result));
        } else {
            trace_put_udec((uint64_t)entry->result);
        }
        console_puts(" a0=");
        trace_put_hex(entry->a0);
        console_puts(" a1=");
        trace_put_hex(entry->a1);
        if (entry->text[0]) {
            console_puts(" path=");
            linux_trace_put_string((uint64_t)(uintptr_t)entry->text, 119);
        }
        console_putchar('\n');
    }
}

/* 文件类调用失败时把"哪个 fd / 哪个文件 / 什么 errno"直接打出来。
 * sqlite 之类的库只会把底层失败折成 "disk I/O error"，看不到根因；
 * 这条日志就是用来把 errno 和路径对上的。EAGAIN/EINTR 是正常控制流，跳过。 */
static void linux_trace_fd_error(uint64_t number,
                                 const hbos_syscall_frame_t *frame,
                                 int64_t result) {
    static int budget = 80;
    if (result >= 0 || !budget) return;
    switch (number) {
        case 0: case 1: case 5: case 8: case 9:
        case 17: case 18: case 72: case 74: case 75: case 77: case 285:
            break;
        default:
            return;
    }
    if (result == -EAGAIN || result == -EINTR) return;
    budget--;
    int fd = (int)frame->a0;
    console_puts("[FDERR] nr=");
    trace_put_udec(number);
    console_puts(" fd=");
    trace_put_udec((uint64_t)fd);
    console_puts(" ret=-");
    trace_put_udec((uint64_t)(-result));
    task_t *cur = task_current();
    if (cur && cur->fd_table && fd >= 0 && fd < POSIX_MAX_FDS &&
        cur->fd_table->entries[fd].used && cur->fd_table->entries[fd].path[0]) {
        console_puts(" path=");
        linux_trace_put_string(
            (uint64_t)(uintptr_t)cur->fd_table->entries[fd].path, 100);
    }
    console_putchar('\n');
    /* mmap 失败是"某子系统缺能力"的强信号，前三次附上用户态调用链，
     * 一眼看出是谁要这块内存（例如 MAP_SHARED 可写文件映射）。 */
    static int mmap_backtraces;
    if (number == 9 && mmap_backtraces < 3) {
        mmap_backtraces++;
        linux_user_backtrace((hbos_syscall_frame_t *)(uintptr_t)frame,
                             "MMAPFAIL");
    }
}

uint64_t linux_syscall_dispatch_frame(hbos_syscall_frame_t *linux_frame) {
    if (!linux_frame) return (uint64_t)(-EFAULT);
    uint64_t number = linux_frame->nr;
    uint64_t result = linux_dispatch_body(linux_frame);
    linux_trace_fd_error(number, linux_frame, (int64_t)result);
    linux_trace_push(number, linux_frame->a0, linux_frame->a1,
                     (int64_t)result);
    return result;
}
