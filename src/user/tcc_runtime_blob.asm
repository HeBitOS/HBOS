; HBOS TinyCC runtime bundle — embedded via .incbin, same pattern as
; src/graphics/gui_wallimg.asm. build/tcc/hbos_runtime.o is crt0.o + all of
; HBOS's user-mode libc, `ld -r`'d together into one relocatable object, so
; TinyCC-compiled user programs have something to link against (HBOS has no
; persistent filesystem seeded at boot — this gets written into ramfs once
; at kernel init, see src/tools/tcc_runtime_seed.c).
section .rodata
bits 64

%ifndef HBOS_BUILD_DIR
%define HBOS_BUILD_DIR "build"
%endif
%strcat HBOS_TCC_RUNTIME_PATH HBOS_BUILD_DIR, "/tcc/hbos_runtime.o"

global _binary_build_tcc_hbos_runtime_o_start
_binary_build_tcc_hbos_runtime_o_start:
incbin HBOS_TCC_RUNTIME_PATH

global _binary_build_tcc_hbos_runtime_o_end
_binary_build_tcc_hbos_runtime_o_end:
