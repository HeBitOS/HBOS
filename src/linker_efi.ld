/* UEFI Linker Script for EFI PE format */

ENTRY(efi_main)

OUTPUT_FORMAT("elf64-x86-64")

SECTIONS {
    . = 0x0;

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.gnu_hash)
    }
}

