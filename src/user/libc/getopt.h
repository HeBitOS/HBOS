#ifndef HBOS_USER_LIBC_GETOPT_H
#define HBOS_USER_LIBC_GETOPT_H

/* Short-option getopt only (no getopt_long) — enough for the BusyBox
 * applets targeted so far; add getopt_long() if/when one needs it. */
extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt(int argc, char *const argv[], const char *optstring);

#endif
