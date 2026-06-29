; ============================================================
; HAX Application Blob —— Embedded via .incbin
; Generated payload: build/hax_blob.bin (by tools/genhax.py)
; Holds every compiled ./app/*.hax (ELF64) concatenated.
; ============================================================

section .rodata
align 16

global _binary_build_hax_blob_bin_start
_binary_build_hax_blob_bin_start:
incbin "build/hax_blob.bin"

global _binary_build_hax_blob_bin_end
_binary_build_hax_blob_bin_end:
