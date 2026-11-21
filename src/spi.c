/*
 * spi.c - SPI0 master, polled (0.72): GPIO8 CE0, GPIO9 MISO, GPIO10 MOSI, GPIO11 SCLK (all ALT0).
 * Used by the Waveshare 1.3" OLED HAT (SH1106 over 4-wire SPI). Mode 0, CE0 active low,
 * clock = core clock / divider (default 64 -> ~3.9 MHz at 250 MHz, the SH1106 takes up to 10 MHz).
 */
#include "kernel.h"

#define SPI0      (MMIO_BASE + 0x204000)
#define SPI_CS    (SPI0 + 0x00)
#define SPI_FIFO  (SPI0 + 0x04)
#define SPI_CLK   (SPI0 + 0x08)
#define CS_CLEAR  (3u << 4)
#define CS_TA     (1u << 7)
#define CS_DONE   (1u << 16)
#define CS_RXD    (1u << 17)
#define CS_TXD    (1u << 18)

static int ready;

void spi_init(u32 div)
{
    for (int p = 8; p <= 11; p++) gpio_func(p, 4);
    mmio_write(SPI_CLK, div ? div : 64);
    mmio_write(SPI_CS, CS_CLEAR);
    ready = 1;
}

/* send n bytes (what comes back is thrown away); 0 = ok, -2 = timeout */
int spi_write(const void *buf, int n)
{
    const u8 *b = buf;
    if (!ready) spi_init(0);
    mmio_write(SPI_CS, CS_CLEAR | CS_TA);
    u64 end = timer_us() + 20000 + (u64)n * 10;
    int i = 0;
    while (i < n) {
        u32 s = mmio_read(SPI_CS);
        if (s & CS_TXD) mmio_write(SPI_FIFO, b[i++]);
        while (mmio_read(SPI_CS) & CS_RXD) (void)mmio_read(SPI_FIFO);
        if (timer_us() > end) { mmio_write(SPI_CS, CS_CLEAR); return -2; }
    }
    while (!(mmio_read(SPI_CS) & CS_DONE)) {
        while (mmio_read(SPI_CS) & CS_RXD) (void)mmio_read(SPI_FIFO);
        if (timer_us() > end) { mmio_write(SPI_CS, CS_CLEAR); return -2; }
    }
    mmio_write(SPI_CS, CS_CLEAR);                     /* TA off = CE0 high */
    return 0;
}
