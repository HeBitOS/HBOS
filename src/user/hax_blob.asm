; ============================================================
; HAX Application Blob —— Embedded via .incbin
; Generated payload: build/hax_blob.bin (by tools/genhax.py)
; Holds every compiled ./app/*.hax (ELF64) concatenated.
; ============================================================

section .rodata
align 16

%ifndef HBOS_BUILD_DIR
%define HBOS_BUILD_DIR "build"
%endif
%strcat HBOS_HAX_BLOB_PATH HBOS_BUILD_DIR, "/hax_blob.bin"

global _binary_build_hax_blob_bin_start
_binary_build_hax_blob_bin_start:
incbin HBOS_HAX_BLOB_PATH

global _binary_build_hax_blob_bin_end
_binary_build_hax_blob_bin_end:
