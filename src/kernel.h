#ifndef KERNEL_H
#define KERNEL_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long      u64;
typedef unsigned long      size_t;
typedef unsigned long      uintptr_t;

#define MMIO_BASE   0x3F000000UL

static inline void mmio_write(u64 reg, u32 val) { *(volatile u32 *)reg = val; }
static inline u32  mmio_read(u64 reg)           { return *(volatile u32 *)reg; }

/* GPIO */
#define GPFSEL0     (MMIO_BASE + 0x200000)
#define GPSET0      (MMIO_BASE + 0x20001C)
#define GPCLR0      (MMIO_BASE + 0x200028)
#define GPLEV0      (MMIO_BASE + 0x200034)
#define GPPUD       (MMIO_BASE + 0x200094)
#define GPPUDCLK0   (MMIO_BASE + 0x200098)

/* lib.c */
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
size_t strlen(const char *s);
int    parse_num(const char *s, u64 *out);   /* decimal or 0x hex */
int    memcmp_n(const void *a, const void *b, size_t n);

/* timer.c */
u64  timer_us(void);
void delay_us(u64 us);
void delay_cycles(u32 n);

/* uart.c */
void uart_init(void);
void uart_putc(char c);
char uart_getc(void);
int  uart_has_char(void);

/* mbox.c */
#define MBOX_CH_PROP 8
extern volatile u32 mbox[64];
int mbox_call(u8 ch);
int mbox_prop(u32 tag, u32 *vals, int n);   /* one tag, n values in/out */

/* mmu.c */
void mmu_init(u64 ram_end);
int  mmu_enabled(void);
void dcache_clean_inval(void *addr, size_t len);

/* fb.c */
int  fb_init(u32 w, u32 h);
int  fb_ready(void);
u32  fb_width(void);
u32  fb_height(void);
void fb_putc(char c);
void fb_clear(void);
void fb_setcolor(u32 fg);

/* console.c */
#define COL_DEFAULT  0
#define COL_GREEN    1
#define COL_CYAN     2
#define COL_YELLOW   3
#define COL_RED      4
#define COL_GRAY     5
#define COL_WHITE    6
void con_putc(char c);
void con_puts(const char *s);
void con_color(int c);
void con_clear(void);
int  con_getc(void);        /* blocking: UART or USB keyboard */
int  con_trygetc(void);     /* -1 if nothing */
void kprintf(const char *fmt, ...);

/* gpio */
void gpio_func(int pin, int fn);     /* 0=in 1=out 4=alt0 ... */
void gpio_set(int pin, int on);
int  gpio_get(int pin);
void gpio_pull(int pin, int mode);   /* 0 = off, 1 = down, 2 = up */
void board_led(int on);              /* green ACT LED */

/* board.c */
void board_init(void);
u32  board_rev(void);
int  board_type(void);
const char *board_name(void);
const char *board_host(void);
int  board_usb_direct(void);

/* app.c - programs from the SD card */
int  app_run(const char *path);
int  app_start(const char *path);     /* .bin or .bas */
int  app_resolve(const char *name, char *out, int max);
void app_autostart(void);
void app_crashed(void);

/* basic.c */
void basic_main(const char *file);
int  basic_run_file(const char *path);

/* shell.c */
void shell_run(void);
void shell_restart(void);

#endif
