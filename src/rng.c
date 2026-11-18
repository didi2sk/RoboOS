/*
 * Hardware random number generator (BCM2835 RNG at 0x3F104000), for TLS keys.
 * Mixed with the free-running timer in case the RNG is missing.
 */
#include "kernel.h"

#define RNG_CTRL     (MMIO_BASE + 0x104000)
#define RNG_STATUS   (MMIO_BASE + 0x104004)
#define RNG_DATA     (MMIO_BASE + 0x104008)
#define RNG_INT_MASK (MMIO_BASE + 0x104010)

static int rng_on;

static void rng_init(void)
{
    if (rng_on) return;
    mmio_write(RNG_STATUS, 0x40000);                         /* discard the first numbers */
    mmio_write(RNG_INT_MASK, mmio_read(RNG_INT_MASK) | 1);   /* no interrupt */
    mmio_write(RNG_CTRL, mmio_read(RNG_CTRL) | 1);
    rng_on = 1;
}

/* n random bytes; 1 = from the hardware RNG, 0 = only the timer (RNG did not answer) */
int rng_bytes(void *out, int n)
{
    u8 *p = out;
    int hw = 1;
    rng_init();
    u64 mix = timer_us() * 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < n; i += 4) {
        u64 end = timer_us() + 20000;
        while (!(mmio_read(RNG_STATUS) >> 24) && timer_us() < end) ;
        u32 v;
        if (mmio_read(RNG_STATUS) >> 24) v = mmio_read(RNG_DATA);
        else { hw = 0; v = 0; }
        mix = (mix ^ v ^ timer_us()) * 0x9E3779B97F4A7C15ULL;
        for (int k = 0; k < 4 && i + k < n; k++) p[i + k] = (u8)((v ^ (u32)(mix >> 32)) >> (8 * k));
    }
    return hw;
}
