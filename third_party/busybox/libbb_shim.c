/* HBOS: hand-written implementations of the small set of BusyBox libbb.h
 * helpers the vendored applets actually call — see libbb.h. */
#include "libbb.h"
#include "getopt.h"
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>

int unicode_status = UNICODE_OFF;
const char bb_msg_standard_input[] = "standard input";
const char *const bb_argv_dash[] = { "-", NULL };

static unsigned vgetopt32(char **argv, const char *opts, va_list ap) {
    if (*opts == '^') opts++;

    char **arg_dest[256] = {0};
    int bit_of[256] = {0};
    int bitpos = 0;
    for (const char *p = opts; *p; p++) {
        if (*p == ':') continue;
        bit_of[(unsigned char)*p] = bitpos;
        if (p[1] == ':') {
            arg_dest[(unsigned char)*p] = va_arg(ap, char **);
        }
        bitpos++;
    }

    int argc = 0;
    while (argv[argc]) argc++;

    unsigned flags = 0;
    optind = 1;
    int c;
    while ((c = getopt(argc, argv, opts)) != -1) {
        if (c == '?') continue;
        unsigned char uc = (unsigned char)c;
        flags |= (1u << bit_of[uc]);
        if (arg_dest[uc]) *arg_dest[uc] = optarg;
    }
    return flags;
}

unsigned getopt32(char **argv, const char *opts, ...) {
    va_list ap;
    va_start(ap, opts);
    unsigned r = vgetopt32(argv, opts, ap);
    va_end(ap);
    return r;
}

unsigned getopt32long(char **argv, const char *opts, const char *longopts, ...) {
    (void)longopts;
    va_list ap;
    va_start(ap, longopts);
    unsigned r = vgetopt32(argv, opts, ap);
    va_end(ap);
    return r;
}

const char *applet_name = "hbos-applet";

void xfunc_die(void) {
    exit(EXIT_FAILURE);
}

void bb_error_msg(const char *fmt, ...) {
    fflush_all();
    fprintf(stderr, "%s: ", applet_name);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

void bb_error_msg_and_die(const char *fmt, ...) {
    fflush_all();
    fprintf(stderr, "%s: ", applet_name);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    xfunc_die();
}

void bb_simple_error_msg(const char *s) {
    bb_error_msg("%s", s);
}

void bb_simple_error_msg_and_die(const char *s) {
    bb_error_msg_and_die("%s", s);
}

void bb_perror_msg(const char *fmt, ...) {
    fflush_all();
    fprintf(stderr, "%s: ", applet_name);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ": %s\n", strerror(errno));
}

void bb_perror_msg_and_die(const char *fmt, ...) {
    fflush_all();
    fprintf(stderr, "%s: ", applet_name);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ": %s\n", strerror(errno));
    xfunc_die();
}

void bb_show_usage(void) {
    fprintf(stderr, "%s: invalid usage\n", applet_name);
    exit(EXIT_FAILURE);
}

FILE *fopen_or_warn_stdin(const char *filename) {
    if (filename == bb_msg_standard_input || (filename[0] == '-' && filename[1] == '\0'))
        return stdin;
    FILE *fp = fopen(filename, "r");
    if (!fp) bb_perror_msg("%s", filename);
    return fp;
}

int fclose_if_not_stdin(FILE *fp) {
    if (fp == stdin) return 0;
    return fclose(fp);
}

int open_or_warn_stdin(const char *filename) {
    if (filename == bb_msg_standard_input || (filename[0] == '-' && filename[1] == '\0'))
        return STDIN_FILENO;
    int fd = open(filename, O_RDONLY);
    if (fd < 0) bb_perror_msg("%s", filename);
    return fd;
}

void fflush_stdout_and_exit(int status) {
    fflush(stdout);
    exit(status);
}

void die_if_ferror_stdout(void) {
    if (ferror(stdout)) {
        bb_error_msg_and_die("%s", "I/O error");
    }
}

long bb_copyfd_eof(int fd1, int fd2) {
    char buf[4096];
    long total = 0;
    for (;;) {
        long n = read(fd1, buf, sizeof(buf));
        if (n < 0) return -1;
        if (n == 0) break;
        if (full_write(fd2, buf, (size_t)n) != n) return -1;
        total += n;
    }
    return total;
}

