# Vendored BusyBox applets (Milestone 1)

Source: https://git.busybox.net/busybox
Commit: `cd8427b8ec76d212343af8725f1bed24053d087e` (2026-07-03)

## Why one `.hax` per applet, not a multi-call binary

Real BusyBox is one ELF binary with ~465 "applets" (`ls`, `cat`, `echo`, ...)
selected at runtime by `argv[0]` (`include/applets.src.h`'s `APPLET()` macros,
dispatched via `libbb/appletlib.c`'s `find_applet_by_name()`), with per-name
hardlinks/symlinks pointing at the one binary. HBOS's `.hax` convention is
already "one binary per program" (proven out by `tcc.hax`), which is a better
architectural fit here — no applet table, no argv[0] dispatch, no symlink
farm. Each wanted applet becomes its own tiny `.hax`: a small
`entry_<name>.c` that declares `HAX_APP` metadata (via `hax_applet.h`, a
smaller version of the same trick used in `third_party/tinycc/tcc.c`),
`#include`s the mostly-unmodified upstream applet `.c` file, and defines
`main(argc, argv) { return <name>_main(argc, argv); }`.

## Why no Kbuild/Kconfig

Real BusyBox's build is Linux-kernel-style Kbuild/Kconfig: a `.config` file
(450+ `CONFIG_*` flags even in the smallest pre-made config) gets compiled
by BusyBox's own `scripts/kconfig/conf` tool into `include/autoconf.h`, then
split by `scripts/basic/split-include` into `include/config/*.h`. **This
port does not compile-and-run those tools** — same reasoning as the
TinyCC port's `tccdefs_.h` decision: running a fetched project's own build
tools is "executing third-party code as part of the build," which a safety
classifier blocks. `scripts/kconfig/confdata.c` documents the exact,
purely mechanical per-symbol header format Kconfig would generate:

```c
/* enabled ("y" in .config) */
#define CONFIG_FOO 1
#define ENABLE_FOO 1
#define IF_FOO(...) __VA_ARGS__
#define IF_NOT_FOO(...)
/* disabled (not set) */
#undef CONFIG_FOO
#define ENABLE_FOO 0
#define IF_FOO(...)
#define IF_NOT_FOO(...) __VA_ARGS__
```

Instead of generating this file, **`libbb.h` in this directory is a small,
hand-written substitute** covering only what the vendored applets actually
reference — grown incrementally, one applet at a time, by reading each
applet's own source (and `Config.in` when a symbol's meaning isn't obvious
from the `.c` file alone) rather than by running Kconfig. Symbols this
port doesn't need are simply never defined; `#if ENABLE_X` checks work
fine with `ENABLE_X` left undefined (the preprocessor treats it as 0), but
`IF_X(...)`-style function-like macros used inline in code need an
explicit definition wherever a vendored file references them, even for a
disabled/dead-code branch — see `ENABLE_DESKTOP`/`getopt32` below for a
concrete example of this.

## `libbb.h` / `libbb_shim.c` — what's real vs. stubbed

Real BusyBox's `libbb/` is ~150 files of shared helpers, heavily
config-gated. This port implements a small, honest subset in
`libbb_shim.c`, plus vendors a couple of genuinely self-contained upstream
`libbb/*.c` files verbatim (`#include`d into `libbb_shim.c`, same
unity-build style as the entry files):

- **Hand-written (not upstream)**: `xmalloc`, `xrealloc` (die-on-OOM
  wrappers around `malloc`/`realloc`), `stpcpy`, `full_write`,
  `bb_simple_perror_msg`, `fflush_all` — each is a few lines, faithfully
  matching upstream's *behavior* without vendoring upstream's own (more
  config-gated) implementation.
