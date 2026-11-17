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
void fb_goto(int x, int y);
void fb_clreol(void);
int  fb_cols(void);
int  fb_rows(void);

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
/* special keys returned by con_getkey() next to plain characters */
enum { K_UP = 256, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL, K_PGUP, K_PGDN };
int  con_getkey(void);      /* blocking; decodes escape sequences */
void con_goto(int x, int y);            /* cursor to column x, row y (0-based) */
void con_clreol(void);                  /* clear from the cursor to the end of the line */
void con_size(int *cols, int *rows);    /* HDMI text size, 80x24 on UART only */

/* lineedit.c - line editor with cursor keys and history */
#define LE_MAX  160
#define LE_HIST 10
struct le_hist { char line[LE_HIST][LE_MAX]; int n; };
/* returns the length, -1 on Ctrl+C; h, complete and prompt (for Ctrl+L) may be 0 */
int  line_edit(char *line, int max, struct le_hist *h,
               int (*complete)(char *line, int len), void (*prompt)(void));

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

/* net.c - RoboOS messages over raw Ethernet frames */
#define NET_NAME_MAX 16
#define NET_TEXT_MAX 200
void net_poll(void);                    /* called from the console idle loop */
void net_notify(int on);                /* print incoming messages right away (shell prompt) */
const char *net_name(void);
int  net_set_name(const char *name);    /* 0 = saved to /NAME.TXT, 1 = set only, -1 = bad name */
int  net_send(const char *to, const char *text);   /* 0 delivered, 1 sent to all, <0 error */
const char *net_error(int e);
int  net_recv(char *from, char *text, int max);    /* text length, -1 = no message */
int  net_avail(void);
void net_who(void);                     /* ask who is there and print the answers */
void net_loop(int on);                  /* loopback: sent frames come back (self-test) */
int  net_looping(void);

/* ip.c - TCP/IP (lwIP) */
void ip_poll(int notify);
void ipstack_input(const u8 *frame, int len);  /* a frame that is not a RoboOS message */
int  ip_started(void);
void ip_cmd(int argc, char **argv);
void ping_cmd(int argc, char **argv);
void http_cmd(int argc, char **argv);
const char *os_version(void);
/* sockets for programs: handles 1..8, errors < 0 (sock_error); idle() polls the network */
int  sock_tcp_connect(const char *host, int port, int (*idle)(void));
int  sock_tcp_listen(int port);
int  sock_tcp_accept(int listener);                 /* new handle, 0 = nobody yet */
int  sock_udp_open(int lport, const char *host, int rport, int (*idle)(void));
int  sock_write(int h, const void *d, int len, int (*idle)(void));
int  sock_sendto(int h, const char *host, int port, const void *d, int len, int (*idle)(void));
int  sock_avail(int h);                             /* bytes waiting, -1 = closed and empty */
int  sock_read(int h, void *buf, int max);          /* TCP: 0 nothing, -1 closed; UDP: -1 none */
int  sock_read_from(int h, void *buf, int max, char *ip, int *port);
int  sock_getc(int h);                              /* byte, -1 nothing yet, -2 closed */
void sock_close(int h);
void sock_close_all(void);
const char *sock_error(int e);
const char *ip_address(void);

/* edit.c */
void editor_main(const char *path);

/* path.c - current directory, relative paths */
#define PATH_MAX_LEN 128
const char *cwd_get(void);
int  cwd_set(const char *path);                 /* 0 ok, -1 not found, -2 not a dir, -3 too long */
int  path_abs(const char *in, char *out, int max);   /* relative / . / .. -> absolute; 0 = ok */

/* basic.c */
void basic_main(const char *file);
int  basic_run_file(const char *path);

/* shell.c */
void shell_run(void);
void shell_restart(void);

#endif
