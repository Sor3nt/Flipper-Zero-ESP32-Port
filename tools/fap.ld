/*
 * Linker script for FAP relocatable ELF files.
 * Merges the many per-function sections into a few combined sections
 * to keep section count manageable for the ELF loader.
 */
SECTIONS
{
    .text : {
        *(.literal .literal.*)
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
        *(.rodata.str1.1 .rodata.str1.4)
        *(.srodata .srodata.*)
    }

    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
    }

    .bss : {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    }

    .init_array : {
        KEEP(*(.init_array .init_array.*))
    }

    .fini_array : {
        KEEP(*(.fini_array .fini_array.*))
    }

    .preinit_array : {
        KEEP(*(.preinit_array .preinit_array.*))
    }
}
