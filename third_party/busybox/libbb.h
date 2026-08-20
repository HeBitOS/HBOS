/* HBOS: minimal, hand-written substitute for real BusyBox's include/libbb.h.
 *
 * Real libbb.h aggregates ~150 files' worth of declarations, all gated by
 * Kconfig-generated CONFIG_ / ENABLE_ / IF_ macros — see
 * third_party/busybox/README.md for why this port hand-authors just the
 * pieces each vendored applet actually needs instead of vendoring/
 * reimplementing that whole machinery.
 *
 * Grows incrementally as more applets get added — add exactly what the
 * next applet's own .c file requires, nothing speculative.
 */
#ifndef HBOS_BUSYBOX_LIBBB_H
#define HBOS_BUSYBOX_LIBBB_H

#include <stdlib.h>   /* EXIT_SUCCESS / EXIT_FAILURE */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include "getopt.h"

typedef int exitcode_t;

/* platform.h macros real applet .c files reference directly */
#define UNUSED_PARAM __attribute__((__unused__))
/* Only matters for BusyBox's own LTO'd multi-call binary; each applet here
 * is its own standalone .hax, so this is a no-op. */
#define MAIN_EXTERNALLY_VISIBLE
#define FAST_FUNC
/* Data-alignment hint macro real applet/libbb .c files reference; not
 * meaningful for the tiny per-applet binaries built here. */
#define ALIGN1
#define ALIGN2

/* libbb helper shims — implemented in libbb_shim.c. Real BusyBox's versions
 * carry a lot more (NLS, config-gated behavior variants); these cover only
 * what the vendored applets actually call. */
void *xmalloc(size_t size);
char *stpcpy(char *dst, const char *src);
int full_write(int fd, const void *buf, size_t len);
void bb_simple_perror_msg(const char *s);
#define bb_msg_write_error "write error"

/* Vendored verbatim from upstream libbb/process_escape_sequence.c (compiled
 * as part of libbb_shim.c) — genuinely self-contained, no other libbb.c
 * dependencies. */
char bb_process_escape_sequence(const char **ptr);

char *xrealloc_getcwd_or_warn(char *cwd);
int fflush_all(void);
void *xrealloc(void *ptr, size_t size);
int fputs_stdout(const char *s);
void bb_perror_nomsg_and_die(void) __attribute__((noreturn));
void bb_simple_perror_msg_and_die(const char *s) __attribute__((noreturn));

/* which -- HBOS has no real $PATH (getenv() is always NULL, see stdlib.c),
 * so this is always what's actually searched, matching execvp()'s own
 * fallback convention (src/user/libc/unistd.c). Vendored verbatim from
 * upstream (libbb/executable.c) since it's genuinely self-contained. */
#define bb_default_root_path "/bin"
#define BB_PATH_ROOT_PATH "PATH=/bin"
int file_is_executable(const char *name);
char *find_executable(const char *filename, char **PATHp);

/* whoami/id -- HBOS now carries per-task uid/gid/euid/egid.  There is no
 * /etc/passwd database yet, so non-root UIDs are rendered numerically by
 * xuid2uname(). uid_t is declared in src/user/libc/syscall.h. */
uid_t geteuid(void);
const char *xuid2uname(long uid);

/* Real BusyBox's autoconf.h always defines every ENABLE_* symbol (0 or 1);
 * this one specifically disables pwd.c's optional -L/-P flag handling
 * (POSIX logical-vs-physical cwd) — the only caller of getopt32() (see
 * below) that needs its long-option / "opt_complementary" constraint
 * machinery, which this port does not implement. */
#define ENABLE_DESKTOP 0

/* getopt32()/getopt32long() — real, working reimplementation (not upstream's
 * own libbb/getopt32.c, which is ~630 lines with features no applet vendored
 * here uses), built on top of HBOS's own POSIX-shaped getopt() in
 * src/user/libc/getopt.c. Supports: bit-flag return value keyed by an
 * option letter's position in `opts` (skipping ':'), and one `char **`
 * destination pulled from the varargs, in `opts` string order, for every
 * letter immediately followed by ':' — covers every vendored applet's
 * actual usage. Does NOT support: "^"-prefixed opt_complementary
 * constraint strings (parsed only far enough to skip the leading '^';
 * the "\0"-separated constraint suffix, if any, is simply never reached
 * since C string literal concatenation means `opts` ends at the first
 * embedded NUL — see e.g. rm.c's `getopt32(argv, "^" "fiRrv" "\0" "f-i:i-f")`,
 * where only "fiRrv" is ever parsed), "::" optional-args, "+" numeric-only
 * args, "*" repeatable-args (llist_t), or any long-option (`--foo`) parsing
 * at all — getopt32long() here simply ignores its long-option-table
 * argument and behaves exactly like getopt32(). Do not assume any of that
 * unsupported surface works if a future applet relies on it. */
