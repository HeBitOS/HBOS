#ifndef HBOS_USER_LIBC_MATH_H
#define HBOS_USER_LIBC_MATH_H

/* Double-precision libm for the quickjs runtime (vendored musl subset,
 * third_party/musl-math).  TinyCC's core only needs the macros for type
 * compatibility; the functions are linked in only when a program actually
 * calls them. */

#include <float.h>   /* gcc: HUGE_VAL/INFINITY/NAN */

#ifndef INFINITY
#define INFINITY (__builtin_inff())
#endif
#ifndef NAN
#define NAN (__builtin_nanf(""))
#endif

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, \
                                           FP_SUBNORMAL, FP_ZERO, (x))
#define isfinite(x) __builtin_isfinite(x)
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x)  __builtin_signbit(x)

typedef double double_t;   /* musl libm files use this C99 type */

double fabs(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double sqrt(double x);
double fmod(double x, double y);
double modf(double x, double *iptr);
double copysign(double x, double y);
double fmin(double x, double y);
double fmax(double x, double y);
double remainder(double x, double y);
double scalbn(double x, int n);
double ldexp(double x, int n);
double frexp(double x, int *e);
double nearbyint(double x);
long lrint(double x);
double rint(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);
double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double pow(double x, double y);
double cbrt(double x);
double hypot(double x, double y);

#endif
