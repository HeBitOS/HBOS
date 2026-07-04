#ifndef HBOS_USER_LIBC_MATH_H
#define HBOS_USER_LIBC_MATH_H

/* Minimal stub — TinyCC's core only needs this header to exist (for type/
 * macro compatibility during its own compilation), it doesn't call real
 * libm functions. Add real declarations here if something is later found
 * to actually need them. */
#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))

#endif
