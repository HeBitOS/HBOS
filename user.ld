/* HBOS user-space linker script
 *
 * Links ELF64 executables for HBOS. All code/data is placed
 * above 0x400000 to avoid kernel space.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text._start)
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }
}