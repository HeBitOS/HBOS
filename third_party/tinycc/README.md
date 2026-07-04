# Vendored TinyCC (tcc)

Source: https://repo.or.cz/tinycc.git
Commit: `a338258d309c888bde96b2d1f206299231a54ddf` (2026-06-13, tag `0.9.28rc`)

## What's vendored and why

Only the files needed for TinyCC's **native x86_64 → x86_64 ELF** compiler,
using its default unity-build mode (`ONE_SOURCE=1`, the upstream default):
compiling `tcc.c` alone pulls in everything else via `#include "*.c"`.

- `tcc.c` — entry point / CLI (patched: see below)
- `tcctools.c`, `libtcc.c`, `tccpp.c`, `tccgen.c`, `tccdbg.c`, `tccasm.c`,
  `tccelf.c`, `tccrun.c` — core compiler, shared across all targets
- `x86_64-gen.c`, `x86_64-link.c`, `i386-asm.c` — the x86_64 codegen/linker
  backend (x86_64 mode reuses the i386 inline-assembler)
- `tcc.h`, `tcctok.h`, `i386-asm.h`, `x86_64-asm.h`, `elf.h`, `stab.h`,
  `dwarf.h`, `libtcc.h` — internal headers these need
- `config.h` — hand-written (TCC's own `configure` script isn't used; HBOS
  only ever builds this one fixed target, so a static config.h is simpler
  and more reviewable than running a shell script). See that file for what's
  deliberately turned on/off and why.
- `tccdefs_.h` — TinyCC's built-in predefined macros/typedefs
  (`__SIZE_TYPE__`, `__builtin_*` decls, etc.), embedded as a C string and
  compiled in (`CONFIG_TCC_PREDEFS=1`), so TCC doesn't need to load a real
  `tccdefs.h` file from HBOS's filesystem at runtime (HBOS's ramfs starts
  empty at boot with no seeded files). Generated from upstream's
  `include/tccdefs.h` — see "regenerating tccdefs_.h" below.

**Deliberately not vendored**: everything ARM/ARM64/RISC-V/C67/PE(win32)/
Mach-O(macOS)-specific, the test suite, `docs/`, `win32/`, `contrib/`.

## Patches vs. upstream

All patches are marked with `HBOS:` comments in the vendored source.

**`tcc.c`**
1. A `HAX_APP("tcc", ...)` metadata declaration so `tools/genhax.py` picks up
   the compiled binary automatically (see `app/include/hax.h`).
2. Auto-appending HBOS's own prelinked runtime bundle to the link step (see
   "Linking user output" below) unless `-nostdlib` is passed, so
   `tcc foo.c -o foo` works with no extra flags — matching the ergonomics of
   a normal `gcc`/`clang` invocation.
3. `strtod`/`strtof`/`strtold`/`ldexpl` implementations (HBOS's general libc
   has no floating-point support at all — see "Why tcc.c needs real SSE2"
   below — these live here, in the one file that's compiled with it).

**`tcc.h`**: forces `TCC_IS_NATIVE` back off. It auto-defines when the build
host's architecture matches the target (true for us: x86_64 building for
`TCC_TARGET_X86_64`), which pulls in `tccrun.c`'s entire `-run`/JIT
in-process-execution path (host `dlopen`/`mmap(PROT_EXEC)`-style dynamic
symbol resolution) — HBOS's port deliberately doesn't support that; compile
to a file, then run it separately via the existing `elf64_load_and_spawn`.

**`tccelf.c`**: `tccelf_add_crtbegin()`/`tccelf_add_crtend()`/
`tcc_add_runtime()` no longer search for `crt1.o`/`crti.o`/`crtn.o` or
`-lc` — HBOS has no `/usr/lib` tree at all; the auto-linked
`hbos_runtime.o` bundle already provides everything those would have.

**`x86_64-link.c`**: `ELF_START_ADDR` changed from the Linux default
`0x400000` to `0x1000000000` (see "Linking user output" below) —
low addresses like `0x400000` aren't mapped/available in a freshly created
HBOS user address space, so compiled output failed to load
("segment map failed") until this matched HBOS's own convention.

## Why tcc.c needs real SSE2, and what that required elsewhere

