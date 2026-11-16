/* Mailbox - talking to the GPU firmware (VideoCore) */
#include "kernel.h"

#define MBOX_BASE   (MMIO_BASE + 0xB880)
#define MBOX_READ   (MBOX_BASE + 0x00)
#define MBOX_STATUS (MBOX_BASE + 0x18)
#define MBOX_WRITE  (MBOX_BASE + 0x20)
#define MBOX_FULL   0x80000000
#define MBOX_EMPTY  0x40000000
#define MBOX_OK     0x80000000

volatile u32 __attribute__((aligned(64))) mbox[64];

int mbox_call(u8 ch)
{
    u32 r = (u32)((uintptr_t)mbox & ~0xFUL) | (ch & 0xF);

    dcache_clean_inval((void *)mbox, sizeof(mbox));   /* GPU reads RAM directly */
    while (mmio_read(MBOX_STATUS) & MBOX_FULL) ;
    mmio_write(MBOX_WRITE, r);
    for (;;) {
        while (mmio_read(MBOX_STATUS) & MBOX_EMPTY) ;
        if (mmio_read(MBOX_READ) == r) {
            dcache_clean_inval((void *)mbox, sizeof(mbox));
            return mbox[1] == MBOX_OK;
        }
    }
}

int mbox_prop(u32 tag, u32 *vals, int n)
{
    int i;
    mbox[0] = (6 + n) * 4;
    mbox[1] = 0;
    mbox[2] = tag;
    mbox[3] = n * 4;
    mbox[4] = 0;
    for (i = 0; i < n; i++) mbox[5 + i] = vals[i];
    mbox[5 + n] = 0;
    if (!mbox_call(MBOX_CH_PROP)) return 0;
    if (!(mbox[4] & 0x80000000)) return 0;   /* tag not processed */
    for (i = 0; i < n; i++) vals[i] = mbox[5 + i];
    return 1;
}