int bb_cat(char **argv) {
    int retval = EXIT_SUCCESS;
    if (!*argv) argv = (char **)bb_argv_dash;
    do {
        int fd = open_or_warn_stdin(*argv);
        if (fd >= 0) {
            long r = bb_copyfd_eof(fd, STDOUT_FILENO);
            if (fd != STDIN_FILENO) close(fd);
            if (r >= 0) continue;
        }
        retval = EXIT_FAILURE;
    } while (*++argv);
    return retval;
}

const struct suffix_mult bkm_suffixes[] = {
    { "b", 512 }, { "k", 1024 }, { "m", 1024*1024 }, { "", 0 }
};

unsigned long xatoul_sfx(const char *numstr, const struct suffix_mult *suffixes) {
    char *end;
    unsigned long v = strtoul(numstr, &end, 10);
    if (end == numstr) {
        bb_error_msg_and_die("invalid number '%s'", numstr);
    }
    for (const struct suffix_mult *s = suffixes; s->suffix[0]; s++) {
        if (strcmp(end, s->suffix) == 0) return v * s->mult;
    }
    if (*end != '\0') {
        bb_error_msg_and_die("invalid number '%s'", numstr);
    }
    return v;
}

mode_t bb_parse_mode(const char *s, mode_t base) {
    (void)base;
    char *end;
    long m = strtol(s, &end, 8);
    if (*end != '\0' || m < 0) return (mode_t)-1;
    return (mode_t)m;
}

int bb_make_directory(char *path, long mode, int flags) {
    mode_t m = (mode == -1) ? 0777 : (mode_t)mode;

    if (!(flags & FILEUTILS_RECUR)) {
        if (mkdir(path, m) < 0) {
            bb_perror_msg("can't create directory '%s'", path);
            return -1;
        }
        if (flags & FILEUTILS_VERBOSE) {
            printf("created directory: '%s'\n", path);
        }
        return 0;
    }

    /* -p: create parents as needed, ignore EEXIST at any level */
    char buf[256];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) {
        bb_error_msg("path too long: '%s'", path);
        return -1;
    }
    strcpy(buf, path);

    for (size_t i = 1; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';
            if (buf[0] != '\0' && mkdir(buf, m) < 0 && errno != EEXIST) {
                bb_perror_msg("can't create directory '%s'", buf);
                return -1;
            }
            buf[i] = saved;
        }
    }
    if (flags & FILEUTILS_VERBOSE) {
        printf("created directory: '%s'\n", path);
    }
    return 0;
}

