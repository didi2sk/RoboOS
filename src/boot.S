/*
 * RoboOS - boot code (AArch64)
 *
 * The GPU firmware loads kernel8.img at 0x80000 and starts core 0 here; cores
 * 1-3 wait in the firmware's spin table until smp.c writes secondary_entry to
 * it (they come here only if started without it: then they are parked).
 * We drop from EL3/EL2 to EL1, set up the stack and exception vectors, clear
 * BSS and jump to C.
 */

.section ".text.boot"
.global _start

_start:
    mrs     x1, mpidr_el1
    and     x1, x1, #3
    cbz     x1, primary
park:                               // cores 1-3 sleep forever
    wfe
    b       park

primary:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    and     x0, x0, #3
    cmp     x0, #3
    b.ne    from_el2
    // ---- EL3 -> EL2 ----
    mov     x2, #0x5b1              // NS, SMD, HCE, RW(AArch64)
    msr     scr_el3, x2
    mov     x2, #0x3c9              // EL2h, interrupts masked
    msr     spsr_el3, x2
    adr     x2, from_el2
    msr     elr_el3, x2
    eret

from_el2:
    ldr     x1, =_start             // stack grows down from 0x80000
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    and     x0, x0, #3
    cmp     x0, #2
    b.ne    in_el1
    // ---- EL2 -> EL1 ----
    msr     sp_el1, x1
    mrs     x0, cnthctl_el2         // allow EL1 access to timers
    orr     x0, x0, #3
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mov     x0, #(1 << 31)          // EL1 runs in AArch64
    msr     hcr_el2, x0
    mov     x0, #0x0800
    movk    x0, #0x30d0, lsl #16    // SCTLR_EL1: RES1 bits, MMU off
    msr     sctlr_el1, x0
    mov     x2, #0x3c5              // EL1h, interrupts masked
    msr     spsr_el2, x2
    adr     x2, in_el1
    msr     elr_el2, x2
    eret

in_el1:
    ldr     x1, =_start
    mov     sp, x1
    mov     x0, #(3 << 20)          // enable FP/SIMD at EL1
    msr     cpacr_el1, x0
    ldr     x0, =vectors
    msr     vbar_el1, x0
    isb

    ldr     x1, =__bss_start        // clear BSS
    ldr     x2, =__bss_size
1:  cbz     x2, 2f
    str     xzr, [x1], #8
    sub     x2, x2, #1
    b       1b
2:  bl      kernel_main
    b       park

/* ---------------- cores 1-3 (woken by smp.c) ---------------- */
.global secondary_entry
secondary_entry:
    mrs     x19, mpidr_el1
    and     x19, x19, #3                // core number
    ldr     x20, =core_stacks           // stack top = core_stacks + core * 64 KB
    lsl     x1, x19, #16
    add     x20, x20, x1
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    and     x0, x0, #3
    cmp     x0, #3
    b.ne    2f
    mov     x2, #0x5b1                  // EL3 -> EL2 (as for core 0)
    msr     scr_el3, x2
    mov     x2, #0x3c9
    msr     spsr_el3, x2
    adr     x2, 2f
    msr     elr_el3, x2
    eret
2:  mrs     x0, CurrentEL
    lsr     x0, x0, #2
    and     x0, x0, #3
    cmp     x0, #2
    b.ne    3f
    msr     sp_el1, x20                 // EL2 -> EL1
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0
    mov     x0, #0x0800
    movk    x0, #0x30d0, lsl #16
    msr     sctlr_el1, x0
    mov     x2, #0x3c5
    msr     spsr_el2, x2
    adr     x2, 3f
    msr     elr_el2, x2
    eret
3:  mov     sp, x20
    mov     x0, #(3 << 20)              // FP/SIMD
    msr     cpacr_el1, x0
    ldr     x0, =vectors
    msr     vbar_el1, x0
    isb
    mov     x0, x19
    bl      secondary_main              // smp.c, does not return
4:  wfe
    b       4b

/* ---------------- exception vector table ---------------- */
.macro VEC id
    .balign 0x80
    mov     x0, #\id
    b       exc_common
.endm

.balign 0x800
vectors:
    VEC 0
    VEC 1
    VEC 2
    VEC 3
    VEC 4
    VEC 5
    VEC 6
    VEC 7
    VEC 8
    VEC 9
    VEC 10
    VEC 11
    VEC 12
    VEC 13
    VEC 14
    VEC 15

exc_common:
    ldr     x1, =exc_stack_top      // separate stack per core: keeps the interrupted
    mrs     x2, mpidr_el1           // frames intact (a crashed program can return)
    and     x2, x2, #3
    lsl     x2, x2, #14             // 16 KB each
    sub     x1, x1, x2
    mov     sp, x1
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
    bl      exc_handler             // print fault info (returns only outside programs)
    ldr     x1, =_start             // fresh kernel stack
    mov     sp, x1
    ldr     x0, =shell_restart      // ... and return to the shell
    msr     elr_el1, x0
    mov     x0, #0x3c5
    msr     spsr_el1, x0
    eret

.section ".bss"
.balign 16
exc_stack:
    .space  16384 * 4               // one 16 KB stack per core, core 0 at the top
exc_stack_top:

.balign 16
core_stacks:
    .space  65536 * 3               // cores 1-3: core_stacks + n * 64 KB is the TOP for core n
.global core_stacks