- **Vendored verbatim from upstream** (self-contained, no other `libbb/*.c`
  dependencies): `process_escape_sequence.c` (`bb_process_escape_sequence`,
  used by `echo`'s escape-sequence handling), `xgetcwd.c`
  (`xrealloc_getcwd_or_warn`, used by `pwd`).
- **Stub only, not real** (see the comment in `libbb.h`):
  `getopt32()` — BusyBox's own variadic option-bitmask parser, used with
  wildly different call shapes across hundreds of applets. `pwd.c`'s only
  call to it is inside a dead `if (ENABLE_DESKTOP)` branch (this port sets
  `ENABLE_DESKTOP` to 0, disabling `pwd -L/-P` support) — the stub exists
  purely so that dead branch still compiles. **Do not assume this works**
  if a future applet calls it expecting real parsing; implement it for
  real at that point (HBOS already has a real `getopt()` in
  `src/user/libc/getopt.c`, added this same session, which is a more
  reasonable POSIX-shaped alternative for applets that don't specifically
  need BusyBox's own option-string dialect).

## Milestone 1 applets — verified working (2026-07-04)

`true`, `false`, `echo`, `pwd` — chosen because upstream's own NOFORK
classification flags them as needing zero process/OS calls, making them
the right place to prove the whole approach (vendoring, hand-authored
`libbb.h`, Makefile wiring) before tackling anything that needs the
process-model work (`execve`/`waitpid`/`stat` wrappers, also added to
`src/user/libc/` this same session as Milestone 0 prerequisite work, ahead
of whichever future applet ends up needing them).

Verified in QEMU via `run <name> [args]`:
- `run true` → exit 0
- `run false` → exit 1
- `run echo hello from busybox` → prints `hello from busybox`
- `run pwd` → prints `/`

`echo` here is the "basic" (non-fancy) mode — no `-n`/`-e`/`-E` flags, no
backslash-escape interpretation by default (real BusyBox's own default when
`ENABLE_FEATURE_FANCY_ECHO` is off, which this port's `libbb.h` leaves
undefined/off). The escape-sequence machinery (`bb_process_escape_sequence`)
is vendored and available regardless, since GCC still needs it declared for
that code path to compile even when it's dead at the current config.

## Milestone 2 (partial) — `mkdir`, `wc`, `head`, `cat` — verified (2026-07-04)

Added on top of Milestone 1's infrastructure:

- **`getopt32`/`getopt32long`**: real (not stub) reimplementation in
  `libbb_shim.c`, built on HBOS's own `getopt()` — see the long comment in
  `libbb.h` for exactly what subset of upstream's ~630-line
  `libbb/getopt32.c` this covers (bit-flag return + one required-arg
  pointer per `:`-suffixed letter) and what it doesn't (opt_complementary
  constraints, `::`/`+`/`*` modifiers, any actual `--long-option` parsing —
  `getopt32long` ignores its long-option table entirely).
- **Error-reporting**: `bb_error_msg[_and_die]`, `bb_perror_msg[_and_die]`,
  `bb_simple_error_msg[_and_die]`, `applet_name` (each `entry_<name>.c` now
  sets `applet_name = "<name>";` at the top of `main()`), `bb_show_usage`
  (generic message, not upstream's build-time-generated per-applet usage
  text).
- **`bb_make_directory`** (mkdir -p, hand-written, not vendored — walks
  the path creating each component, ignoring `EEXIST`), **`bb_parse_mode`**
  (octal only, e.g. `-m 0755`; upstream's symbolic `u+rwx` parser not
  implemented), `fopen_or_warn_stdin`/`fclose_if_not_stdin`/
  `open_or_warn_stdin`/`bb_copyfd_eof`/`bb_cat`/`bb_argv_dash` (cat/wc/head
  all read files via these), `xatoul_sfx`/`bkm_suffixes` (head's `-n N[bkm]`
  suffix parsing, hand-written, not upstream's macro-templated
  `xatonum_template.c`), `smallint`/`isprint_asciionly`/`init_unicode`/
  `unicode_status` (wc; this port has no real Unicode support, matching
  upstream's own behavior with Unicode support compiled off).

Verified in QEMU: `run mkdir -p /a/b/c` + `ls` confirms all 3 levels
created; `run wc -l -w -c FILE` matches expected counts; `run head -n 1
FILE`; `run cat FILE` (including a binary file, dumped raw to the
terminal, matching real `cat`); `run cat /nonexistent` correctly reports
**"No such file or directory"** (see below — this surfaced and fixed a
real, pre-existing HBOS bug).

### Bug found and fixed: HBOS user-mode libc never set `errno`

While verifying `cat`/`head` on a missing file, the error message read
`"cat: /tmp/nope.txt: Success"` instead of "No such file or directory".
Root cause (confirmed by reading the code, not guessed): `src/syscall.c`'s
`finish_syscall()` already correctly encodes a negative `-errno` in the
raw syscall return value on failure (this convention was already
documented in that file's own header comment) — but every user-mode libc
wrapper around a raw `__syscallN()` call (`open`, `stat`, `fstat`, `mkdir`,
`read`, `write`, `lseek`, `unlink`, `rmdir`, `waitpid`, `usleep`, `getcwd`,
`fopen`, `fseek` — 12 call sites across 5 files) was just doing
`ret < 0 ? -1 : ret`, silently discarding the encoded error code instead
of unpacking it into the user-mode `errno` global. Fixed generally (not
BusyBox-specifically) by adding `long __syscall_errno(long ret)` to
`src/user/libc/errno.c`/`.h` and routing all 12 call sites through it.
Also marked `exit()` `__attribute__((noreturn))` in `stdlib.h` (it
genuinely never returns — confirmed in `stdlib.c`) since GCC couldn't
otherwise prove several vendored applets' `fflush_stdout_and_exit()`-style
exit paths don't fall off the end of a non-void function; this incidentally
also fixed a pre-existing warning in TinyCC's own `_tcc_error`.

## Next steps (not done yet — see the project plan)

Milestone 2 remainder: `ls`, `rm`, `cp`, `mv` — deferred for now. `rm`
needs a real `remove_file()` (recursive tree walk: `lstat`/`S_ISLNK`/
`access`/`isatty`/interactive-confirm prompts, none of which exist in this
port yet). `cp`/`mv` need `libbb/copy_file.c`'s copy engine (several
hundred lines, handles recursion, hardlinks, symlinks, permission
preservation). `ls.c` alone is **1360 lines** upstream (columns, color,
sorting, recursion) — by far the largest single applet in the whole
target set, plausibly its own milestone. Shell applets (`ash`/`hush`),
networking applets, and anything needing POSIX regex (`grep`/`sed`/`awk`)
or `termios` remain explicitly out of scope (see the project plan file).
