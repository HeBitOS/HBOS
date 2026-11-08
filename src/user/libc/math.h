/**
 * @file    math.h
 * @brief   导出 68 个接口（1 类型 / 26 宏 / 41 函数）
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  double_t    结构体 / 类型别名
 *   宏    INFINITY      常量 / 特性宏
 *         NAN           常量 / 特性宏
 *         M_E           常量 / 特性宏
 *         M_LOG2E       常量 / 特性宏
 *         M_LOG10E      常量 / 特性宏
 *         M_LN2         常量 / 特性宏
 *         M_LN10        常量 / 特性宏
 *         M_PI          常量 / 特性宏
 *         M_PI_2        常量 / 特性宏
 *         M_PI_4        常量 / 特性宏
 *         M_1_PI        常量 / 特性宏
 *         M_2_PI        常量 / 特性宏
 *         M_2_SQRTPI    常量 / 特性宏
 *         M_SQRT2       常量 / 特性宏
 *         M_SQRT1_2     常量 / 特性宏
 *         FP_NAN        常量 / 特性宏
 *         FP_INFINITE   常量 / 特性宏
 *         FP_ZERO       常量 / 特性宏
 *         FP_SUBNORMAL  常量 / 特性宏
 *         FP_NORMAL     常量 / 特性宏
 *         fpclassify    常量 / 特性宏
 *         isfinite      常量 / 特性宏
 *         isnan         是否 NaN
 *         isinf         是否无穷
 *         isnormal      常量 / 特性宏
 *         signbit       常量 / 特性宏
 *   函数  fabs        绝对值
 *         floor       向下取整
 *         ceil        向上取整
 *         trunc       （待补）
 *         round       四舍五入
 *         sqrt        平方根
 *         fmod        浮点取余
 *         modf        （待补）
 *         copysign    （待补）
 *         fmin        取较小值
 *         fmax        取较大值
 *         remainder   （待补）
 *         scalbn      （待补）
 *         ldexp       （待补）
 *         frexp       （待补）
 *         nearbyint   （待补）
 *         lrint       （待补）
 *         rint        （待补）
 *         sin         正弦
 *         cos         余弦
 *         tan         正切
 *         asin        （待补）
 *         acos        （待补）
 *         atan        （待补）
 *         atan2       （待补）
 *         sinh        （待补）
 *         cosh        （待补）
 *         tanh        （待补）
 *         asinh       （待补）
 *         acosh       （待补）
 *         atanh       （待补）
 *         exp         自然指数
 *         exp2        （待补）
 *         expm1       （待补）
 *         log         自然对数
 *         log2        以 2 为底对数
 *         log10       以 10 为底对数
 *         log1p       （待补）
 *         pow         幂
 *         cbrt        （待补）
 *         hypot       （待补）
 */
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