unsigned getopt32(char **argv, const char *opts, ...);
unsigned getopt32long(char **argv, const char *opts, const char *longopts, ...);
/* Real upstream getopt32()/getopt32long() also stash their return value in
 * this global, since some applets check option flags from a function
 * other than the one that called getopt32() (e.g. which.c). */
extern unsigned option_mask32;
/* Long-option-table encoding markers — needed only so vendored files'
 * long-option table string-literal-concatenation still compiles; the
 * table itself is never read (see getopt32long above). */
#define No_argument       "\0"
#define Required_argument "\001"
#define Optional_argument "\002"

/* Error-reporting helpers — real, matching upstream's "<applet_name>: <msg>"
 * format (see libbb_shim.c), minus upstream's syslog/logmode machinery. */
extern const char *applet_name;
void bb_error_msg(const char *fmt, ...);
void bb_error_msg_and_die(const char *fmt, ...) __attribute__((noreturn));
void bb_simple_error_msg(const char *s);
void bb_simple_error_msg_and_die(const char *s) __attribute__((noreturn));
void bb_perror_msg(const char *fmt, ...);
void bb_perror_msg_and_die(const char *fmt, ...) __attribute__((noreturn));
/* Real bb_show_usage() prints the applet's usage text from a table
 * generated at build time from every vendored file's "//usage:" comments
 * (usage_messages.c, itself Kconfig-driven — see README.md). Not vendored;
 * this prints a generic message instead. */
void bb_show_usage(void) __attribute__((noreturn));
void xfunc_die(void) __attribute__((noreturn));

/* stdio/fd convenience wrappers vendored applets call repeatedly. */
FILE *fopen_or_warn_stdin(const char *filename);
int fclose_if_not_stdin(FILE *fp);
int open_or_warn_stdin(const char *filename);
extern const char bb_msg_standard_input[];
extern const char *const bb_argv_dash[];
void fflush_stdout_and_exit(int status) __attribute__((noreturn));
void die_if_ferror_stdout(void);
long bb_copyfd_eof(int fd1, int fd2);
int bb_cat(char **argv);

/* Number-with-suffix parsing (head's "-n N[bkm]") — hand-written, not
 * upstream's macro-templated xatonum_template.c (which generates parsers
 * for 3 different integer widths; only the `unsigned long` one is needed
 * here). */
struct suffix_mult { const char *suffix; unsigned mult; };
extern const struct suffix_mult bkm_suffixes[];
unsigned long xatoul_sfx(const char *numstr, const struct suffix_mult *suffixes);

/* mkdir's mode-string parsing — octal only ("0755"); upstream's
 * bb_parse_mode() also accepts symbolic "u+rwx"-style mode strings via a
 * separate ~100-line parser (bb_parse_mode_symbolic in libbb/bb_parse_mode.c)
 * that no applet vendored so far needs. */
mode_t bb_parse_mode(const char *s, mode_t base);
/* mkdir -p recursive directory creation, and (below) cp/mv's copy_file() —
 * bit values match upstream's libbb.h enum exactly, in case some future
 * applet's getopt32 bit position math ever assumes it. */
#define FILEUTILS_PRESERVE_STATUS (1 << 0)
#define FILEUTILS_DEREFERENCE     (1 << 1)
#define FILEUTILS_RECUR    (1 << 2)
#define FILEUTILS_FORCE    (1 << 3)
#define FILEUTILS_INTERACTIVE (1 << 4)
#define FILEUTILS_NO_OVERWRITE    (1 << 5)
#define FILEUTILS_MAKE_HARDLINK   (1 << 6)
#define FILEUTILS_MAKE_SOFTLINK   (1 << 7)
#define FILEUTILS_DEREF_SOFTLINK  (1 << 8)
#define FILEUTILS_DEREFERENCE_L0  (1 << 9)
#define FILEUTILS_VERBOSE  (1 << 13)
#define FILEUTILS_UPDATE          (1 << 14)
#define FILEUTILS_NO_TARGET_DIR   (1 << 15)
#define FILEUTILS_TARGET_DIR      (1 << 16)
#define FILEUTILS_PRESERVE_SECURITY_CONTEXT (1 << 17)
/* cp's option string; upstream appends IF_SELINUX("c") — SELinux is off
 * here (see ENABLE_SELINUX above) so that's simply omitted. */
#define FILEUTILS_CP_OPTSTR "pdRfinlsLHarPvuTt:"
int bb_make_directory(char *path, long mode, int flags);

/* cp/mv's copy engine. Hand-written (not upstream's libbb/copy_file.c,
 * ~450 lines handling symlinks/hardlinks/SELinux/ino-dev-hashtable-based
 * recursion-loop detection/attribute preservation — none of which apply
 * to HBOS, which has no symlinks or hardlinks at all, and recursion loops
 * can't happen without symlinks): stats source, recurses into
 * directories (mkdir dest + copy each child), plain byte-copy for regular
 * files, -i/-f/-n overwrite handling. Does NOT preserve mode/owner/
 * timestamps (cp -p is a silent no-op here) or support -l/-s
 * (hard/symlink) modes. */
