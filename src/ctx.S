/*
 * Minimal setjmp/longjmp used to abort a running program (Ctrl+C, exit()).
 *   int  ctx_save(struct ctx *c);          returns 0, or the value given to ctx_restore
 *   void ctx_restore(struct ctx *c, int v)
 * Saves x19-x30 and sp (13 * 8 bytes).
 */
.global ctx_save
.global ctx_restore

ctx_save:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x2, sp
    str     x2, [x0, #96]
    mov     x0, #0
    ret

ctx_restore:
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x2, [x0, #96]
    mov     sp, x2
    mov     x0, x1
    cbnz    x0, 1f
    mov     x0, #1
1:  ret
