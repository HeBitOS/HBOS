#ifndef HAX_COMPAT_SYS_TIME_H
#define HAX_COMPAT_SYS_TIME_H
#include <time.h>
struct timeval { long tv_sec; long tv_usec; };
struct itimerval { struct timeval it_interval; struct timeval it_value; };
#define ITIMER_REAL 0
static inline int setitimer(int which, const struct itimerval *n, struct itimerval *o) {
    (void)which; (void)n; (void)o; return 0;   /* HAX: main loop drives handleTimer */
}
#endif
