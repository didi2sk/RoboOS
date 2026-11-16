/* Console - output to UART and HDMI at once, plus a small printf */
#include "kernel.h"
#include "usb.h"
#include <stdarg.h>

static const char *ansi[] = {
    "\033[0m", "\033[1;32m", "\033[1;36m", "\033[1;33m", "\033[1;31m", "\033[0;37m", "\033[1;37m"
};
static const u32 rgb[] = {
    0xC0C0C0, 0x55FF55, 0x55FFFF, 0xFFFF55, 0xFF5555, 0x808080, 0xFFFFFF
};

void con_putc(char c)
{
    if (c == '\n') uart_putc('\r');
    uart_putc(c);
    fb_putc(c);
}

void con_puts(const char *s)
{
    while (*s) con_putc(*s++);
}

void con_color(int c)
{
    const char *s = ansi[c];
    while (*s) uart_putc(*s++);
    fb_setcolor(rgb[c]);
}

void con_clear(void)
{
    const char *s = "\033[2J\033[H";
    while (*s) uart_putc(*s++);
    fb_clear();
}

static void put_num(u64 v, int base, int width, char pad, int upper, int neg)
{
    char buf[24];
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do { buf[n++] = dig[v % base]; v /= base; } while (v);
    if (neg) buf[n++] = '-';
    while (n < width) buf[n++] = pad;
    while (n) con_putc(buf[--n]);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { con_putc(*fmt); continue; }
        fmt++;
        char pad = ' ';
        int width = 0, lng = 0, left = 0;
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') { lng = 1; fmt++; }
        switch (*fmt) {
        case 'd': {
            long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            put_num(v < 0 ? -v : v, 10, width, pad, 0, v < 0);
            break;
        }
        case 'u': put_num(lng ? va_arg(ap, u64) : va_arg(ap, u32), 10, width, pad, 0, 0); break;
        case 'x': put_num(lng ? va_arg(ap, u64) : va_arg(ap, u32), 16, width, pad, 0, 0); break;
        case 'X': put_num(lng ? va_arg(ap, u64) : va_arg(ap, u32), 16, width, pad, 1, 0); break;
        case 'p': con_puts("0x"); put_num(va_arg(ap, u64), 16, 8, '0', 0, 0); break;
        case 'c': con_putc((char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            int len = (int)strlen(s);
            if (!left) while (len < width--) con_putc(' ');
            con_puts(s);
            if (left) while (len < width--) con_putc(' ');
            break;
        }
        case '%': con_putc('%'); break;
        default:  con_putc('%'); con_putc(*fmt); break;
        }
    }
    va_end(ap);
}

/* ---------------- input: UART + USB keyboard ---------------- */
int con_trygetc(void)
{
    if (uart_has_char()) return (u8)uart_getc();
    kbd_poll();
    usb_hotplug();
    return kbd_getc();
}

int con_getc(void)
{
    int c;
    while ((c = con_trygetc()) < 0) ;
    return c;
}
