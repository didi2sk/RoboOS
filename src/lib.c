#include "kernel.h"

void *memset(void *d, int c, size_t n)
{
    u8 *p = d;
    while (n--) *p++ = (u8)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    u8 *a = d; const u8 *b = s;
    while (n--) *a++ = *b++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    u8 *a = d; const u8 *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (u8)*a - (u8)*b : 0;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

int abs(int v) { return v < 0 ? -v : v; }

int atoi(const char *s)
{
    int v = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

int memcmp_n(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int parse_num(const char *s, u64 *out)
{
    u64 v = 0;
    int base = 10, any = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    for (; *s; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return 0;
        v = v * base + d;
        any = 1;
    }
    *out = v;
    return any;
}

/* ---------------- timer (BCM2837 system timer, 1 MHz) ---------------- */
#define SYSTMR_LO (MMIO_BASE + 0x3004)
#define SYSTMR_HI (MMIO_BASE + 0x3008)

u64 timer_us(void)
{
    u32 hi = mmio_read(SYSTMR_HI);
    u32 lo = mmio_read(SYSTMR_LO);
    if (hi != mmio_read(SYSTMR_HI)) {      /* wrapped between reads */
        hi = mmio_read(SYSTMR_HI);
        lo = mmio_read(SYSTMR_LO);
    }
    return ((u64)hi << 32) | lo;
}

void delay_us(u64 us)
{
    u64 end = timer_us() + us;
    while (timer_us() < end) ;
}

void delay_cycles(u32 n)
{
    while (n--) __asm__ volatile("nop");
}

/* ---------------- GPIO ---------------- */
void gpio_func(int pin, int fn)
{
    u64 reg = GPFSEL0 + (pin / 10) * 4;
    int sh = (pin % 10) * 3;
    u32 v = mmio_read(reg);
    v &= ~(7u << sh);
    v |= (u32)(fn & 7) << sh;
    mmio_write(reg, v);
}

int gpio_mode(int pin)
{
    return (int)((mmio_read(GPFSEL0 + (pin / 10) * 4) >> ((pin % 10) * 3)) & 7);
}

void gpio_set(int pin, int on)
{
    mmio_write((on ? GPSET0 : GPCLR0) + (pin / 32) * 4, 1u << (pin % 32));
}

int gpio_get(int pin)
{
    return (mmio_read(GPLEV0 + (pin / 32) * 4) >> (pin % 32)) & 1;
}

void gpio_pull(int pin, int mode)
{
    u64 clk = GPPUDCLK0 + (pin / 32) * 4;
    mmio_write(GPPUD, mode & 3);
    delay_cycles(150);
    mmio_write(clk, 1u << (pin % 32));
    delay_cycles(150);
    mmio_write(GPPUD, 0);
    mmio_write(clk, 0);
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return c ? 0 : (char *)s;
}

char *strstr(const char *h, const char *n)
{
    size_t l = strlen(n);
    for (; *h; h++) if (!strncmp(h, n, l)) return (char *)h;
    return l ? 0 : (char *)h;
}

#define PM_RSTC     (MMIO_BASE + 0x10001C)
#define PM_WDOG     (MMIO_BASE + 0x100024)
#define PM_PASSWORD 0x5A000000

void sys_reboot(void)
{
    mmio_write(PM_WDOG, PM_PASSWORD | 10);
    mmio_write(PM_RSTC, PM_PASSWORD | (mmio_read(PM_RSTC) & ~0x30u) | 0x20);
    for (;;) __asm__ volatile("wfe");
}
