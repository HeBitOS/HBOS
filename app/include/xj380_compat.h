/* ============================================================================
 * app/include/xj380_compat.h  —  XJ380 API 兼容层（草案，非最终实现）
 *
 * 目的：让 XJ380 风格的示例/移植代码在 HBOS 上「编译通过、运行时暴露缺口」，
 *      而不是移植到一半撞上未声明的符号。
 *
 * 对标文档：docs/XJ380-API-DIFF.md（逐项状态与补全计划见该文 §4 / §5）
 *
 * 覆盖等级（严格对应文档判定）：
 *   OK  已映射到 HBOS 现有能力（本文件内联实现）
 *   ~   部分映射（形态转换；偏差写在对应函数注释里）
 *   --  未实现族 → 返回 XAPI_ENOSYS(-38) 或 0/空结果，注释标注对应补全条目
 *
 * 约束：
 *   - 纯头文件、static inline，不参与内核构建；未使用的内联函数会被编译器丢弃。
 *   - 只依赖 app/include/hax.h 与 src/user/libc 的 POSIX 子集。
 *   - 遵守 docs/REPOSITORY_SPLIT_BOUNDARIES.md：不得访问 framebuffer、
 *     VFS 内部结构、任务结构。
 * ==========================================================================*/
#ifndef HBOS_XJ380_COMPAT_H
#define HBOS_XJ380_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#include <hax.h>              /* 已含 libc 的 stdio.h / stdlib.h / string.h / syscall.h */

#include "libc/unistd.h"
#include "libc/fcntl.h"
#include "libc/dirent.h"
#include "libc/sys/stat.h"
#include "libc/sys/mman.h"
#include "libc/time.h"

/* ── 结果约定（XJ380 3.2/3.4：0 = 成功，非 0 = 错误号；4.2：负 errno）────── */
#define XAPI_OK       0u
#define XAPI_EPERM    1u
#define XAPI_ENOENT   2u
#define XAPI_EIO      5u
#define XAPI_EBUSY    16u
#define XAPI_EINVAL   22u
#define XAPI_ENOSYS   38u        /* 本兼容层明示「HBOS 尚未实现」 */

/* ── 基础类型别名（XJ380 1.3，p15–p17）────────────────────────────────────
 * 与 hax.h 的 HI8..HU64 并存：XJ380 无前缀大写，HBOS 用 H 前缀。 */
#ifndef XJ380_BASIC_TYPES_DEFINED
#define XJ380_BASIC_TYPES_DEFINED
typedef int8_t   INT8;
typedef uint8_t  UINT8;
typedef int16_t  INT16;
typedef uint16_t UINT16;
typedef int32_t  INT32;
typedef uint32_t UINT32;
typedef int64_t  INT64;
typedef uint64_t UINT64;

typedef char  *WSTR;             /* UTF-8 字符串 */
typedef UINT64 HDLE;             /* 不透明窗口句柄（本层承载 hax_window_t） */
typedef void (*MsgPrcor)(UINT64 Type, UINT64 hData, UINT64 lData);
#endif

/* 句柄装载/取出：hax_window_t 是 HI32，XJ380 的 HDLE 是 64 位。
 * 负值（HBOS 的非法窗口）在 XJ380 侧用 XAPI_HDLE_INVALID=0 表示。 */
#define XAPI_HDLE_INVALID ((HDLE)0u)

static inline HDLE xj380_hdle_from_win(hax_window_t w) {
    return (w < 0) ? XAPI_HDLE_INVALID : (HDLE)(HU32)w;
}
static inline hax_window_t xj380_win_from_hdle(HDLE h) {
    return (h == XAPI_HDLE_INVALID) ? (hax_window_t)-1 : (hax_window_t)(HI32)h;
}

/* ══ 3.3 类型转换（p28–p29）— OK 纯语法糖 ═══════════════════════════════ */

static inline int xcr_char2int(const char *s) {
    return s ? (int)strtol(s, NULL, 10) : 0;
}

/** 整数 → 十进制字符串；返回写入长度（不含结尾 NUL），超长时安全截断。 */
static inline int xcr_int2char(long v, char *buf, int cap) {
    if (!buf || cap < 2) return 0;
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%ld", v);
    if (n < 0) return 0;
    if (n >= cap) n = cap - 1;
    memcpy(buf, tmp, (size_t)n);
    buf[n] = '\0';
    return n;
}

/** 整数 → 小写十六进制字符串（无前缀）；返回写入长度，超长时安全截断。 */
static inline int xcr_hex2char(unsigned long v, char *buf, int cap) {
    if (!buf || cap < 2) return 0;
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%lx", v);
    if (n < 0) return 0;
    if (n >= cap) n = cap - 1;
    memcpy(buf, tmp, (size_t)n);
    buf[n] = '\0';
    return n;
}

