/*
 * Program loader: loads a flat .bin from the SD card to APP_BASE, gives it an
 * Arduino-style API table and runs it. Ctrl+C (checked whenever the program
 * calls delay()/getKey()/stopRequested()), exit() and crashes return to the shell.
 */
#include "kernel.h"
#include "sd.h"
#include "../sdk/robos.h"

#undef print
#undef printf
#undef delay
#undef millis
#undef micros
#undef exit

#define APP_MAX   0x01000000UL           /* 16 MB: APP_BASE .. 0x02000000 */

struct ctx { u64 regs[13]; };
int  ctx_save(struct ctx *c);
void ctx_restore(struct ctx *c, int v);

static struct ctx app_ctx;
static int app_running, app_reason, app_code;
#define R_RETURN 0
#define R_EXIT   1
#define R_BREAK  2
#define R_CRASH  3

/* keys typed while a program runs (except Ctrl+C) */
static char keyq[16];
static int kh, kt;

static void poll_input(void)
{
    int c = con_trygetc();
    if (c == 3 && app_running) {
        app_running = 0;
        app_reason = R_BREAK;
        ctx_restore(&app_ctx, 1);
    }
    if (c >= 0) {
        int n = (kh + 1) % 16;
        if (n != kt) { keyq[kh] = (char)c; kh = n; }
    }
}

/* ---------------- API implementation ---------------- */

static void a_print(const char *s) { con_puts(s); }
static void a_putc(char c)         { con_putc(c); }
static void a_clear(void)          { con_clear(); }
static void a_color(int c)         { if (c >= 0 && c <= 6) con_color(c); }

static int a_getkey(void)
{
    poll_input();
    if (kh == kt) return -1;
    char c = keyq[kt];
    kt = (kt + 1) % 16;
    return (u8)c;
}

static void a_pinmode(int pin, int mode)
{
    if (pin < 0 || pin > 53) return;
    gpio_func(pin, mode == 1 ? 1 : 0);
    gpio_pull(pin, mode == 2 ? 2 : mode == 3 ? 1 : 0);
}

static void a_write(int pin, int v) { if (pin >= 0 && pin <= 53) gpio_set(pin, v != 0); }
static int  a_read(int pin)         { return (pin >= 0 && pin <= 53) ? gpio_get(pin) : 0; }

static void a_delay(u32 ms)
{
    u64 end = timer_us() + (u64)ms * 1000;
    do poll_input(); while (timer_us() < end);
}

static void a_delay_us(u32 us) { delay_us(us); }
static u64  a_millis(void)     { return timer_us() / 1000; }
static u64  a_micros(void)     { return timer_us(); }
static int  a_stop(void)       { poll_input(); return 0; }

static void a_exit(int code)
{
    app_running = 0;
    app_reason = R_EXIT;
    app_code = code;
    ctx_restore(&app_ctx, 1);
}

/* ---- TCP/IP: waiting keeps the network running and honours Ctrl+C ---- */
static int  a_idle(void)                        { poll_input(); return 0; }
static int  a_tcp_connect(const char *h, int p) { return sock_tcp_connect(h, p, a_idle); }
static int  a_tcp_accept(int l)                 { poll_input(); return sock_tcp_accept(l); }
static int  a_udp_open(int lp, const char *h, int rp) { return sock_udp_open(lp, h, rp, a_idle); }
static int  a_swrite(int h, const void *d, int n) { poll_input(); return sock_write(h, d, n, a_idle); }
static int  a_sprint(int h, const char *s)      { return a_swrite(h, s, (int)strlen(s)); }
static int  a_sread(int h, void *b, int max)     { poll_input(); return sock_read(h, b, max); }
static int  a_avail(int h)                      { poll_input(); return sock_avail(h); }
static int  a_sendto(int h, const char *host, int p, const void *d, int n) { return sock_sendto(h, host, p, d, n, a_idle); }
static int  a_recvfrom(int h, void *b, int max, char *ip, int *p) { poll_input(); return sock_read_from(h, b, max, ip, p); }

