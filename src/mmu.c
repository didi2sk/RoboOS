/*
 * MMU - identity mapping (virtual = physical), 2 MB blocks.
 *   0 .. ram_end            normal memory, cached (fast)
 *   ram_end .. 0x3F000000   normal non-cacheable (GPU memory, framebuffer)
 *   __dma_start (2 MB)      non-cacheable - USB DMA buffers
 *   0x3F000000 .. 4 GB      peripherals (device)
 * Without the MMU all memory is "device" = slow, and unaligned access faults.
 */
#include "kernel.h"

extern char __pt_start[], __dma_start[], __dma_end[];

#define PT_BLOCK    0x1UL
#define PT_TABLE    0x3UL
#define PT_AF       (1UL << 10)
#define PT_ISH      (3UL << 8)
#define PT_XN       (3UL << 53)
#define ATTR(i)     ((u64)(i) << 2)

#define MAIR_NORMAL  0   /* 0xFF write-back */
#define MAIR_DEVICE  1   /* 0x04 nGnRE      */
#define MAIR_NC      2   /* 0x44 non-cache  */

static int mmu_on;

void mmu_init(u64 ram_end)
{
    u64 *l1 = (u64 *)__pt_start;
    u64 *l2 = (u64 *)(__pt_start + 4096);
    u64 a;
    int i;

    memset(l1, 0, 4096);
    for (i = 0; i < 512; i++) {
        a = (u64)i << 21;
        if (a >= 0x3F000000UL)
            l2[i] = a | PT_BLOCK | PT_AF | ATTR(MAIR_DEVICE) | PT_XN;
        else if (a >= ram_end || (a >= (u64)__dma_start && a < (u64)__dma_end))
            l2[i] = a | PT_BLOCK | PT_AF | PT_ISH | ATTR(MAIR_NC) | PT_XN;
        else
            l2[i] = a | PT_BLOCK | PT_AF | PT_ISH | ATTR(MAIR_NORMAL);
    }
    l1[0] = (u64)l2 | PT_TABLE;
    for (i = 1; i < 4; i++)               /* 1-4 GB: ARM local peripherals */
        l1[i] = ((u64)i << 30) | PT_BLOCK | PT_AF | ATTR(MAIR_DEVICE) | PT_XN;

    u64 mair = (0xFFUL << 0) | (0x04UL << 8) | (0x44UL << 16);
    u64 tcr  = 32               /* T0SZ: 4 GB address space */
             | (1UL << 8)       /* IRGN0 WB */
             | (1UL << 10)      /* ORGN0 WB */
             | (3UL << 12)      /* SH0 inner shareable */
             | (0UL << 14)      /* TG0 4 KB */
             | (1UL << 23);     /* EPD1: TTBR1 unused */

    __asm__ volatile(
        "dsb sy\n"
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb ish\n"
        "isb\n"
        "mrs x0, sctlr_el1\n"
        "orr x0, x0, #(1 << 0)\n"     /* M: MMU */
        "orr x0, x0, #(1 << 2)\n"     /* C: data cache */
        "orr x0, x0, #(1 << 12)\n"    /* I: instruction cache */
        "bic x0, x0, #(1 << 1)\n"     /* A: allow unaligned access */
        "msr sctlr_el1, x0\n"
        "isb\n"
        :: "r"(mair), "r"(tcr), "r"((u64)l1) : "x0", "memory");
    mmu_on = 1;
}

int mmu_enabled(void) { return mmu_on; }

void dcache_clean_inval(void *addr, size_t len)
{
    u64 a = (u64)addr & ~63UL, end = (u64)addr + len;
    for (; a < end; a += 64)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
