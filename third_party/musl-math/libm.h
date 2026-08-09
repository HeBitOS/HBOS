/* Simplified internal header for the vendored musl libm subset.
 * Derived from musl 1.2.5 src/internal/libm.h (MIT), trimmed to the
 * double-precision functions HBOS's quickjs runtime needs.  No long
 * double / float variants, no fenv dependency: the fenv functions are
 * reduced to no-ops because HBOS never changes the default rounding
 * mode and the kernel owns the FPU context across task switches.
 */
#ifndef HBOS_LIBM_H
#define HBOS_LIBM_H

#include <stdint.h>
#include <float.h>
#include <math.h>
#include <fenv.h>

#define WANT_ROUNDING 0
#define WANT_SNAN 0
#define TOINT_INTRINSICS 0

#define issignalingf_inline(x) 0
#define issignaling_inline(x) 0

#ifdef __GNUC__
#define predict_true(x) __builtin_expect(!!(x), 1)
#define predict_false(x) __builtin_expect(x, 0)
#else
#define predict_true(x) (x)
#define predict_false(x) (x)
#endif

static inline double eval_as_double(double x)
{
	double y = x;
	return y;
}

#ifndef fp_barrier
#define fp_barrier fp_barrier
static inline double fp_barrier(double x)
{
	volatile double y = x;
	(void)y;
	return y;
}
#endif

#ifndef fp_force_eval
#define fp_force_eval fp_force_eval
static inline void fp_force_eval(double x)
{
	volatile double y;
	y = x;
	(void)y;
}
#endif

#define weak_alias(old, new) \
	extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))

/* musl's fenv-based helpers reduce to nothing under WANT_ROUNDING=0. */
#define FORCE_EVAL(x) do {                        \
	if (sizeof(x) == sizeof(float)) {         \
		volatile float __x = (x);         \
		(void)__x;                        \
	} else if (sizeof(x) == sizeof(double)) { \
		volatile double __x = (x);        \
		(void)__x;                        \
	} else {                                  \
		volatile long double __x = (x);   \
		(void)__x;                        \
	}                                         \
} while(0)

#define asuint(f) ((union{float _f; uint32_t _i;}){f})._i
#define asfloat(i) ((union{uint32_t _i; float _f;}){i})._f
#define asuint64(f) ((union{double _f; uint64_t _i;}){f})._i
#define asdouble(i) ((union{uint64_t _i; double _f;}){i})._f

#define EXTRACT_WORDS(hi,lo,d)                    \
do {                                              \
  uint64_t __u = asuint64(d);                     \
  (hi) = __u >> 32;                               \
  (lo) = (uint32_t)__u;                           \
} while (0)

#define GET_HIGH_WORD(hi,d)                       \
do {                                              \
  (hi) = asuint64(d) >> 32;                       \
} while (0)

#define GET_LOW_WORD(lo,d)                        \
do {                                              \
  (lo) = (uint32_t)asuint64(d);                   \
} while (0)

#define INSERT_WORDS(d,hi,lo)                     \
do {                                              \
  (d) = asdouble(((uint64_t)(hi)<<32) | (uint32_t)(lo)); \
} while (0)

#define SET_HIGH_WORD(d,hi)                       \
  INSERT_WORDS(d, hi, (uint32_t)asuint64(d))

#define SET_LOW_WORD(d,lo)                        \
  INSERT_WORDS(d, asuint64(d)>>32, lo)

#define GET_FLOAT_WORD(w,d)                       \
do {                                              \
  (w) = asuint(d);                                \
} while (0)

#define SET_FLOAT_WORD(d,w)                       \
do {                                              \
  (d) = asfloat(w);                               \
} while (0)

#define hidden __attribute__((visibility("hidden")))

hidden int    __rem_pio2_large(double*,double*,int,int,int);
hidden int    __rem_pio2(double,double*);
hidden double __sin(double,double,int);
hidden double __cos(double,double);
hidden double __tan(double,double,int);
hidden double __expo2(double,double);
hidden long double __polevll(long double, const long double *, int);

hidden double __math_xflow(uint32_t, double);
hidden double __math_uflow(uint32_t);
hidden double __math_oflow(uint32_t);
hidden double __math_divzero(uint32_t);
hidden double __math_invalid(double);

#endif /* HBOS_LIBM_H */