static int a_temp(void)
{
    u32 v[2] = { 0, 0 };
    return mbox_prop(0x00030006, v, 2) ? (int)v[1] : -1;
}

static const struct robos_api api_table = {
    .version = API_VERSION,
    .size = sizeof(struct robos_api),
    .print = a_print,
    .printf = kprintf,
    .putc = a_putc,
    .getKey = a_getkey,
    .clear = a_clear,
    .color = a_color,
    .pinMode = a_pinmode,
    .digitalWrite = a_write,
    .digitalRead = a_read,
    .led = board_led,
    .delay = a_delay,
    .delayMicroseconds = a_delay_us,
    .millis = a_millis,
    .micros = a_micros,
    .stopRequested = a_stop,
    .exit = a_exit,
    .temperature = a_temp,
    .netSend = net_send,
    .netReceive = net_recv,
    .netName = net_name,
    .tcpConnect = a_tcp_connect,
    .tcpListen = sock_tcp_listen,
    .tcpAccept = a_tcp_accept,
    .udpOpen = a_udp_open,
    .sockWrite = a_swrite,
    .sockPrint = a_sprint,
    .sockRead = a_sread,
    .sockAvailable = a_avail,
    .udpSendTo = a_sendto,
    .udpReceiveFrom = a_recvfrom,
    .sockClose = sock_close,
    .sockError = sock_error,
    .ipAddress = ip_address,
};

/* called from the exception handler: a crash inside a program ends the program */
void app_crashed(void)
{
    if (!app_running) return;
    app_running = 0;
    app_reason = R_CRASH;
    ctx_restore(&app_ctx, 1);
}

/* ---------------- loading ---------------- */

static void sync_icache(u64 start, u64 len)
{
    for (u64 a = start & ~63UL; a < start + len; a += 64)
        __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\n ic iallu\n dsb ish\n isb" ::: "memory");
}

/* find the file: as given, in /RUN or /APPS, with or without .bin / .bas */
int app_resolve(const char *name, char *out, int max)
{
    static const char *fmt[9][2] = { { "", "" }, { "", ".bin" }, { "", ".bas" },
                                     { "/RUN/", "" }, { "/RUN/", ".bin" }, { "/RUN/", ".bas" },
                                     { "/APPS/", "" }, { "/APPS/", ".bin" }, { "/APPS/", ".bas" } };
    struct fat_file f;
    char abs[PATH_MAX_LEN];
    int plain = 1;                                       /* a bare name: also look in /RUN, /APPS */
    for (const char *s = name; *s; s++) if (*s == '/') plain = 0;
    if (path_abs(name, abs, sizeof(abs))) return -1;
    for (int i = 0; i < 3; i++) {                        /* the current directory first */
        int n = 0;
        for (const char *s = abs; *s && n < max - 1; s++) out[n++] = *s;
        for (const char *s = fmt[i][1]; *s && n < max - 1; s++) out[n++] = *s;
        out[n] = 0;
        if (!fat_open(out, &f) && !f.is_dir) return 0;
    }
    for (int i = 3; plain && i < 9; i++) {
        int n = 0;
        for (const char *s = fmt[i][0]; *s && n < max - 1; s++) out[n++] = *s;
        for (const char *s = name; *s && n < max - 1; s++) out[n++] = *s;
        for (const char *s = fmt[i][1]; *s && n < max - 1; s++) out[n++] = *s;
        out[n] = 0;
        if (!fat_open(out, &f) && !f.is_dir) return 0;
    }
    return -1;
}

