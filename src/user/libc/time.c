#include "time.h"
#include "syscall.h"
#include "sys/time.h"

time_t time(time_t *tloc) {
    struct timeval tv;
    __syscall1(HBOS_SYS_GETTOD, (long)&tv);
    if (tloc) *tloc = (time_t)tv.tv_sec;
    return (time_t)tv.tv_sec;
}

static struct tm g_localtime_tm;

struct tm *localtime_r(const time_t *timer, struct tm *result) {
    /* HBOS has no timezone: treat as UTC (same as localtime). */
    return localtime(timer), result ? result : &g_localtime_tm;
}

int clock_gettime(clockid_t clockid, struct timespec *tp) {
    if (!tp) return -1;
    return (int)__syscall3(HBOS_SYS_CLOCK_GETTIME, (long)clockid, (long)tp, 0);
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv) return -1;
    __syscall1(HBOS_SYS_GETTOD, (long)tv);
    return 0;
}

/* Howard Hinnant's days_from_civil / civil_from_days (public-domain-style
 * calendar algorithm), same one used kernel-side in src/rtc_tz.h —
 * duplicated here rather than shared since that header is kernel-only. */
static long civil_from_days_wday(long z) {
    return ((z % 7) + 7 + 4) % 7; /* z=0 (1970-01-01) was a Thursday = 4 */
}

static void civil_from_days(long z, long *y, int *m, int *d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long dd = doy - (153 * mp + 2) / 5 + 1;
    long mm = mp + (mp < 10 ? 3 : -9);
    *y = yy + (mm <= 2);
    *m = (int)mm;
    *d = (int)dd;
}

struct tm *localtime(const time_t *timer) {
    static struct tm result;
    long t = timer ? (long)*timer : 0;
    long days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    long sod = t - days * 86400;

    long y; int m, d;
    civil_from_days(days, &y, &m, &d);

    result.tm_sec = (int)(sod % 60);
    result.tm_min = (int)((sod / 60) % 60);
    result.tm_hour = (int)(sod / 3600);
    result.tm_mday = d;
    result.tm_mon = m - 1;
    result.tm_year = (int)(y - 1900);
    result.tm_wday = (int)civil_from_days_wday(days);
    result.tm_yday = 0;
    result.tm_isdst = 0;
    return &result;
}
