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
char  *strchr(const char *s, int c);
char  *strstr(const char *h, const char *n);
void   sys_reboot(void);                 /* watchdog reset, does not return */

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

/* smp.c - core 1 = USB + network; locks */
int  smp_core(void);
int  smp_start_io(void);
int  io_core_active(void);
void io_poll_all(void);
void io_lock(void);
void io_unlock(void);
int  io_release(void);                  /* drop the io lock completely (to wait); returns depth */
void io_reacquire(int depth);
int  io_held(void);
void con_lock(void);
void con_unlock(void);
void mbox_lock(void);
void mbox_unlock(void);
void smp_crashed(int core);
u64  smp_io_loops(void);
int  smp_start_workers(void);           /* cores 2 + 3; returns how many run */
int  smp_run(int core, void (*fn)(void *), void *arg);   /* -1 = not there / busy */
int  smp_wait(int core, u32 ms);        /* -1 = gave up */
int  smp_workers(void);
int  smp_worker_ok(int core);
void kbd_poll(void);
void usb_hotplug(void);

/* mmu.c */
void mmu_init(u64 ram_end);
void mmu_enable(void);
int  mmu_enabled(void);
void dcache_clean_inval(void *addr, size_t len);

/* fb.c */
int  fb_init(u32 w, u32 h);              /* 0, 0 = the monitor's own resolution */
void fb_native(u32 *w, u32 *h);
int  fb_pages(void);                    /* 2 = a hidden page for tear-free video */
u32  fb_pitch(void);
u32 *fb_page_row(int page, int y);
void fb_show_page(int page);
int  screen_set(u32 w, u32 h);          /* console.c: change the resolution now */
void screen_choice(u32 *w, u32 *h);     /* main.c: /SCREEN.TXT (0 x 0 = auto) */
int  fb_ready(void);
u32  fb_width(void);
u32  fb_height(void);
void fb_putc(char c);
void fb_clear(void);
void fb_setcolor(u32 fg);
void fb_goto(int x, int y);
void fb_hold(int on);                    /* 1 = a picture owns the screen */
void fb_redraw(void);
u32 *fb_row(int y);                      /* pixels 0x00RRGGBB */
void fb_text_px(int x, int y, const char *s, u32 fg, u32 bg);
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
int  gpio_mode(int pin);                 /* function bits: 0 in, 1 out, 4 alt0 ... */
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
void ip_prepare(void);                   /* read /IP.TXT on core 0 before core 1 starts */
void ipstack_input(const u8 *frame, int len);  /* a frame that is not a RoboOS message */
int  ip_started(void);
void ip_cmd(int argc, char **argv);
void ping_cmd(int argc, char **argv);
void http_cmd(int argc, char **argv);
const char *ip_address_str(void);

/* webapi.c - JSON API behind 'http on' (Bearer token) */
void api_set_token(const char *t);
const char *api_token(void);
void api_poll(void);
int  api_handle(const char *method, const char *path, const char *auth, const char *body, int blen,
                const char *from, char *out, int max);
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

/* sound.c - tones on the 3.5 mm jack (PWM) or GPIO18/19 */
void sound_tone(int freq, int ms);       /* start; ms > 0 ends by itself, 0 = until sound_stop */
void sound_stop(void);
void sound_poll(void);                   /* from the console idle loop */
int  sound_playing(void);
int  sound_beep(int freq, int ms, int (*idle)(void));        /* waits; 1 = interrupted */
int  sound_play(const char *music, int (*idle)(void), int *err);   /* 0 ok, 1 stopped, -1 error */
void sound_volume(int v);                /* 0..100 */
int  sound_get_volume(void);
void sound_output(int gpio);             /* 0 = jack, 1 = GPIO18/19 */
const char *sound_pins(void);

/* i2s.c - I2S DAC module on GPIO18/19/21 */
int  i2s_start(void);
void i2s_stop(void);
int  i2s_running(void);
void i2s_tone(int freq, int ms);         /* 0 = silence */
void i2s_poll(void);
void i2s_volume(int v);
int  i2s_wav(const char *path, int (*idle)(void));   /* 0 ok, 1 stopped, < 0 error */
int  pwm_wav(const char *path, int (*idle)(void));   /* WAV on the PWM (pins set by sound.c) */
const char *i2s_wav_error(int e);
void i2s_status(void);
int  audio_open(int hz, int on_i2s);     /* a sound stream (video); 0 = ok */
void audio_write(const short *lr, int frames);   /* stereo 16-bit; waits while full */
int  audio_queued_ms(void);
void audio_close(int now);               /* now = drop what is queued */
int  sound_stream_open(int hz);          /* sound.c: on the current output */
u32  i2s_segments(void);                 /* audio DMA segments played so far */
int  sound_wav(const char *path, int (*idle)(void)); /* sound.c: I2S DAC or PWM (jack / GPIO) */
int  sound_output_mode(void);            /* 0 jack, 1 gpio, 2 i2s */

/* heap.c - memory for big temporary work */
void *heap_alloc(size_t n);
void *heap_realloc(void *p, size_t n);
void  heap_free(void *p);
void  heap_reset(void);
u64   heap_free_bytes(void);

/* image.c - pictures (JPEG, PNG, BMP, GIF) */
int  img_draw(const char *path, int x, int y, int w, int h, int center);   /* 0 ok, < 0 error */
const char *img_error(void);
void img_splash_start(void);             /* /SPLASH.PNG|JPG|BMP at boot */
void img_splash_end(void);

/* video.c - MPEG-1 video + MP2 sound (.mpg) */
int  video_play(const char *path, int info);   /* 0 end, 1 stopped by a key, < 0 error */
const char *video_error(void);
void video_cmd(int argc, char **argv);
void view_cmd(int argc, char **argv);

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