typedef struct { UINT8 Red, Green, Blue; } XCOLOR;         /* p91：24 位色 */
typedef struct { UINT8 Red, Green, Blue, Alpha; } XCOLORA; /* p80：32 位色 */

/** 0xRRGGBB → XCOLOR（HBOS 的 HCOLOR 与 0xRRGGBB 同布局）。 */
static inline XCOLOR toRGB(UINT32 color) {
    XCOLOR c;
    c.Red   = (UINT8)((color >> 16) & 0xFFu);
    c.Green = (UINT8)((color >> 8) & 0xFFu);
    c.Blue  = (UINT8)(color & 0xFFu);
    return c;
}

/** 0xRRGGBB + alpha → XCOLORA。 */
static inline XCOLORA toRGBA(UINT32 color, UINT8 alpha) {
    XCOLORA c;
    c.Red   = (UINT8)((color >> 16) & 0xFFu);
    c.Green = (UINT8)((color >> 8) & 0xFFu);
    c.Blue  = (UINT8)(color & 0xFFu);
    c.Alpha = alpha;
    return c;
}

/** 双轨颜色的收敛点：XCOLORA → HBOS 的 0xAARRGGBB（文档 §6「不适用」）。
 *  HBOS 不引入第二套颜色表示，转换只发生在本兼容层内部。 */
static inline HCOLOR xj380_hcolor_a(XCOLORA c) {
    return ((HCOLOR)c.Alpha << 24) | ((HCOLOR)c.Red << 16) |
           ((HCOLOR)c.Green << 8) | (HCOLOR)c.Blue;
}

/** XCOLOR → HCOLOR（不透明）。 */
static inline HCOLOR xj380_hcolor(XCOLOR c) {
    return ((HCOLOR)c.Red << 16) | ((HCOLOR)c.Green << 8) | (HCOLOR)c.Blue;
}

/* ══ 3.1 文本输入输出（p21–p23）— OK 映射 libc stdio ═══════════════════ */

static inline void xapi_Output(WSTR str)    { fputs(str ? str : "", stdout); }
static inline void xapi_PrintLine(WSTR str) { puts(str ? str : ""); }
static inline void xapi_EndLine(void)       { fputc('\n', stdout); }
static inline char xapi_Getch(void)         { return (char)getchar(); }

/** XJ380 约定调用方提供 ≥1024 字节可写缓冲区（p21）。 */
static inline void xapi_Input(WSTR str)   { if (str) (void)fgets(str, 1024, stdin); }
static inline void xapi_Getline(WSTR str) { if (str) (void)fgets(str, 1024, stdin); }

static inline void xapi_Printf(WSTR fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/** -- 缺：无用户态串口通道（文档 §4.3）。 */
static inline void xapi_OutputSerial(WSTR str) { (void)str; }

/* ══ 3.2 文件操作（p23–p27）══════════════════════════════════════════ */

typedef struct { UINT64 length; void *buffer; } XFILE;                        /* p23 */
typedef struct { char filename[256]; UINT64 length; UINT64 filetype; } DirNode; /* p24 */

/** ~ XJ380 语义是「整文件读入 XFILE」，HBOS 原语是 fd。
 *  上限 64 MiB（超出返回 NULL），大文件请直接用 open/read。 */
static inline XFILE *xapi_OpenFile(WSTR path) {
    if (!path) return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    size_t cap = 64u * 1024u, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { close(fd); return NULL; }

    for (;;) {
        if (len == cap) {
            if (cap >= (64u * 1024u * 1024u)) { free(buf); close(fd); return NULL; }
            char *nb = (char *)realloc(buf, cap * 2u);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb;
            cap *= 2u;
        }
        long n = (long)read(fd, buf + len, cap - len);
        if (n < 0) { free(buf); close(fd); return NULL; }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);

    XFILE *f = (XFILE *)malloc(sizeof(XFILE));
    if (!f) { free(buf); return NULL; }
    f->length = (UINT64)len;
    f->buffer = buf;
    return f;
}

static inline void xapi_CloseFile(XFILE *fsptr) {
    if (!fsptr) return;
    free(fsptr->buffer);
    free(fsptr);
}

static inline UINT64 xapi_Mkdir(WSTR path) {
    return (path && mkdir(path, 0777) == 0) ? XAPI_OK : XAPI_EIO;
}
static inline UINT64 xapi_Rmdir(WSTR path) {
    return (path && rmdir(path) == 0) ? XAPI_OK : XAPI_EIO;
}
static inline UINT64 xapi_DeleteFile(WSTR path) {
    return (path && unlink(path) == 0) ? XAPI_OK : XAPI_ENOENT;
}
static inline UINT64 xapi_RenameFile(WSTR old_path, WSTR new_path) {
    if (!old_path || !new_path) return XAPI_EINVAL;
    return (rename(old_path, new_path) == 0) ? XAPI_OK : XAPI_EIO;
}

static inline UINT64 xapi_CreateFile(WSTR filename) {
    if (!filename) return XAPI_EINVAL;
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return XAPI_EIO;
    close(fd);
    return XAPI_OK;
}

/** OK 已映射：lseek + read 循环（HBOS libc 未声明 pread）。 */
static inline UINT64 xapi_ReadFile(WSTR filename, char *buffer, UINT64 size, UINT64 offset) {
    if (!filename || !buffer) return XAPI_EINVAL;
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return XAPI_ENOENT;
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) { close(fd); return XAPI_EIO; }
    UINT64 total = 0;
    while (total < size) {
        long n = (long)read(fd, buffer + total, (size_t)(size - total));
        if (n < 0) { close(fd); return XAPI_EIO; }
        if (n == 0) break;
        total += (UINT64)n;
    }
    close(fd);
    return XAPI_OK;
}

