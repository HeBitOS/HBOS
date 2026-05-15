OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS {
    . = 1M;

    .multiboot ALIGN(8) : {
        *(.multiboot)
    }

    .text ALIGN(0x1000) : {
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(0x1000) : {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(0x1000) : {
        *(.data)
        *(.data.*)
    }

    .bss ALIGN(0x1000) : {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }
}
