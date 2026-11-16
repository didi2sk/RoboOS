ENTRY(_app_header)
SECTIONS
{
    . = 0x01000000;                 /* APP_BASE */
    .text : {
        KEEP(*(.text.header))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss (NOLOAD) : {
        . = ALIGN(16);
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(16);
    __app_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
