/* Hand-written config.h for HBOS's vendored TinyCC build.
 * TCC normally generates this via its own `configure` shell script; HBOS
 * doesn't use that (no host autoconf-style detection needed — we always
 * build for exactly one target: native x86_64, producing HBOS .hax ELF64
 * executables). See third_party/tinycc/README.md for the vendored version
 * and what was deliberately left out (win32/PE, cross targets, -run/JIT). */
#ifndef HBOS_TCC_CONFIG_H
#define HBOS_TCC_CONFIG_H

#define TCC_VERSION "0.9.28rc-hbos"

/* Only x86_64, only the native (non-cross) compiler. */
#define TCC_TARGET_X86_64 1

/* Static non-PIE ET_EXEC output (default when CONFIG_TCC_PIE is undefined) —
 * matches src/elf.c's elf64_load_and_spawn, which reads PT_LOAD/vaddr
 * directly with no relocation support. Leave CONFIG_TCC_PIE undefined. */

/* Embed tccdefs.h as a compile-time C string (third_party/tinycc/tccdefs_.h,
 * generated from upstream's include/tccdefs.h — see README.md) instead of
 * loading it from a runtime file. HBOS has no seeded filesystem content at
 * boot, so runtime file-load (the CONFIG_TCC_PREDEFS=0 path) isn't viable
 * without extra infrastructure; the embedded-string path needs nothing else. */
#define CONFIG_TCC_PREDEFS 1

/* HBOS's .hax apps have no shared-library/dynamic-loading support at all
 * (statically linked always) — this skips tcc.h's `#include <dlfcn.h>` and
 * the RTLD_*-using dlopen() plumbing entirely, matching reality. */
#define CONFIG_TCC_STATIC 1

/* HBOS apps are single-threaded (no pthreads) — the semaphore-based locking
 * TCC normally wraps its global compiler state in (for safe multi-threaded
 * library use) can't apply and isn't needed; WAIT_SEM/POST_SEM become no-ops. */
#define CONFIG_TCC_SEMLOCK 0

/* No libtcc1.a on HBOS (TCC's own small helper-routine archive — division/
 * conversion helpers mostly relevant to 32-bit x86; x86_64 has native
 * instructions for the common cases, so skipping this is expected to be
 * fine for normal programs). Empty string makes tcc_add_runtime()'s
 * `if (TCC_LIBTCC1[0]) tcc_add_support(...)` a no-op instead of erroring
 * on a file HBOS doesn't have. Revisit (build & bundle a real one) if a
 * compiled program ever fails to link with an undefined __divdi3/__fixdfdi/
 * etc-style reference. */
#define TCC_LIBTCC1 ""

/* Deliberately NOT defined (leave TCC's #ifndef defaults / #ifdef-gated
 * features off):
 *   TCC_IS_NATIVE      - tccrun.c's -run/JIT execution path is entirely
 *                         `#ifdef TCC_IS_NATIVE`-gated; leaving it undefined
 *                         compiles tccrun.c down to nothing. HBOS's port is
 *                         "compile to a file, then `run` it separately via
 *                         the existing elf64_load_and_spawn loader" — no
 *                         in-process JIT execution needed.
 *   CONFIG_TCC_BACKTRACE, CONFIG_TCC_BCHECK, CONFIG_TCC_ASM (inline asm),
 *   CONFIG_TCC_STATIC  - not needed for the initial port; can be revisited
 *                         later if a real need shows up.
 *   CONFIG_TCC_SEMLOCK - only matters for multi-threaded TCC library use.
 */

/* Where TCC looks for default sysinclude/lib paths and its own crt objects.
 * HBOS has no "/usr" tree; the vendored tcc.c always appends HBOS's own
 * prelinked runtime bundle (see README.md "Linking user output") instead of
 * relying on CONFIG_TCC_CRTPREFIX/CONFIG_TCC_LIBPATHS at all, so these are
 * left at TCC's built-in fallback values and effectively unused. */

#endif /* HBOS_TCC_CONFIG_H */
