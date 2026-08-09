#ifndef HBOS_USER_LIBC_FENV_H
#define HBOS_USER_LIBC_FENV_H

/* Minimal fenv for the quickjs runtime: HBOS never changes the FPU
 * rounding/exception state (the kernel owns FPU context across task
 * switches and resets MXCSR to 0x1F80), so all fenv operations are
 * no-ops and the environment type is an empty struct. */

typedef struct { unsigned int __empty; } fenv_t;

#define FE_INVALID    0x01
#define FE_DIVBYZERO  0x04
#define FE_OVERFLOW   0x08
#define FE_UNDERFLOW  0x10
#define FE_INEXACT    0x20
#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | \
                       FE_UNDERFLOW | FE_INEXACT)

#define FE_TONEAREST  0
#define FE_DOWNWARD   0x400
#define FE_UPWARD     0x800
#define FE_TOWARDZERO 0xc00

static inline int feclearexcept(int excepts) { (void)excepts; return 0; }
static inline int feraiseexcept(int excepts) { (void)excepts; return 0; }
static inline int fetestexcept(int excepts) { (void)excepts; return 0; }
static inline int fegetround(void) { return FE_TONEAREST; }
static inline int fesetround(int round) { (void)round; return 0; }
static inline int fegetenv(fenv_t *envp) { (void)envp; return 0; }
static inline int fesetenv(const fenv_t *envp) { (void)envp; return 0; }

#endif