TCC's own source does real `double`/`float` arithmetic (constant-folding
float literals in `tccgen.c`/`tccpp.c`), which needs hardware floating
point — every other `.hax` app builds with
`-mno-80387 -mno-mmx -mno-sse -mno-sse2` (see `Makefile` `USER_CFLAGS`)
since nothing else needs it, but `tcc.hax` gets its own `TCC_CFLAGS`
without those flags (default SSE2).

This isn't just a build-flag choice: HBOS's kernel never actually turned
SSE on at the CPU level (`CR0.EM`/`CR4.OSFXSR` etc. were never touched
anywhere), so an SSE instruction would raise `#UD` (Invalid Opcode)
regardless of what flags compiled it — confirmed by hitting exactly that
crash before adding `fpu_enable()` (`src/core/cpu.h`, called once from
`src/kernel.c` right after `gdt_idt_init()`).

Known, accepted follow-up: `src/core/task_switch.asm` still doesn't
save/restore XMM registers on a context switch. In practice this is safe
today because `tcc.hax` is the only program that touches SSE — nothing else
can clobber its XMM state between time slices — but it's not a general
solution. Add FXSAVE/XRSTOR to `task_switch.asm` if a second FP-using
program is ever added.

## Linking user output

TCC defaults to producing static, non-PIE `ET_EXEC` ELF binaries — this
matches `src/elf.c`'s `elf64_load_and_spawn` (reads `PT_LOAD`
segments/vaddr directly, no relocation support). Its *load address*
needed changing to match HBOS's convention (see `x86_64-link.c` patch
above), but no PIE-related flags are needed.

What TCC-compiled user programs need is HBOS's own `_start`/libc, not
glibc's crt1.o/crti.o/crtn.o + libc.so (see the `tccelf.c` patch above —
that whole search is skipped). HBOS has no persistent filesystem seeded at
boot, so instead of expecting real crt files to exist on disk, the build
produces one combined relocatable object,
`build/tcc/hbos_runtime.o` (`$(LD) -r` over `$(USER_LIBC_OBJS)` +
`crt0.o`), which gets embedded into the kernel image (same `.incbin`
pattern as the wallpaper/font blobs — see `src/user/tcc_runtime_blob.asm`)
and written out to a fixed ramfs path once at boot
(`src/tools/tcc_runtime_seed.c`). TCC's own link step appends that path
automatically (see the `tcc.c` patch above). No `libtcc1.a` (TCC's own
helper-routine archive) either — `config.h` sets `TCC_LIBTCC1 ""`; x86_64
has native instructions for the division/conversion cases that archive
exists for on 32-bit targets, so this is expected to be fine for normal
programs. Revisit (build and bundle a real one) if a compiled program ever
fails to link with an undefined `__divdi3`/`__fixdfdi`/etc-style reference.

## Verified working end to end (2026-07-04)

Compiled and ran, in QEMU, via `run tcc <file>.c -o <out>` then `run <out>`:
- `int main(){return 42;}` → exit code 42
- `printf("...%d...", 7)` → correct formatted output (varargs work)
- a `for` loop summing 1..10 → exit code 55 (correct)

## A real, pre-existing HBOS libc bug this port found

`src/user/libc/string.c`'s `strchr`/`strrchr` didn't handle `c == 0`
(searching for the string's own NUL terminator) — standard, well-defined
behavior (`strchr(s, 0)` should return a pointer to the terminator, not
NULL) that nothing had ever exercised before. TinyCC's own
`tcc_basename()` calls exactly this (`strchr(name, 0)`, then walks
backward looking for a path separator), and got NULL instead, which
cascaded into a `strrchr(NULL, '.')` crash inside `tcc_fileextension()`.
Fixed in `string.c` — this was a real bug in general-purpose code, not
something specific to the TCC port.

## Regenerating tccdefs_.h

`tccdefs_.h` is normally produced by TinyCC's own build (`c2str.exe`,
compiled from `conftest.c` with `-DC2STR`) converting `include/tccdefs.h`
into an escaped, quoted C string. HBOS's build does **not** compile and run
TinyCC's own `conftest.c` for this (that would mean executing fetched
third-party code as part of the build) — instead a small standalone script,
reimplementing the same (simple, well-defined) line-based text conversion
by hand, produced the vendored `tccdefs_.h` from upstream's
`include/tccdefs.h`. If TinyCC is ever re-vendored at a newer commit,
`tccdefs_.h` needs regenerating the same way from the new `include/tccdefs.h`.
