#include "getopt.h"
#include "stdio.h"
#include "string.h"

char *optarg;
int optind = 1;
int opterr = 1;
int optopt;

static int optpos = 1;

int getopt(int argc, char *const argv[], const char *optstring) {
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || argv[optind][1] == '\0')
        return -1;
    if (strcmp(argv[optind], "--") == 0) {
        optind++;
        return -1;
    }

    char c = argv[optind][optpos];
    const char *spec = strchr(optstring, c);

    if (c == ':' || !spec) {
        optopt = c;
        if (opterr) fprintf(stderr, "unknown option -- '%c'\n", c);
        goto advance_one;
    }

    if (spec[1] == ':') {
        if (argv[optind][optpos + 1] != '\0') {
            optarg = (char *)&argv[optind][optpos + 1];
            optind++;
        } else {
            optind++;
            if (optind >= argc) {
                optopt = c;
                if (opterr) fprintf(stderr, "option requires an argument -- '%c'\n", c);
                optpos = 1;
                return '?';
            }
            optarg = argv[optind];
            optind++;
        }
        optpos = 1;
        return c;
    }

advance_one:
    optpos++;
    if (argv[optind][optpos] == '\0') {
        optind++;
        optpos = 1;
    }
    return (c == ':' || !spec) ? '?' : c;
}
