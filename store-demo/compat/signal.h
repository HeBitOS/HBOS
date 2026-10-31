#ifndef HAX_COMPAT_SIGNAL_H
#define HAX_COMPAT_SIGNAL_H
#define SIGINT 2
#define SIGALRM 14
#define SA_RESTART 0
struct sigaction { void (*sa_handler)(int); int sa_flags; };
static inline void (*signal(int sig, void (*fn)(int)))(int) {
    (void)sig; (void)fn; return 0;
}
static inline int sigaction(int sig, const struct sigaction *a, struct sigaction *o) {
    (void)sig; (void)a; (void)o; return 0;
}
#endif