static inline UINT64 xapi_WriteFile(WSTR filename, char *buffer, UINT64 size, UINT64 offset) {
    if (!filename || !buffer) return XAPI_EINVAL;
    int fd = open(filename, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) return XAPI_EIO;
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) { close(fd); return XAPI_EIO; }
    UINT64 total = 0;
    while (total < size) {
        long n = (long)write(fd, buffer + total, (size_t)(size - total));
        if (n <= 0) { close(fd); return XAPI_EIO; }
        total += (UINT64)n;
    }
    close(fd);
    return XAPI_OK;
}

/** OK 已映射：opendir/readdir 物化为 DirNode 数组（XJ380 是「一次返回全量」）。
 *  length 不做逐项 stat（XJ380 p24 亦未定义其语义），filetype 取 d_type。 */
static inline void xapi_SearchFile(WSTR path, INT32 *count, DirNode **dir) {
    if (count) *count = 0;
    if (dir) *dir = NULL;
    if (!path || !count || !dir) return;

    DIR *d = opendir(path);
    if (!d) return;

    size_t cap = 16, n = 0;
    DirNode *arr = (DirNode *)malloc(cap * sizeof(DirNode));
    if (!arr) { closedir(d); return; }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (n == cap) {
            DirNode *na = (DirNode *)realloc(arr, cap * 2u * sizeof(DirNode));
            if (!na) break;
            arr = na;
            cap *= 2u;
        }
        memset(&arr[n], 0, sizeof(arr[n]));
        snprintf(arr[n].filename, sizeof(arr[n].filename), "%s", e->d_name);
        arr[n].filetype = (UINT64)e->d_type;
        n++;
    }
    closedir(d);

    *count = (INT32)n;
    *dir   = arr;
}

static inline void xapi_SearchFile_freem(DirNode *dir, INT32 count) {
    (void)count;
    free(dir);
}

/** -- 缺：无文件选择器服务（文档 §5 P2-10）。 */
static inline UINT64 xapi_FileDialog(UINT64 mode, WSTR title, WSTR initial_path,
                                     char *out_path, UINT64 out_size) {
    (void)mode; (void)title; (void)initial_path;
    if (out_path && out_size) out_path[0] = '\0';
    return XAPI_ENOSYS;
}

/* ══ 3.4 进程与线程（p30–p32）════════════════════════════════════════ */

static inline UINT64 xapi_Fork(void) { return (UINT64)__syscall1(HBOS_SYS_FORK, 0); }

static inline UINT64 xapi_Execve(WSTR filename, WSTR argv[], WSTR envp[]) {
    return (UINT64)execve(filename, (char *const *)argv, (char *const *)envp);
}

static inline void xapi_Exit(UINT64 value) { __syscall1(HBOS_SYS_EXIT, (long)value); }

/** -- 缺：无用户态任务枚举 syscall（文档 §5 P1-5）。
 *  返回 0 表示「无法枚举」，调用方不得据此认为进程数为 0。 */
typedef struct {
    UINT64 pid, ppid, tid, cpu_id, task_level, thread_count, window_count;
    UINT64 memory_bytes, cpu_runtime_ticks;
    UINT32 process_status, thread_status;
    char   process_name[32], thread_name[32];
} XapiTaskInfo;                                               /* p30 子集 */

#define XAPI_TASK_NAME_LEN 32

static inline UINT64 xapi_GetTaskList(XapiTaskInfo *buffer, UINT64 max_count) {
    (void)buffer; (void)max_count;
    return 0;
}

static inline UINT64 xapi_KillProcess(UINT64 pid) {
    return (__syscall3(HBOS_SYS_KILL, (long)pid, 15 /* SIGTERM */, 0) == 0)
               ? XAPI_OK : XAPI_EIO;
}

/* ══ 3.5 获取当前信息（p33–p34）══════════════════════════════════════ */

/** OK：HBOS_SYS_UNAME 使用 Linux utsname 布局（见 src/syscall.c 的处理处）。 */
typedef struct {
    char sysname[65], nodename[65], release[65];
    char version[65], machine[65], domainname[65];
} xj380_utsname_t;

