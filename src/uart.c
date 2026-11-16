/* PL011 UART0 on GPIO14 (TX) / GPIO15 (RX), 115200 8N1 */
#include "kernel.h"

#define UART0_DR    (MMIO_BASE + 0x201000)
#define UART0_FR    (MMIO_BASE + 0x201018)
#define UART0_IBRD  (MMIO_BASE + 0x201024)
#define UART0_FBRD  (MMIO_BASE + 0x201028)
#define UART0_LCRH  (MMIO_BASE + 0x20102C)
#define UART0_CR    (MMIO_BASE + 0x201030)
#define UART0_IMSC  (MMIO_BASE + 0x201038)
#define UART0_ICR   (MMIO_BASE + 0x201044)

void uart_init(void)
{
    mmio_write(UART0_CR, 0);                 /* disable UART */

    /* set UART clock to 4 MHz via firmware (default 48 MHz) */
    u32 clk[3] = { 2 /* UART */, 4000000, 0 };
    mbox_prop(0x00038002, clk, 3);

    /* GPIO32/33 (Bluetooth) -> input, GPIO14/15 -> ALT0 (PL011) */
    gpio_func(32, 0);
    gpio_func(33, 0);
    gpio_func(14, 4);
    gpio_func(15, 4);
    mmio_write(GPPUD, 0);                    /* no pull-up/down */
    delay_cycles(150);
    mmio_write(GPPUDCLK0, (1 << 14) | (1 << 15));
    delay_cycles(150);
    mmio_write(GPPUDCLK0, 0);

    mmio_write(UART0_ICR, 0x7FF);            /* clear interrupts */
    mmio_write(UART0_IBRD, 2);               /* 4e6 / (16*115200) = 2.17 */
    mmio_write(UART0_FBRD, 0xB);
    mmio_write(UART0_LCRH, 0x7 << 4);        /* 8 bit, FIFO */
    mmio_write(UART0_IMSC, 0);
    mmio_write(UART0_CR, 0x301);             /* UART, TX, RX enabled */
}

void uart_putc(char c)
{
    while (mmio_read(UART0_FR) & 0x20) ;     /* TX FIFO full */
    mmio_write(UART0_DR, (u8)c);
}

int uart_has_char(void)
{
    return !(mmio_read(UART0_FR) & 0x10);
}

char uart_getc(void)
{
    while (mmio_read(UART0_FR) & 0x10) ;     /* RX FIFO empty */
    return (char)(mmio_read(UART0_DR) & 0xFF);
}
