/*
 * A simple memory allocator for big temporary work (decoding images):
 * a bump allocator over a fixed RAM area. Every block remembers its size
 * (so realloc can copy); free() of the last block gives the memory back,
 * heap_reset() frees everything. Nothing in the kernel keeps heap memory
 * between commands.
 */
#include "kernel.h"

#define HEAP_START 0x04000000UL          /* after programs (16-32 MB) and the editor */
#define HEAP_END   0x10000000UL          /* 192 MB; fits the 512 MB Zero 2 W too */

static u64 top = HEAP_START;

void heap_reset(void) { top = HEAP_START; }
u64  heap_free_bytes(void) { return HEAP_END - top; }

void *heap_alloc(size_t n)
{
    u64 p = (top + 16 + 15) & ~15UL;     /* 16-byte header with the size */
    if (p + n > HEAP_END) return 0;
    *(u64 *)(p - 16) = n;
    top = p + n;
    return (void *)p;
}

void heap_free(void *p)
{
    if (p && (u64)p + *(u64 *)((u64)p - 16) == top) top = (u64)p - 16;   /* the last block */
}

void *heap_realloc(void *p, size_t n)
{
    if (!p) return heap_alloc(n);
    u64 old = *(u64 *)((u64)p - 16);
    if ((u64)p + old == top) {                                           /* the last block grows */
        if ((u64)p + n > HEAP_END) return 0;
        *(u64 *)((u64)p - 16) = n;
        top = (u64)p + n;
        return p;
    }
    void *q = heap_alloc(n);
    if (q) memcpy(q, p, old < n ? old : n);
    return q;
}
