/*
 * Program header + entry point. The kernel loads the .bin at APP_BASE,
 * checks the magic, zeroes the BSS and jumps to offset 0 with x0 = api.
 */
.section ".text.header"
.global _app_header
_app_header:
    b       _start                  /* +0  entry */
    .word   1                       /* +4  header version */
    .ascii  "ROBOAPP1"              /* +8  magic */
    .quad   __app_end               /* +16 end of image incl. BSS */
    .quad   0                       /* +24 reserved */

_start:
    b       __robos_start           /* crt0.c */