int app_run(const char *path)
{
    struct fat_file f;
    u8 *base = (u8 *)APP_BASE;

    if (!fat_mounted()) { kprintf("no SD card / filesystem\n"); return -1; }
    if (fat_open(path, &f) || f.is_dir) { kprintf("not found: %s\n", path); return -1; }
    if (f.size < 32 || f.size > APP_MAX) { kprintf("%s: bad size %u\n", path, f.size); return -1; }
    if (fat_read(&f, base, f.size) != (int)f.size) { kprintf("%s: read error\n", path); return -1; }

    if (memcmp_n(base + 8, APP_MAGIC, 8)) {
        kprintf("%s: not a RoboOS program (missing %s header)\n", path, APP_MAGIC);
        return -1;
    }
    u64 end = *(u64 *)(base + 16);
    if (end < APP_BASE + f.size || end > APP_BASE + APP_MAX) {
        kprintf("%s: bad header\n", path);
        return -1;
    }
    memset(base + f.size, 0, end - (APP_BASE + f.size));    /* BSS */
    sync_icache(APP_BASE, end - APP_BASE);

    kh = kt = 0;
    app_reason = R_RETURN;
    app_code = 0;
    if (ctx_save(&app_ctx) == 0) {
        app_running = 1;
        int (*entry)(const struct robos_api *) = (void *)base;
        app_code = entry(&api_table);
        app_running = 0;
    }
    sock_close_all();                    /* whatever the program left open (also after Ctrl+C) */

    con_color(COL_GRAY);
    switch (app_reason) {
    case R_BREAK: kprintf("\n[%s stopped by Ctrl+C]\n", path); break;
    case R_CRASH: kprintf("[%s crashed]\n", path); break;
    default:      if (app_code) kprintf("[%s exited with code %d]\n", path, app_code); break;
    }
    con_color(COL_DEFAULT);
    return app_code;
}

/* ---------------- autostart from /RUN ---------------- */

#define MAX_AUTO 16
static char auto_names[MAX_AUTO][FAT_MAX_NAME];
static int  auto_n;

static int ends_with(const char *s, const char *ext)
{
    size_t n = strlen(s);
    if (n < 4) return 0;
    const char *e = s + n - 4;
    for (int i = 0; i < 4; i++) if ((e[i] | 32) != ext[i]) return 0;
    return 1;
}
static int ends_bin(const char *s) { return ends_with(s, ".bin"); }
static int ends_bas(const char *s) { return ends_with(s, ".bas"); }

/* run a program file: .bas in BASIC, anything else as a binary */
int app_start(const char *path)
{
    if (ends_bas(path)) return basic_run_file(path);
    return app_run(path);
}

static int collect(const struct fat_dirent *e, void *ctx)
{
    (void)ctx;
    if (!e->is_dir && (ends_bin(e->name) || ends_bas(e->name)) && auto_n < MAX_AUTO)
        memcpy(auto_names[auto_n++], e->name, FAT_MAX_NAME);
    return 0;
}

void app_autostart(void)
{
    auto_n = 0;
    if (!fat_mounted() || fat_list("/RUN", collect, 0)) return;
    if (!auto_n) return;

    for (int i = 1; i < auto_n; i++)                         /* sort by name */
        for (int j = i; j > 0 && strcmp(auto_names[j - 1], auto_names[j]) > 0; j--) {
            char t[FAT_MAX_NAME];
            memcpy(t, auto_names[j], FAT_MAX_NAME);
            memcpy(auto_names[j], auto_names[j - 1], FAT_MAX_NAME);
            memcpy(auto_names[j - 1], t, FAT_MAX_NAME);
        }

    con_color(COL_YELLOW);
    kprintf("  Autostart /RUN:");
    for (int i = 0; i < auto_n; i++) kprintf(" %s", auto_names[i]);
    kprintf("  (any key within 2 s = skip)\n");
    con_color(COL_DEFAULT);
    u64 end = timer_us() + 2000000;
    while (timer_us() < end)
        if (con_trygetc() >= 0) { kprintf("  autostart skipped\n\n"); return; }

    for (int i = 0; i < auto_n; i++) {
        char path[FAT_MAX_NAME + 8];
        int n = 0;
        for (const char *s = "/RUN/"; *s; s++) path[n++] = *s;
        for (const char *s = auto_names[i]; *s && n < (int)sizeof(path) - 1; s++) path[n++] = *s;
        path[n] = 0;
        con_color(COL_GRAY); kprintf("  > run %s\n", path); con_color(COL_DEFAULT);
        app_start(path);
    }
    con_putc('\n');
}
