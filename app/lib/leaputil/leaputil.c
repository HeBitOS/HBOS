/* app/lib/leaputil/leaputil.c —— 闰年判定实现 */
#include "leaputil.h"

int leaputil_is_leap(int year) {
    if (year % 100 == 0)
        return (year % 400 == 0) ? 1 : 0;
    return (year % 4 == 0) ? 1 : 0;
}