static inline void xapi_GetSystemVersion(WSTR version) {
    if (!version) return;
    xj380_utsname_t u;
    memset(&u, 0, sizeof(u));
    if (__syscall1(HBOS_SYS_UNAME, (long)&u) < 0) {
        snprintf(version, 128, "HBOS");
        return;
    }
    snprintf(version, 128, "%s %s", u.sysname, u.release);
}

static inline UINT64 xapi_GetTime(void) { return (UINT64)time(NULL); }

typedef struct {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
} TimeType;                                                   /* p32 */

/** OK 字段序与 struct tm 完全一致。 */
static inline void xapi_GetTimeX(TimeType *tm) {
    if (!tm) return;
    time_t t = time(NULL);
    struct tm r;
    if (!localtime_r(&t, &r)) { memset(tm, 0, sizeof(*tm)); return; }
    tm->tm_sec = r.tm_sec;   tm->tm_min  = r.tm_min;  tm->tm_hour = r.tm_hour;
    tm->tm_mday = r.tm_mday; tm->tm_mon  = r.tm_mon;  tm->tm_year = r.tm_year;
    tm->tm_wday = r.tm_wday; tm->tm_yday = r.tm_yday; tm->tm_isdst = r.tm_isdst;
}

static inline UINT64 xapi_GetTimeNano(void) {
    struct timespec ts;
    if (__syscall3(HBOS_SYS_CLOCK_GETTIME, 0 /* CLOCK_REALTIME */,
                   (long)&ts, 0) < 0) return 0;
    return ((UINT64)ts.tv_sec * 1000000000ull) + (UINT64)ts.tv_nsec;
}

/** -- 缺：无用户模型（文档 §6「不适用」）。返回固定占位身份。 */
typedef struct { char name[64]; UINT32 user_type; } UserInfo;  /* p32 */

static inline void xapi_GetCurrentUser(UserInfo *user_info) {
    if (!user_info) return;
    memset(user_info, 0, sizeof(*user_info));
    snprintf(user_info->name, sizeof(user_info->name), "user");
}

/** -- 缺：CPU 型号未导出（文档 §5 P1-5）。返回架构名。 */
static inline void xapi_GetCpuModel(WSTR version) {
    if (version) snprintf(version, 128, "x86_64");
}

/** -- 缺：物理内存量未导出（文档 §5 P1-5）。0 表示未知。 */
static inline UINT64 xapi_GetMemorySize(void) { return 0; }

/* ══ 3.6 运行（p35–p37）══════════════════════════════════════════════ */

/** -- 缺：无应用崩溃上报契约。写入固定说明便于定位。 */
static inline void xapi_Broken(WSTR broken_info) {
    if (broken_info) snprintf(broken_info, 256, "xapi_Broken: not supported on HBOS");
}

/** ~ 单位差异：XJ380 用毫秒，HBOS usleep 用微秒。 */
static inline void xapi_Sleep(UINT64 ms) { usleep((unsigned)(ms * 1000u)); }

static inline void xapi_Run(WSTR path) {
    if (!path) return;
    char *const argv[] = { (char *)path, NULL };
    (void)execve(path, argv, NULL);
}

static inline UINT64 xapi_RunArgs(WSTR path, WSTR argv[]) {
    return (UINT64)execve(path, (char *const *)argv, NULL);
}

/** ~ XJ380 保留调用者身份；HBOS 无权限层级，等同 fork + execve（文档 §6）。 */
static inline UINT64 xapi_SpawnInherited(WSTR path, WSTR argv[]) {
    UINT64 pid = xapi_Fork();
    if (pid == 0) {
        (void)execve(path, (char *const *)argv, NULL);
        xapi_Exit(127);
    }
    return pid;
}

/** -- 缺：HBOS 无系统时间缓存/节流机制，无操作。 */
static inline void xapi_FlushTime(void) { }

typedef struct {
    UINT64 cpu_total_ticks, cpu_busy_ticks, cpu_count;
    UINT64 memory_total_bytes, memory_free_bytes, timestamp_ns;
} XapiSystemStats;                                            /* p35 */

/** -- 缺：无系统统计导出（文档 §5 P1-5）。返回 0 表示「无法读取」。 */
static inline UINT64 xapi_GetSystemStats(XapiSystemStats *stats) {
    if (!stats) return XAPI_EINVAL;
    memset(stats, 0, sizeof(*stats));
    return 0;
}

/** ~ 无统一电源动作入口与权限校验，直连 HBOS 内核同类 syscall。 */
#define XAPI_POWER_SHUTDOWN 0u
#define XAPI_POWER_REBOOT   1u

static inline UINT64 xapi_PowerAction(UINT64 action) {
    if (action == XAPI_POWER_REBOOT) return (UINT64)__syscall1(HBOS_SYS_REBOOT, 0);
    return (UINT64)__syscall1(HBOS_SYS_SHUTDOWN, 0);
}