void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p) {
        /* real BusyBox's xfunc_die() prints bb_msg_memory_exhausted then
         * exits(xfunc_error_retval) (default 1) — same effect, simpler. */
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

char *stpcpy(char *dst, const char *src) {
    while ((*dst = *src)) { dst++; src++; }
    return dst;
}

int full_write(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t done = 0;
    while (done < len) {
        long n = write(fd, p + done, len - done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return (int)done;
}

void bb_simple_perror_msg(const char *s) {
    fprintf(stderr, "%s: %s\n", s, strerror(errno));
}

void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (!p && size != 0) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

int fflush_all(void) {
    return fflush(NULL);
}

/* Hand-written (not upstream) — small enough that vendoring upstream's
 * own concat_path_file.c (which needs xasprintf + last_char_is) wasn't
 * worth the extra dependency chain. */
static char *last_char_is(const char *s, int c) {
    if (!s || !*s) return NULL;
    char *p = (char *)s + strlen(s) - 1;
    return (*p == (char)c) ? p : NULL;
}

char *concat_path_file(const char *path, const char *filename) {
    if (!path) path = "";
    int need_slash = (last_char_is(path, '/') == NULL);
    while (*filename == '/') filename++;
    size_t len = strlen(path) + need_slash + strlen(filename) + 1;
    char *out = xmalloc(len);
    strcpy(out, path);
    if (need_slash) strcat(out, "/");
    strcat(out, filename);
    return out;
}

char *concat_subpath_file(const char *path, const char *f) {
    if (f && DOT_OR_DOTDOT(f)) return NULL;
    return concat_path_file(path, f);
}

char *bb_get_last_path_component_strip(char *path) {
    char *slash = last_char_is(path, '/');
    if (slash) {
        while (*slash == '/' && slash != path) *slash-- = '\0';
    }
    slash = strrchr(path, '/');
    if (!slash || (slash == path && !slash[1])) return path;
    return slash + 1;
}

int bb_ask_y_confirmation(void) {
    fflush_all();
    int first = 0, c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (!first && c != ' ' && c != '\t') first = c | 0x20;
    }
    return first == 'y';
}

int cp_mv_stat2(const char *fn, struct stat *fn_stat, int (*sf)(const char *, struct stat *)) {
    if (sf(fn, fn_stat) < 0) {
        if (errno != ENOENT) {
            bb_perror_msg("can't stat '%s'", fn);
            return -1;
        }
        return 0;
    }
    if (S_ISDIR(fn_stat->st_mode)) return 3;
    return 1;
}

int cp_mv_stat(const char *fn, struct stat *fn_stat) {
    return cp_mv_stat2(fn, fn_stat, stat);
}

static int copy_regular_file(const char *source, const char *dest, int flags) {
    struct stat dest_stat;
    if (lstat(dest, &dest_stat) == 0) {
        if (flags & FILEUTILS_NO_OVERWRITE) return 0;
        if (flags & FILEUTILS_INTERACTIVE) {
            fprintf(stderr, "%s: overwrite '%s'? ", applet_name, dest);
            if (!bb_ask_y_confirmation()) return 0;
        }
    }

    int sfd = open(source, O_RDONLY);
    if (sfd < 0) {
        bb_perror_msg("can't open '%s'", source);
        return -1;
    }
    int dfd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0) {
        bb_perror_msg("can't create '%s'", dest);
        close(sfd);
        return -1;
    }

    char buf[8192];
    int ok = 1;
    for (;;) {
        long n = read(sfd, buf, sizeof(buf));
        if (n < 0) { bb_perror_msg("can't read '%s'", source); ok = 0; break; }
        if (n == 0) break;
        if (full_write(dfd, buf, (size_t)n) != n) {
            bb_perror_msg("can't write '%s'", dest);
            ok = 0;
            break;
        }
    }
    close(sfd);
    close(dfd);
    return ok ? 0 : -1;
}

int copy_file(const char *source, const char *dest, int flags) {
    struct stat source_stat, dest_stat;

    if (lstat(source, &source_stat) < 0) {
        bb_perror_msg("can't stat '%s'", source);
        return -1;
    }

    if (lstat(dest, &dest_stat) == 0) {
        if (source_stat.st_dev == dest_stat.st_dev &&
            source_stat.st_ino == dest_stat.st_ino) {
            bb_error_msg("'%s' and '%s' are the same file", source, dest);
            return -1;
        }
    }

    if (S_ISDIR(source_stat.st_mode)) {
        if (!(flags & FILEUTILS_RECUR)) {
            bb_error_msg("omitting directory '%s'", source);
            return -1;
        }
        if (mkdir(dest, 0777) < 0 && errno != EEXIST) {
            bb_perror_msg("can't create directory '%s'", dest);
            return -1;
        }

        DIR *dp = opendir(source);
        if (!dp) {
            bb_perror_msg("can't open '%s'", source);
            return -1;
        }
        int retval = 0;
        struct dirent *d;
        while ((d = readdir(dp)) != NULL) {
            if (DOT_OR_DOTDOT(d->d_name)) continue;
            char *new_source = concat_path_file(source, d->d_name);
            char *new_dest = concat_path_file(dest, d->d_name);
            if (copy_file(new_source, new_dest, flags) < 0) retval = -1;
            free(new_source);
            free(new_dest);
        }
        closedir(dp);
        return retval;
    }

    return copy_regular_file(source, dest, flags);
}

/* Vendored verbatim from upstream — see the declarations in libbb.h. */
#include "process_escape_sequence.c"
#include "xgetcwd.c"
#include "remove_file.c"