int copy_file(const char *source, const char *dest, int flags);
int cp_mv_stat2(const char *fn, struct stat *fn_stat, int (*sf)(const char *, struct stat *));
int cp_mv_stat(const char *fn, struct stat *fn_stat);

/* dirname/basename — hand-written dirname() (real POSIX <libgen.h>
 * function, which HBOS's libc doesn't have yet; standard semantics: may
 * modify its argument in place, "." if no '/', "/" for "/"). skip_dash_dash/
 * single_argv vendored verbatim from upstream (libbb/single_argv.c,
 * genuinely self-contained). */
char *dirname(char *path);
char **skip_dash_dash(char **argv);
char *single_argv(char **argv);

/* touch -- HBOS's VFS has no mutable file timestamp support at all (struct
 * stat's st_atime/st_mtime are filled in at query time, not stored per-file
 * and updatable), so utimensat()/futimens() here are honest no-ops that
 * just report success -- this is *not* real timestamp support, only
 * enough for touch's much more common real-world use ("make sure this
 * file exists") to work without spurious errors on already-existing
 * files. This port disables ENABLE_FEATURE_TOUCH_SUSV3 (-a/-m/-d/-t/-r),
 * so touch.c's own code never actually tries to compute a real timestamp
 * to pass in anyway. */
struct timespec { long tv_sec; long tv_nsec; };
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#define UTIME_NOW  ((1L << 30) - 1L)
#define UTIME_OMIT ((1L << 30) - 2L)
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags);
int futimens(int fd, const struct timespec times[2]);
#define xclose(fd) close(fd)

/* rm — recursive remove, path-component helpers, y/n confirmation prompt.
 * Vendored verbatim from upstream where genuinely self-contained
 * (get_last_path_component.c, concat_subpath_file.c + concat_path_file.c,
 * ask_confirmation.c); remove_file() itself is also vendored verbatim
 * (libbb/remove_file.c) since it only calls things already declared here. */
#define DOT_OR_DOTDOT(s) ((s)[0] == '.' && (!(s)[1] || ((s)[1] == '.' && !(s)[2])))
char *bb_get_last_path_component_strip(char *path);
char *concat_path_file(const char *path, const char *filename);
char *concat_subpath_file(const char *path, const char *f);
int bb_ask_y_confirmation(void);
int remove_file(const char *path, int flags);

/* wc's unicode support — this port treats everything as single-byte/ASCII
 * (HBOS's own libc has no locale/unicode machinery), matching upstream's
 * own behavior when built with unicode support off. */
enum { UNICODE_OFF = 0, UNICODE_ON = 1 };
extern int unicode_status;
static inline void init_unicode(void) { unicode_status = UNICODE_OFF; }
#define isprint_asciionly(c) ((unsigned)((c) - 0x20) <= (0x7e - 0x20))
typedef signed char smallint;

/* Feature-disable macros: real autoconf.h always defines these; this port
 * leaves the underlying ENABLE_* at the preprocessor's implicit "undefined
 * == 0", but IF_X(...)/IF_NOT_X(...) are function-like macros referenced
 * inline in vendored code regardless of whether the feature is on, so each
 * one actually used must get an explicit (here: always-off) definition. */
#define IF_SELINUX(...)
#define IF_SUID(...)
#define IF_SUID_CONFIG_QUIET(...)
#define IF_SELINUX_OR_FEATURE_USERNS(...)
#define IF_SELINUX_OR_FEATURE_USERSPEC(...)
#define IF_FEATURE_VERBOSE(...) __VA_ARGS__
#define IF_FEATURE_CATV(...)
#define IF_FEATURE_CATN(...)
#define IF_FEATURE_FANCY_HEAD(...)
#define IF_UNICODE_SUPPORT(...)
/* touch's -a/-m/-d/-t/-r (SUSv3 date/reference-file options) -- HBOS's VFS
 * has no mutable timestamps to actually set (see utimensat() above), so
 * there's nothing for these to do; touch always just acts like plain
 * SUSv2 touch (create-if-missing, ignore any requested time). */
#define IF_FEATURE_TOUCH_SUSV3(...)
#define IF_NOT_FEATURE_TOUCH_SUSV3(...) __VA_ARGS__
#define ENABLE_FEATURE_TOUCH_SUSV3 0
#define ENABLE_SELINUX 0
#define ENABLE_FEATURE_VERBOSE 1
#define ENABLE_LONG_OPTS 0
#define ENABLE_FEATURE_CATV 0
#define ENABLE_FEATURE_CATN 0
#define ENABLE_FEATURE_FANCY_HEAD 0
#define ENABLE_INCLUDE_SUSv2 0
#define ENABLE_FEATURE_WC_LARGE 1
#define ENABLE_UNICODE_SUPPORT 0
#define ENABLE_FEATURE_CLEAN_UP 1

#endif
