/* HBOS: hand-written, not vendored from upstream (upstream TinyCC ships no
 * stdint.h at all — see third_party/tinycc/README.md; it doesn't need one
 * to compile itself, but compiled *user* programs still `#include
 * <stdint.h>`). Standard fixed-width integer typedefs for x86_64. */
#ifndef _STDINT_H
#define _STDINT_H

typedef signed char        int8_t;
typedef short               int16_t;
typedef int                 int32_t;
typedef long                int64_t;
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long       uint64_t;

typedef long                intptr_t;
typedef unsigned long       uintptr_t;
typedef long                intmax_t;
typedef unsigned long       uintmax_t;

#define INT8_MIN   (-128)
#define INT16_MIN  (-32768)
#define INT32_MIN  (-2147483647 - 1)
#define INT64_MIN  (-9223372036854775807L - 1)
#define INT8_MAX   127
#define INT16_MAX  32767
#define INT32_MAX  2147483647
#define INT64_MAX  9223372036854775807L
#define UINT8_MAX  255
#define UINT16_MAX 65535
#define UINT32_MAX 4294967295U
#define UINT64_MAX 18446744073709551615UL

#define SIZE_MAX   UINT64_MAX

#endif /* _STDINT_H */
