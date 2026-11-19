/*
 * The console UART: PL011 (UART0) on GPIO14 (TX) / GPIO15 (RX), 115200 8N1.
 * When Bluetooth is switched on ('bt'), the PL011 goes to the BT chip (GPIO32/33)
 * and the console moves to the mini UART (UART1) on the same pins 14/15, at the
 * same speed - as Raspberry Pi OS does. 'bt off' moves it back.
 */
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
    mmio_write(GPPUD, 0);                    /* TX: no pull-up/down */
    delay_cycles(150);
    mmio_write(GPPUDCLK0, 1 << 14);
    delay_cycles(150);
    mmio_write(GPPUDCLK0, 0);
    /* RX: pull-up = the idle level of a serial line. Without it a pin with no
     * cable floats and noise turns into random characters (seen at boot) */
    mmio_write(GPPUD, 2);
    delay_cycles(150);
    mmio_write(GPPUDCLK0, 1 << 15);
    delay_cycles(150);
    mmio_write(GPPUD, 0);
    mmio_write(GPPUDCLK0, 0);

    mmio_write(UART0_ICR, 0x7FF);            /* clear interrupts */
    mmio_write(UART0_IBRD, 2);               /* 4e6 / (16*115200) = 2.17 */
    mmio_write(UART0_FBRD, 0xB);
    mmio_write(UART0_LCRH, 0x7 << 4);        /* 8 bit, FIFO */
    mmio_write(UART0_IMSC, 0);
    mmio_write(UART0_CR, 0x301);             /* UART, TX, RX enabled */
}

/* ---------------- mini UART (AUX) ---------------- */
#define AUX_ENABLES (MMIO_BASE + 0x215004)
#define MU_IO       (MMIO_BASE + 0x215040)
#define MU_IER      (MMIO_BASE + 0x215044)
#define MU_IIR      (MMIO_BASE + 0x215048)
#define MU_LCR      (MMIO_BASE + 0x21504C)
#define MU_MCR      (MMIO_BASE + 0x215050)
#define MU_LSR      (MMIO_BASE + 0x215054)
#define MU_CNTL     (MMIO_BASE + 0x215060)
#define MU_BAUD     (MMIO_BASE + 0x215068)

static volatile int on_mini;                /* 1 = the console is on the mini UART */

static void pull(int pin, int mode)
{
    mmio_write(GPPUD, (u32)mode);
    delay_cycles(150);
    mmio_write(GPPUDCLK0 + (pin >= 32 ? 4 : 0), 1u << (pin & 31));
    delay_cycles(150);
    mmio_write(GPPUD, 0);
    mmio_write(GPPUDCLK0 + (pin >= 32 ? 4 : 0), 0);
}

/* the console to the mini UART on GPIO14/15 (ALT5); the PL011 is then free */
void uart_console_mini(int on)
{
    if (on == on_mini) return;
    if (on) {
        while (mmio_read(UART0_FR) & 0x08) ;  /* PL011 still sending */
        u32 clk[2] = { 4, 0 };               /* core clock: 250 MHz with enable_uart=1 */
        u32 core = mbox_prop(0x00030002, clk, 2) && clk[1] ? clk[1] : 250000000;
        mmio_write(AUX_ENABLES, mmio_read(AUX_ENABLES) | 1);
        mmio_write(MU_CNTL, 0);
        mmio_write(MU_IER, 0);
        mmio_write(MU_LCR, 3);               /* 8 bit */
        mmio_write(MU_MCR, 0);
        mmio_write(MU_IIR, 0xC6);            /* clear both FIFOs */
        mmio_write(MU_BAUD, core / (8 * 115200) - 1);
        gpio_func(14, 2);                    /* ALT5 */
        gpio_func(15, 2);
        mmio_write(MU_CNTL, 3);              /* TX + RX on */
        on_mini = 1;
    } else {
        while (!(mmio_read(MU_LSR) & 0x40)) ;  /* mini UART idle */
        on_mini = 0;
        uart_init();                         /* PL011 back on 14/15, GPIO32/33 to inputs */
    }
}

/* ---------------- PL011 for the Bluetooth chip (GPIO30-33 ALT3) ---------------- */

void uart_bt_init(int flow)
{
    mmio_write(UART0_CR, 0);
    u32 clk[3] = { 2, 48000000, 0 };         /* 48 MHz UART clock: room for faster speeds later */
    mbox_prop(0x00038002, clk, 3);
    gpio_func(32, 7);                        /* ALT3: TXD0 */
    gpio_func(33, 7);                        /*       RXD0 */
    pull(32, 0);
    pull(33, 2);
    if (flow) {                              /* 3B+ / Zero 2 W: CTS0 = 30, RTS0 = 31 */
        gpio_func(30, 7); gpio_func(31, 7);
        pull(30, 2); pull(31, 0);
    }
    mmio_write(UART0_ICR, 0x7FF);
    mmio_write(UART0_IBRD, 26);              /* 48e6 / (16 * 115200) = 26.04 */
    mmio_write(UART0_FBRD, 3);
    mmio_write(UART0_LCRH, 0x7 << 4);        /* 8N1, FIFO */
    mmio_write(UART0_IMSC, 0);
    mmio_write(UART0_CR, 0x301 | (flow ? (3u << 14) : 0));   /* + CTSEN / RTSEN */
}

void uart_bt_putc(u8 c)
{
    while (mmio_read(UART0_FR) & 0x20) ;
    mmio_write(UART0_DR, c);
}

/* the next byte from the BT chip, -1 = none within us microseconds */
int uart_bt_getc(u32 us)
{
    u64 end = timer_us() + us;
    while (mmio_read(UART0_FR) & 0x10) if (timer_us() >= end) return -1;
    return (int)(mmio_read(UART0_DR) & 0xFF);
}

/* ---------------- the console ---------------- */

void uart_putc(char c)
{
    if (on_mini) {
        while (!(mmio_read(MU_LSR) & 0x20)) ;  /* TX FIFO full */
        mmio_write(MU_IO, (u8)c);
        return;
    }
    while (mmio_read(UART0_FR) & 0x20) ;     /* TX FIFO full */
    mmio_write(UART0_DR, (u8)c);
}

int uart_has_char(void)
{
    if (on_mini) return mmio_read(MU_LSR) & 1;
    return !(mmio_read(UART0_FR) & 0x10);
}

char uart_getc(void)
{
    if (on_mini) {
        while (!(mmio_read(MU_LSR) & 1)) ;
        return (char)(mmio_read(MU_IO) & 0xFF);
    }
    while (mmio_read(UART0_FR) & 0x10) ;     /* RX FIFO empty */
    u32 d = mmio_read(UART0_DR);
    if (d & 0xF00) return 0;                 /* framing / parity / break / overrun: noise, not a key */
    return (char)(d & 0xFF);
}
