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
        . = ALIGN(8);
        __start_hbos_apps = .;
        KEEP(*(hbos_apps))
        . = ALIGN(8);
        __stop_hbos_apps = .;
    }

    .bss ALIGN(0x1000) : {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    . = ALIGN(0x1000);
    _end = .;
}
