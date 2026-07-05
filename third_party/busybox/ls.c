/* Mini ls -- hand-written, not vendored from upstream.
 *
 * Real BusyBox ls.c is ~1360 lines: terminal-width-aware multi-column
 * output, 6 sort variants, coloring, and a dozen Kconfig-gated display
 * modes. None of that fits this port's model (no ioctl(TIOCGWINSZ), no
 * ANSI-color policy elsewhere in these applets) and the whole thing is
 * one monolithic ls_main() that doesn't decompose into a small vendorable
 * core plus a swappable HBOS-specific backend the way cp/mv/rm did -- so
 * this is a small, self-contained reimplementation covering the common
 * case instead: -a (dotfiles), -l (long format: type+perms, size, name --
 * no owner/group/date column, since HBOS's VFS has no real multi-user
 * model or mutable per-file timestamps, see touch.c's same reasoning),
 * -R (recurse into subdirectories), -d (list a directory argument itself
 * rather than its contents). Entries are always one per line, sorted by
 * name -- no multi-column/terminal-width formatting.
 */
#include "libbb.h"

#define OPT_l (1 << 0)
#define OPT_a (1 << 1)
#define OPT_R (1 << 2)
#define OPT_d (1 << 3)

typedef struct {
    char name[NAME_MAX + 1];
    uint8_t type;
} ls_entry_t;

static int name_cmp(const void *a, const void *b) {
    return strcmp(((const ls_entry_t *)a)->name, ((const ls_entry_t *)b)->name);
}

static void mode_string(mode_t m, char out[11]) {
    char t = '-';
    if (S_ISDIR(m)) t = 'd';
    else if (S_ISCHR(m)) t = 'c';
    else if (S_ISBLK(m)) t = 'b';
    else if (S_ISFIFO(m)) t = 'p';
    else if (S_ISLNK(m)) t = 'l';
    else if (S_ISSOCK(m)) t = 's';
    out[0] = t;
    out[1] = (m & S_IRUSR) ? 'r' : '-';
    out[2] = (m & S_IWUSR) ? 'w' : '-';
    out[3] = (m & S_IXUSR) ? 'x' : '-';
    out[4] = (m & S_IRGRP) ? 'r' : '-';
    out[5] = (m & S_IWGRP) ? 'w' : '-';
    out[6] = (m & S_IXGRP) ? 'x' : '-';
    out[7] = (m & S_IROTH) ? 'r' : '-';
    out[8] = (m & S_IWOTH) ? 'w' : '-';
    out[9] = (m & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

static void print_entry_l(const char *fullpath, const char *name) {
    struct stat st;
    if (stat(fullpath, &st) != 0) {
        bb_perror_msg("%s", fullpath);
        printf("?????????? ?          ? %s\n", name);
        return;
    }
    char perms[11];
    mode_string(st.st_mode, perms);
    printf("%s %8ld %s\n", perms, (long)st.st_size, name);
}

/* Returns 0 on success, 1 if any error was reported (matches coreutils'
 * own exit-status convention: nonzero if anything couldn't be read). */
static int list_dir(const char *path, unsigned flags, int print_header) {
    DIR *d = opendir(path);
    if (!d) {
        bb_perror_msg("cannot access '%s'", path);
        return 1;
    }

    ls_entry_t *entries = NULL;
    size_t count = 0, cap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != 0) {
        if (ent->d_name[0] == '\0') continue;
        if (DOT_OR_DOTDOT(ent->d_name)) continue;
        if (!(flags & OPT_a) && ent->d_name[0] == '.') continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 32;
            entries = xrealloc(entries, cap * sizeof(*entries));
        }
        strncpy(entries[count].name, ent->d_name, NAME_MAX);
        entries[count].name[NAME_MAX] = '\0';
        entries[count].type = ent->d_type;
        count++;
    }
    closedir(d);

    qsort(entries, count, sizeof(*entries), name_cmp);

    if (print_header) printf("%s:\n", path);

    for (size_t i = 0; i < count; i++) {
        char *full = concat_path_file(path, entries[i].name);
        if (flags & OPT_l) print_entry_l(full, entries[i].name);
        else printf("%s\n", entries[i].name);
        free(full);
    }

    int status = 0;
    if (flags & OPT_R) {
        for (size_t i = 0; i < count; i++) {
            if (entries[i].type != DT_DIR) continue;
            char *full = concat_path_file(path, entries[i].name);
            printf("\n");
            status |= list_dir(full, flags, 1);
            free(full);
        }
    }

    free(entries);
    return status;
}

int ls_main(int argc, char **argv) {
    unsigned flags = getopt32(argv, "laRd");
    argc -= optind;
    argv += optind;

    const char *default_args[] = { ".", 0 };
    char **paths = argc ? argv : (char **)default_args;
    int npaths = argc ? argc : 1;

    int status = 0;
    int header = (npaths > 1) || (flags & OPT_R);

    for (int i = 0; i < npaths; i++) {
        struct stat st;
        if (stat(paths[i], &st) != 0) {
            bb_perror_msg("cannot access '%s'", paths[i]);
            status = 1;
            continue;
        }
        if (S_ISDIR(st.st_mode) && !(flags & OPT_d)) {
            if (i > 0) printf("\n");
            status |= list_dir(paths[i], flags, header);
        } else {
            if (flags & OPT_l) print_entry_l(paths[i], paths[i]);
            else printf("%s\n", paths[i]);
        }
    }

    return status;
}
