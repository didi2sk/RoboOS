ENTRY(_start)

SECTIONS
{
    . = 0x80000;
    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    . = ALIGN(16);
    __bss_start = .;
    .bss (NOLOAD) : {
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(4096);
    __bss_end = .;

    /* MMU page tables (4 KB aligned) */
    __pt_start = .;
    . += 2 * 4096;
    __kernel_end = .;

    /* 2 MB block for DMA buffers (USB) - mapped non-cacheable */
    . = ALIGN(0x200000);
    __dma_start = .;
    . += 0x200000;
    __dma_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
__bss_size = (__bss_end - __bss_start) >> 3;