/* ══ 3.7 内存（p37–p38）══════════════════════════════════════════════ */

static inline void *xapi_AllocateMemory(UINT64 size) { return malloc((size_t)size); }
static inline void  xapi_FreeMemory(void *ptr)       { free(ptr); }

/** ~ 需 POSIX prot/flags；XJ380 的 flags 语义手册未细化（p38）。 */
#define XAPI_MAP_READ  0x1u
#define XAPI_MAP_WRITE 0x2u

static inline void *xapi_MapMemory(void *ptr, uint64_t size, uint32_t flags) {
    int prot = PROT_READ | ((flags & XAPI_MAP_WRITE) ? PROT_WRITE : 0);
    return mmap(ptr, (size_t)size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

/* ══ 4.1 窗口（p65–p67）— OK 映射到多窗口 v2 ABI ═════════════════════ */

#define XWIN_NORMAL             0
#define XWIN_FRAME_OFF          1
#define XWIN_FULL_SCR           2
#define XWIN_TYPE_MASK          0x0f
#define XWIN_SUPPORT_RESIZEABLE 0x80

typedef struct { UINT32 width, height; WSTR title; UINT8 sets; } XWINDOW; /* p65 */

static inline void xapi_CreateWindow(HDLE *handle, XWINDOW *xwin) {
    if (handle) *handle = XAPI_HDLE_INVALID;
    if (!handle || !xwin) return;
    *handle = xj380_hdle_from_win(
        hax_window_create(xwin->title, (int)xwin->width, (int)xwin->height, 0));
}

static inline void xapi_SetWindowTitle(HDLE handle, WSTR str) {
    (void)hax_window_set_title(xj380_win_from_hdle(handle), str);
}

static inline void xapi_CloseWindow(HDLE handle) {
    (void)hax_window_close(xj380_win_from_hdle(handle));
}

/** -- 缺：只能构建期指定图标（.haxicon 段），运行期无接口（文档 §5 P2-11）。 */
static inline void xapi_SetIcon(HDLE handle, WSTR path) { (void)handle; (void)path; }

static inline void xapi_GetWindowSize(HDLE handle, UINT64 *width, UINT64 *height) {
    hax_window_state_t st;
    memset(&st, 0, sizeof(st));
    st.struct_size = (HU32)sizeof(st);
    if (hax_window_get_state(xj380_win_from_hdle(handle), &st) < 0) {
        if (width) *width = 0;
        if (height) *height = 0;
        return;
    }
    if (width) *width = (UINT64)st.width;
    if (height) *height = (UINT64)st.height;
}

/* ══ 4.2 绘图（p67–p79）— ~ 命令批提交，非临时表面 ═══════════════════ */

/* v2 绘制命令只有 CLEAR/FILL/TEXT/ARGB，且内核侧 TEXT 只吃
 * (data=字符串, color)，见 src/gui/service.c 的 GUI_SERVICE_V2_DRAW_BATCH。 */

static inline void xj380_win_fill(HDLE handle, HI32 x, HI32 y, HI32 w, HI32 h,
                                  HCOLOR color) {
    hax_draw_command_t c;
    memset(&c, 0, sizeof(c));
    c.type = HAX_DRAW_FILL;
    c.x = x; c.y = y; c.width = w; c.height = h; c.color = color;
    (void)hax_window_draw(xj380_win_from_hdle(handle), &c, 1);
}

/** ~ 单像素无原生 op：用 1×1 FILL 表达（文档 §4.4）。 */
static inline void xapi_DrawPoint(HDLE handle, UINT32 x, UINT32 y, UINT32 color) {
    xj380_win_fill(handle, (HI32)x, (HI32)y, 1, 1, (HCOLOR)color);
}

/** ~ 直线需自行栅格化（v2 无 line 命令；hax_gui_line 只服务 TUI 画布）。
 *  按 64 条一批提交（内核 batch 上限 1024）。 */
static inline void xapi_DrawLine(HDLE handle, UINT32 x1, UINT32 y1,
                                 UINT32 x2, UINT32 y2, UINT32 color) {
    hax_window_t w = xj380_win_from_hdle(handle);
    if (w < 0) return;

    int ix1 = (int)x1, iy1 = (int)y1, ix2 = (int)x2, iy2 = (int)y2;
    int dx = (ix2 > ix1) ? (ix2 - ix1) : (ix1 - ix2);
    int dy = (iy2 > iy1) ? (iy2 - iy1) : (iy1 - iy2);
    int steps = (dx > dy) ? dx : dy;
    if (steps <= 0) { xapi_DrawPoint(handle, x1, y1, color); return; }

    hax_draw_command_t cmds[64];
    int n = 0;
    for (int i = 0; i <= steps; i++) {
        hax_draw_command_t *c = &cmds[n];
        memset(c, 0, sizeof(*c));
        c->type   = HAX_DRAW_FILL;
        c->x      = (HI32)(ix1 + (int)((long)(ix2 - ix1) * i / steps));
        c->y      = (HI32)(iy1 + (int)((long)(iy2 - iy1) * i / steps));
        c->width  = 1;
        c->height = 1;
        c->color  = (HCOLOR)color;
        if (++n == 64) { (void)hax_window_draw(w, cmds, (HU32)n); n = 0; }
    }
    if (n) (void)hax_window_draw(w, cmds, (HU32)n);
}

static inline void xapi_DrawRect(HDLE handle, UINT32 x1, UINT32 y1,
                                 UINT32 x2, UINT32 y2, UINT32 color, bool fill) {
    HI32 x = (HI32)x1, y = (HI32)y1;
    HI32 w = (HI32)((x2 >= x1) ? (x2 - x1 + 1) : 0);
    HI32 h = (HI32)((y2 >= y1) ? (y2 - y1 + 1) : 0);
    if (w <= 0 || h <= 0) return;

    if (fill) { xj380_win_fill(handle, x, y, w, h, (HCOLOR)color); return; }

    hax_draw_command_t e[4];
    memset(e, 0, sizeof(e));
    for (int i = 0; i < 4; i++) { e[i].type = HAX_DRAW_FILL; e[i].color = (HCOLOR)color; }
    e[0].x = x;         e[0].y = y;         e[0].width = w; e[0].height = 1; /* 上 */
    e[1].x = x;         e[1].y = y + h - 1; e[1].width = w; e[1].height = 1; /* 下 */
    e[2].x = x;         e[2].y = y;         e[2].width = 1; e[2].height = h; /* 左 */
    e[3].x = x + w - 1; e[3].y = y;         e[3].width = 1; e[3].height = h; /* 右 */
    (void)hax_window_draw(xj380_win_from_hdle(handle), e, 4);
}

/** ~ 无字号/字体：内核 TEXT 命令只接受字符串 + 颜色（scale 恒 1）。
 *  需要缩放时用单窗口 hax_gui_text()（仅 TUI 画布）。 */
static inline void xapi_DrawText(HDLE handle, UINT32 x, UINT32 y, WSTR str,
                                 UINT32 size, UINT32 color) {
    (void)size;
    hax_draw_command_t c;
    memset(&c, 0, sizeof(c));
    c.type  = HAX_DRAW_TEXT;
    c.x     = (HI32)x;
    c.y     = (HI32)y;
    c.color = (HCOLOR)color;
    c.data  = (const void *)str;
    (void)hax_window_draw(xj380_win_from_hdle(handle), &c, 1);
}

/* -- 缺族：文本度量与裁剪绘制（文档 §5 P0-1，共 11 项）。
 * 返回 0 / 空行为的桩，调用方必须能容忍「测得宽度为 0」。 */
static inline UINT64 xapi_CalcTextWidth(WSTR str, UINT32 size) {
    (void)str; (void)size; return 0;
}
static inline void xapi_DrawTextl(HDLE h, UINT32 x, UINT32 y, WSTR s, UINT32 sz,
                                  UINT32 c, UINT32 *width) {
    if (width) *width = 0;
    xapi_DrawText(h, x, y, s, sz, c);
}
static inline void xapi_DrawSWText(HDLE h, UINT32 x, UINT32 y, WSTR s, UINT32 c) {
    xapi_DrawText(h, x, y, s, 0, c);
}
static inline void xapi_DrawTextTop(HDLE h, INT32 x, INT32 y, WSTR t, UINT32 sz, UINT32 c) {
    xapi_DrawText(h, (UINT32)x, (UINT32)y, t, sz, c);
}
static inline void xapi_DrawMonoTextTop(HDLE h, INT32 x, INT32 y, WSTR t, UINT32 c) {
    xapi_DrawText(h, (UINT32)x, (UINT32)y, t, 0, c);
}
static inline void xapi_DrawTextInRect(HDLE h, INT32 x, INT32 y, INT32 w, INT32 hh,
                                       WSTR t, UINT32 sz, UINT32 c) {
    (void)w; (void)hh;   /* 无裁剪矩形，直接绘制（文档 §5 P0-1） */
    xapi_DrawText(h, (UINT32)x, (UINT32)y, t, sz, c);
}
static inline INT32 xapi_DrawSvg(HDLE h, UINT32 x, UINT32 y, UINT32 w, WSTR s, bool t) {
    (void)h; (void)x; (void)y; (void)w; (void)s; (void)t;
    return -(INT32)XAPI_ENOSYS;
}
static inline INT32 xapi_DrawFA(HDLE h, UINT32 x, UINT32 y, UINT32 w, char *n, bool t) {
    (void)h; (void)x; (void)y; (void)w; (void)n; (void)t;
    return -(INT32)XAPI_ENOSYS;
}

/* ══ 4.3 图片（p80–p82）— -- 缺：SDK 无解码器（文档 §5 P1-4）════════ */

static inline void xapi_DrawBMP(HDLE h, UINT32 x, UINT32 y, UINT32 w, UINT32 hh, WSTR p) {
    (void)h; (void)x; (void)y; (void)w; (void)hh; (void)p;
}
static inline void xapi_DrawPNG(HDLE h, UINT32 x, UINT32 y, UINT32 w, UINT32 hh, WSTR p) {
    (void)h; (void)x; (void)y; (void)w; (void)hh; (void)p;
}
static inline void xapi_DrawPicture(HDLE h, UINT32 x, UINT32 y, UINT32 w, UINT32 hh, WSTR p) {
    (void)h; (void)x; (void)y; (void)w; (void)hh; (void)p;
}
static inline void xapi_GetPicSize(UINT32 *width, UINT32 *height, WSTR path) {
    (void)path;
    if (width) *width = 0;
    if (height) *height = 0;
}
static inline bool xapi_LoadPicture(XCOLORA *buffer, UINT32 width, UINT32 height, WSTR path) {
    (void)buffer; (void)width; (void)height; (void)path;
    return false;
}

/* ══ 4.5 framebuffer（p91–p93）— ~ 只有写方向 ═══════════════════════ */

/** OK 已映射到 HAX_DRAW_ARGB；pixels 为 0xAARRGGBB，stride 为像素步长。 */
static inline void xapi_WriteBuffer(HDLE handle, UINT32 x, UINT32 y, UINT32 width,
                                    UINT32 height, UINT32 *buffer) {
    hax_draw_command_t c;
    memset(&c, 0, sizeof(c));
    c.type   = HAX_DRAW_ARGB;
    c.x      = (HI32)x;
    c.y      = (HI32)y;
    c.width  = (HI32)width;
    c.height = (HI32)height;
    c.data   = (const void *)buffer;
    c.stride = (HI32)width;
    (void)hax_window_draw(xj380_win_from_hdle(handle), &c, 1);
}

/** -- 缺：无 framebuffer 回读（文档 §5 P2-9 的前置条件）。 */
static inline void xapi_ReadBuffer(HDLE h, UINT32 x, UINT32 y, UINT32 w, UINT32 hh,
                                   UINT32 *buffer) {
    (void)h; (void)x; (void)y; (void)w; (void)hh; (void)buffer;
}

static inline void xapi_RefreshWindow(HDLE handle) {
    (void)hax_window_present(xj380_win_from_hdle(handle), NULL);
}

static inline void xapi_RefreshPartWindow(HDLE handle, UINT32 x1, UINT32 y1,
                                          UINT32 x2, UINT32 y2) {
    hax_rect_t dirty;
    dirty.x      = (HI32)x1;
    dirty.y      = (HI32)y1;
    dirty.width  = (HI32)((x2 >= x1) ? (x2 - x1 + 1) : 0);
    dirty.height = (HI32)((y2 >= y1) ? (y2 - y1 + 1) : 0);
    (void)hax_window_present(xj380_win_from_hdle(handle), &dirty);
}

/* ══ 4.6 控件（p94–p99）— OK 已由 hax_widgets.h 全覆盖 ═══════════════
 * 形态差异：XJ380 控件归窗口所有（CRLid 标识），HBOS 控件归应用侧 hax_ui_t。
 * 因此这里**不提供** xapi_Button 一类过程式接口——直接使用：
 *   hax_ui_add_button / add_textbox / add_toggle / add_scrollbar / add_menu …
 *   hax_ui_set_value / hax_ui_remove_widget / hax_ui_dispatch
 * 不桥接的理由见文档 §4.4 与 §5 P3-14（服务端控件布局会破坏拆分边界）。
 * ═══════════════════════════════════════════════════════════════════ */

/* ══ 4.7 动画（p100–p104）— ~ 应用内插值，无窗口绑定 ═════════════════
 * 直接用 hax_anim.h：hax_anim_declare/set/to/to_many/tick/finish。
 * 文档 §5 P1-3 规划补 hax_anim_bind() + HAX_EV_ANIM 以支持服务端帧驱动。
 * ═══════════════════════════════════════════════════════════════════ */

/* ══ 4.8 剪贴板（p104–p112）— -- 系统级缺失（文档 §5 P0-2）═══════════ */

#define XCLIP_OK 0

static inline INT64 xapi_ClipboardSetText(const char *text, UINT64 bytes) {
    (void)text; (void)bytes; return -(INT64)XAPI_ENOSYS;
}
static inline INT64 xapi_ClipboardSetData(const void *data) {
    (void)data; return -(INT64)XAPI_ENOSYS;
}
static inline INT64 xapi_ClipboardQuery(void *info) {
    (void)info; return -(INT64)XAPI_ENOSYS;
}
static inline INT64 xapi_ClipboardAttach(UINT64 window, UINT32 profile, UINT32 formats) {
    (void)window; (void)profile; (void)formats; return -(INT64)XAPI_ENOSYS;
}
static inline INT64 xapi_RequestPaste(UINT64 window, UINT64 control, UINT32 format) {
    (void)window; (void)control; (void)format; return -(INT64)XAPI_ENOSYS;
}
static inline INT64 xapi_GetPasteInfo(UINT64 paste, void *info) {
    (void)paste; (void)info; return -(INT64)XAPI_ENOSYS;
}
static inline INT64 xapi_ReadPaste(UINT64 paste, UINT32 format, void *buffer, UINT64 cap) {
    (void)paste; (void)format; (void)buffer; (void)cap; return -(INT64)XAPI_ENOSYS;
}
static inline INT64 xapi_FinishPaste(UINT64 paste) {
    (void)paste; return -(INT64)XAPI_ENOSYS;
}
static inline INT64 xapi_ClipboardClaimMove(UINT64 window, UINT64 control) {
    (void)window; (void)control; return -(INT64)XAPI_ENOSYS;
}

/** ~ 应用内选区已有（hax_widgets.h:816/833/843），跨进程协议缺。 */
static inline INT64 xapi_TextInputGetSelection(UINT64 w, UINT64 ctl, void *sel) {
    (void)w; (void)ctl; (void)sel; return -(INT64)XAPI_ENOSYS;
}
static inline INT64 xapi_TextInputSetSelection(UINT64 w, UINT64 ctl, const void *sel) {
    (void)w; (void)ctl; (void)sel; return -(INT64)XAPI_ENOSYS;
}
static inline INT64 xapi_TextInputSetFlags(UINT64 w, UINT64 ctl, UINT32 flags) {
    (void)w; (void)ctl; (void)flags; return -(INT64)XAPI_ENOSYS;
}

/* ══ 4.9 输入法（p112–p126）— -- 16 项全缺，路线见文档 §5 P3-13 ═══════
 * 前置条件：UTF-8 键盘事件 → 候选窗服务 → 跨进程 IME 会话协议。
 * ═══════════════════════════════════════════════════════════════════ */

/* ══ 4.10 截屏（p127–p128）— -- 需先有 framebuffer 回读 ═══════════════ */

static inline UINT64 xapi_CaptureScreen(UINT32 *buffer, UINT64 pixel_capacity,
                                        UINT32 *width, UINT32 *height) {
    (void)buffer; (void)pixel_capacity;
    if (width) *width = 0;
    if (height) *height = 0;
    return XAPI_ENOSYS;
}

/** -- 不适用：HBOS 无 DRM/内容保护层（文档 §6）。 */
static inline INT64 xapi_SetSecret(void) { return -(INT64)XAPI_ENOSYS; }

/* ══ 4.11 通知与重点（p128–p130）— -- 无系统托盘/通知中心 ═════════════ */

typedef struct {
    UINT64 key;
    char  *title, *text;
    UINT64 builtin_icon;
    char  *icon_path;
    char  *action0_text; UINT64 action0_id;
    char  *action1_text; UINT64 action1_id;
} XApiNotificationRequest;                                    /* p128 */

static inline void   xapi_SendAppMessage(WSTR title, WSTR text) {
    (void)title; (void)text;
}
static inline UINT64 xapi_SetNotifyPrcor(MsgPrcor func) {
    (void)func; return XAPI_ENOSYS;
}
static inline UINT64 xapi_SendNotification(const XApiNotificationRequest *req) {
    (void)req; return XAPI_ENOSYS;
}
static inline INT32 xapi_HighlightNotice(UINT32 style, const char *text) {
    (void)style; (void)text; return -(INT32)XAPI_ENOSYS;
}

/* ══ 5.x 网络 — 直接用 BSD socket（src/user/libc/socket.h）═════════════
 * OK   socket/bind/listen/accept/connect/send/recv/sendto/recvfrom/
 *      shutdown/getsockopt/setsockopt/poll/epoll_*
 * OK   getaddrinfo/gethostbyname（netdb.h，阻塞式；无异步句柄）
 * ~    端点查询 getsockname/getpeername（AF_INET 的 getpeername 仍缺，
 *      见 docs/LINUX_KDE_COMPAT.md「当前明确缺口」）
 * --   句柄式 TLS/HTTP 报文 API、网卡枚举与网络配置管理
 *      （文档 §5 P2-12、§6「不适用」）
 * 便捷 GET：hax_https_get()（hax.h:164）/ hax_https_get_v2()（hax.h:183）
 * ═══════════════════════════════════════════════════════════════════ */
typedef int64_t XNetResult;
#define XNET_OK 0

#endif /* HBOS_XJ380_COMPAT_H */
