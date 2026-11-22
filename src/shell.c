/* Shell - line editor (history, Tab completion) and commands */
#include "kernel.h"
#include "usb.h"
#include "sd.h"

#define LINE_MAX  256
#define ARGS_MAX  8

static struct le_hist hist;
static int hist_saved = -1;          /* hist.n written to /HISTORY.TXT, -1 = not loaded yet */

/* ---------------- helpers ---------------- */

static u32 clock_hz(u32 id, u32 tag)
{
    u32 v[2] = { id, 0 };
    return mbox_prop(tag, v, 2) ? v[1] : 0;
}

static int temp_mc(void)
{
    u32 v[2] = { 0, 0 };
    return mbox_prop(0x00030006, v, 2) ? (int)v[1] : -1;
}

static void need(const char *usage) { kprintf("usage: %s\n", usage); }

/* ---------------- commands ---------------- */

static void cmd_help(int, char **);

static void cmd_info(int argc, char **argv)
{
    (void)argc; (void)argv;
    u32 rev = board_rev();
    u32 fw[1] = { 0 }, ser[2] = { 0, 0 }, mac[2] = { 0, 0 };
    u32 arm[2] = { 0, 0 }, vc[2] = { 0, 0 };
    u64 el;
    mbox_prop(0x00000001, fw, 1);
    mbox_prop(0x00010004, ser, 2);
    mbox_prop(0x00010003, mac, 2);
    mbox_prop(0x00010005, arm, 2);
    mbox_prop(0x00010006, vc, 2);
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    u8 *m = (u8 *)mac;
    int t = temp_mc();

    con_color(COL_CYAN); con_puts("Hardware\n"); con_color(COL_DEFAULT);
    kprintf("  Model        %s (rev 0x%06x)\n", board_name(), rev);
    if (rev & (1 << 23))
        kprintf("  RAM total    %u MB\n", 256u << ((rev >> 20) & 7));
    kprintf("  RAM ARM/GPU  %u MB / %u MB\n", arm[1] >> 20, vc[1] >> 20);
    kprintf("  Serial no.   %08x%08x\n", ser[1], ser[0]);
    kprintf("  MAC          %02x:%02x:%02x:%02x:%02x:%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
    kprintf("  Firmware     0x%08x\n", fw[0]);
    con_color(COL_CYAN); con_puts("CPU\n"); con_color(COL_DEFAULT);
    kprintf("  Core         Cortex-A53, running on core 0 at EL%u\n", (u32)((el >> 2) & 3));
    kprintf("  ARM clock    %u MHz (max %u MHz)\n",
            clock_hz(3, 0x00030002) / 1000000, clock_hz(3, 0x00030004) / 1000000);
    kprintf("  Core clock   %u MHz\n", clock_hz(4, 0x00030002) / 1000000);
    if (t >= 0) kprintf("  Temperature  %d.%d C\n", t / 1000, (t % 1000) / 100);
    con_color(COL_CYAN); con_puts("System\n"); con_color(COL_DEFAULT);
    kprintf("  MMU/cache    %s\n", mmu_enabled() ? "enabled" : "disabled");
    if (io_core_active())
        kprintf("  Cores        0: shell + programs, 1: USB + network (%u rounds), 2-3: %d worker(s)\n", (u32)smp_io_loops(), smp_workers());
    else
        kprintf("  Cores        only core 0 is used (core 1 did not start)\n");
    if (fb_ready()) kprintf("  Display      %ux%u, 32 bpp\n", fb_width(), fb_height());
    else            kprintf("  Display      not available\n");
}

static void cmd_temp(int argc, char **argv)
{
    (void)argc; (void)argv;
    int t = temp_mc();
    if (t < 0) { kprintf("temperature not available\n"); return; }
    kprintf("CPU: %d.%d C\n", t / 1000, (t % 1000) / 100);
}

static void cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    u64 us = timer_us();
    u64 s = us / 1000000;
    kprintf("up %ud %02u:%02u:%02u.%03u\n",
            (u32)(s / 86400), (u32)(s / 3600 % 24), (u32)(s / 60 % 60),
            (u32)(s % 60), (u32)(us / 1000 % 1000));
}

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) { con_puts(argv[i]); if (i + 1 < argc) con_putc(' '); }
    con_putc('\n');
}

static void cmd_clear(int argc, char **argv) { (void)argc; (void)argv; con_clear(); }

/* history: the last LE_HIST commands, numbered; !N / !! run one again */
static void cmd_history(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "clear")) {
        hist.n = 0;
        if (fat_mounted()) fat_write_file("/HISTORY.TXT", "", 0);
        hist_saved = 0;
        return;
    }
    int first = hist.n > LE_HIST ? hist.n - LE_HIST : 0;
    for (int i = first; i < hist.n; i++) {
        con_color(COL_YELLOW); kprintf("%4d  ", i + 1);
        con_color(COL_DEFAULT); kprintf("%s\n", hist.line[i % LE_HIST]);
    }
}

/* /HISTORY.TXT: the history survives a reboot (one command per line, oldest first).
   Written by core 0 before each new command runs (FAT is core 0 only). */
static char hbuf[LE_HIST * LINE_MAX];


static void hist_load(void)
{
    struct fat_file f;
    hist_saved = 0;
    if (!fat_mounted() || fat_open("/HISTORY.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, hbuf, sizeof(hbuf) - 1);
    if (n <= 0) return;
    hbuf[n] = 0;
    for (char *p = hbuf; *p; ) {
        char *e = p;
        while (*e && *e != '\n') e++;
        int len = (int)(e - p);
        if (len && p[len - 1] == '\r') len--;
        if (len > 0 && len < LINE_MAX) {
            memcpy(hist.line[hist.n % LE_HIST], p, (u32)len);
            hist.line[hist.n % LE_HIST][len] = 0;
            hist.n++;
        }
        p = *e ? e + 1 : e;
    }
    hist_saved = hist.n;
}

static void hist_save(void)
{
    if (hist.n == hist_saved || !fat_mounted()) return;
    int first = hist.n > LE_HIST ? hist.n - LE_HIST : 0, n = 0;
    for (int i = first; i < hist.n; i++) {
        for (const char *s = hist.line[i % LE_HIST]; *s && n < (int)sizeof(hbuf) - 2; s++) hbuf[n++] = *s;
        hbuf[n++] = '\n';
    }
    fat_write_file("/HISTORY.TXT", hbuf, (u32)n);
    hist_saved = hist.n;
}

/* "!!" = the previous command, "!N" = command number N from 'history'.
   The expanded line replaces "!N" in the history. 0 = not found. */
static int hist_expand(char *line)
{
    int n = hist.n - 1;                     /* index of the "!..." line itself */
    int want;
    if (line[1] == '!' && !line[2]) want = n - 1;
    else {
        want = 0;
        const char *p = line + 1;
        if (*p < '0' || *p > '9') return 0;
        while (*p >= '0' && *p <= '9') want = want * 10 + (*p++ - '0');
        if (*p) return 0;
        want--;
    }
    int first = hist.n > LE_HIST ? hist.n - LE_HIST : 0;
    if (want < first || want >= n) return 0;
    const char *src = hist.line[want % LE_HIST];
    int k = 0;
    for (; src[k] && k < LINE_MAX - 1; k++) line[k] = src[k];
    line[k] = 0;
    memcpy(hist.line[n % LE_HIST], line, k + 1);
    if (n > 0 && !strcmp(hist.line[(n - 1) % LE_HIST], line)) hist.n--;   /* no duplicate */
    con_puts(line); con_putc('\n');
    return 1;
}

static void cmd_led(int argc, char **argv)
{
    if (argc < 2) { need("led on|off|blink [count]"); return; }
    if (!strcmp(argv[1], "on"))       { board_led(1); kprintf("LED on\n"); }
    else if (!strcmp(argv[1], "off")) { board_led(0); kprintf("LED off\n"); }
    else if (!strcmp(argv[1], "blink")) {
        u64 n = 5;
        if (argc > 2) parse_num(argv[2], &n);
        kprintf("blinking %u times (Ctrl+C stops)\n", (u32)n);
        for (u64 i = 0; i < n; i++) {
            board_led(1); delay_us(200000);
            board_led(0); delay_us(200000);
            if (con_trygetc() == 3) break;
        }
    } else need("led on|off|blink [count]");
}

static const char *fn_name(int pin)
{
    static const char *n[8] = { "in", "out", "alt5", "alt4", "alt0", "alt1", "alt2", "alt3" };
    u32 v = mmio_read(GPFSEL0 + (pin / 10) * 4);
    return n[(v >> ((pin % 10) * 3)) & 7];
}

static void cmd_gpio(int argc, char **argv)
{
    u64 pin;
    if (argc == 1) {
        for (int p = 0; p < 28; p++) {
            kprintf("GPIO%-2u %-4s %u   ", p, fn_name(p), gpio_get(p));
            if (p % 4 == 3) con_putc('\n');
        }
        return;
    }
    if (argc < 3 || !parse_num(argv[1], &pin) || pin > 53) {
        need("gpio [pin in|out|hi|lo|toggle|read]");
        return;
    }
    if (pin == 14 || pin == 15) kprintf("warning: GPIO14/15 are the UART console\n");
    const char *op = argv[2];
    if      (!strcmp(op, "in"))  gpio_func(pin, 0);
    else if (!strcmp(op, "out")) gpio_func(pin, 1);
    else if (!strcmp(op, "hi"))  { gpio_func(pin, 1); gpio_set(pin, 1); }
    else if (!strcmp(op, "lo"))  { gpio_func(pin, 1); gpio_set(pin, 0); }
    else if (!strcmp(op, "toggle")) { gpio_func(pin, 1); gpio_set(pin, !gpio_get(pin)); }
    else if (strcmp(op, "read")) { need("gpio [pin in|out|hi|lo|toggle|read]"); return; }
    kprintf("GPIO%u: %s, level %u\n", (u32)pin, fn_name(pin), gpio_get(pin));
}

static void cmd_mem(int argc, char **argv)
{
    u64 addr, len = 128;
    if (argc < 2 || !parse_num(argv[1], &addr)) { need("mem <address> [length]"); return; }
    if (argc > 2) parse_num(argv[2], &len);
    if (len > 4096) len = 4096;
    addr &= ~3UL;
    for (u64 off = 0; off < len; off += 16) {
        u8 line[16];
        for (int i = 0; i < 16; i += 4) {
            u32 w = *(volatile u32 *)(addr + off + i);
            memcpy(line + i, &w, 4);
        }
        con_color(COL_GRAY); kprintf("%08lx  ", addr + off); con_color(COL_DEFAULT);
        for (int i = 0; i < 16; i++) kprintf("%02x%s", line[i], i == 7 ? "  " : " ");
        con_color(COL_GRAY); con_puts(" |");
        for (int i = 0; i < 16; i++) con_putc(line[i] >= 32 && line[i] < 127 ? line[i] : '.');
        con_puts("|\n"); con_color(COL_DEFAULT);
    }
}

static void cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("rebooting...\n");
    delay_us(50000);
    sys_reboot();
}

static void cmd_halt(int argc, char **argv)
{
    (void)argc; (void)argv;
    board_led(0);
    kprintf("System halted. It is now safe to remove power.\n");
    for (;;) __asm__ volatile("wfi");
}

static void cmd_usb(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "debug")) {
        usb_debug = !(argc > 2 && !strcmp(argv[2], "off"));
        kprintf("USB debug %s\n", usb_debug ? "on" : "off");
        return;
    }
    io_lock();                           /* the USB controller is core 0's for a while */
    if (argc > 1 && !strcmp(argv[1], "test")) {
        if (!kbd_present()) kprintf("no keyboard connected\n");
        else kbd_selftest(8000);
        io_unlock();
        return;
    }
    if (argc > 1 && !strcmp(argv[1], "reset")) {
        kprintf("reinitializing USB...\n");
        kprintf("devices found: %d\n", usb_init());
    } else if (argc > 1) {
        io_unlock();
        need("usb [reset|test|debug on|off]");
        return;
    }
    usb_print_tree();
    if (kbd_present()) kprintf("keyboard: %s, mode %s\n", kbd_name(), kbd_mode());
    io_unlock();
}

/* ---------------- network ---------------- */

static void put_mac(const u8 *m)
{
    kprintf("%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void net_status(void)
{
    int code, full = 0; u32 hc;
    const char *bad = eth_error(&code, &hc);
    kprintf("chip:  %s, id %08x\n", eth_chip_name(), eth_chip());
    if (bad) { kprintf("init:  FAILED at %s (%d, HCINT %x)\n", bad, code, hc); return; }
    kprintf("MAC:   "); put_mac(eth_mac()); con_putc('\n');
    int sp = eth_link(&full);
    if (sp) kprintf("link:  up, %d Mbit %s duplex\n", sp, full ? "full" : "half");
    else    kprintf("link:  down (cable? auto-negotiation takes a few seconds)\n");
    kprintf("stats: rx %u ok %u bad %u err, tx %u ok %u err",
            eth_stat.rx_ok, eth_stat.rx_bad, eth_stat.rx_err, eth_stat.tx_ok, eth_stat.tx_err);
    if (eth_stat.rx_err || eth_stat.tx_err)
        kprintf(" (last %d, HCINT %x)", eth_stat.last_err, eth_stat.last_hcint);
    con_putc('\n');
}

/* send a broadcast test frame every second and list what arrives */
static void net_test(u32 secs)
{
    static u8 f[1600];
    u8 tx[64];
    u32 shown = 0, sent = 0;
    u64 end = timer_us() + (u64)secs * 1000000, next = 0;

    con_color(COL_YELLOW);
    kprintf("  Network test %u s: sending a broadcast frame (type 88b5) every second,\n"
            "  listing received frames. Ctrl+C stops.\n", secs);
    con_color(COL_DEFAULT);
    while (timer_us() < end) {
        if (timer_us() >= next) {
            next = timer_us() + 1000000;
            memset(tx, 0, sizeof(tx));
            memset(tx, 0xFF, 6);
            memcpy(tx + 6, eth_mac(), 6);
            tx[12] = 0x88; tx[13] = 0xB5;                /* local experimental EtherType */
            memcpy(tx + 14, "RoboOS hello", 12);
            tx[27] = (u8)sent++;
            eth_send(tx, 60);
        }
        int n = eth_recv(f, sizeof(f));
        if (n >= 14) {
            if (shown < 24) {
                kprintf("  %4u B  ", n); put_mac(f + 6); kprintf(" -> "); put_mac(f);
                kprintf("  type %02x%02x", f[12], f[13]);
                if (f[12] == 0x88 && f[13] == 0xB5) { con_color(COL_GREEN); kprintf("  RoboOS!"); con_color(COL_DEFAULT); }
                con_putc('\n');
            } else if (shown == 24) {
                kprintf("  ... (more frames are only counted)\n");
            }
            shown++;
        }
        if (con_trygetc() == 3) break;
    }
    net_status();
    kprintf("       rx polls with no data: %u\n", eth_stat.rx_empty);
}

static void cmd_net(int argc, char **argv)
{
    static const char *usage = "net [who | send <name|all> <text> | name [new] | speed 100|1000 | loop on|off | test [s]]";
    if (argc > 2 && !strcmp(argv[1], "speed")) {
        if (eth_set_gigabit(!strcmp(argv[2], "1000")) < 0) { kprintf("only the Pi 3B+ chip (LAN7515) can do 1000 Mbit\n"); return; }
        kprintf("negotiating the link again (up to 1000 Mbit: %s); see 'net' in a few seconds\n", eth_gigabit() ? "yes" : "no");
        return;
    }
    if (argc > 1 && !strcmp(argv[1], "loop")) {
        net_loop(!(argc > 2 && !strcmp(argv[2], "off")));
        kprintf("loopback %s\n", net_looping() ? "on: messages you send come back to this board" : "off");
        return;
    }
    if (argc > 1 && !strcmp(argv[1], "name")) {
        if (argc > 2) {
            int r = net_set_name(argv[2]);
            if (r < 0) { kprintf("bad name: 1-15 letters, digits, - or _ (not 'all')\n"); return; }
            if (r > 0) kprintf("(not saved: no SD card or write error)\n");
        }
        kprintf("name:  %s\n", net_name());
        return;
    }
    if (argc > 1 && !strcmp(argv[1], "who")) { net_who(); return; }
    if (argc > 1 && !strcmp(argv[1], "send")) {
        char text[NET_TEXT_MAX + 1];
        int n = 0;
        if (argc < 4) { need(usage); return; }
        for (int i = 3; i < argc; i++) {
            if (i > 3 && n < NET_TEXT_MAX) text[n++] = ' ';
            for (const char *s = argv[i]; *s && n < NET_TEXT_MAX; s++) text[n++] = *s;
        }
        text[n] = 0;
        int r = net_send(argv[2], text);
        if (r < 0) con_color(COL_RED);
        kprintf("%s\n", net_error(r));
        con_color(COL_DEFAULT);
        return;
    }
    if (!eth_present()) {
        kprintf("no Ethernet chip found (LAN9514 on Pi 3B, LAN7515 on Pi 3B+)\n");
        kprintf("name:  %s%s\n", net_name(), net_looping() ? "  (loopback on)" : "");
        return;
    }
    if (argc > 1 && !strcmp(argv[1], "test")) {
        u64 s = 10;
        if (argc > 2) parse_num(argv[2], &s);
        if (eth_error(0, 0)) { net_status(); return; }
        io_lock();                       /* the frames are core 0's during the test */
        net_test((u32)s);
        io_unlock();
    } else if (argc > 1) {
        need(usage);
    } else {
        kprintf("name:  %s%s\n", net_name(), net_looping() ? "  (loopback on)" : "");
        net_status();
    }
}

/* ---------------- SD card ---------------- */

static int need_fs(void)
{
    if (fat_mounted()) return 1;
    kprintf("no SD card filesystem (try 'sd init')\n");
    return 0;
}

static int ls_cb(const struct fat_dirent *e, void *ctx)
{
    int *n = ctx;
    (*n)++;
    if (e->is_dir) {
        con_color(COL_CYAN); kprintf("  %-32s", e->name); con_color(COL_DEFAULT);
        kprintf("   <dir>\n");
    } else {
        kprintf("  %-32s %8u\n", e->name, e->size);
    }
    return 0;
}

static void cmd_ls(int argc, char **argv)
{
    if (!need_fs()) return;
    char path[PATH_MAX_LEN];
    int n = 0;
    if (path_abs(argc > 1 ? argv[1] : ".", path, sizeof(path))) { kprintf("path too long\n"); return; }
    int r = fat_list(path, ls_cb, &n);
    if (r == -2) {                                       /* a file: show just that one */
        struct fat_file f;
        if (!fat_open(path, &f)) kprintf("  %-32s %8u\n", argv[1], f.size);
    }
    else if (r)  kprintf("not found: %s\n", argc > 1 ? argv[1] : path);
    else if (!n) kprintf("  (empty)\n");
}

static void cmd_cat(int argc, char **argv)
{
    struct fat_file f;
    u8 buf[256];
    char path[PATH_MAX_LEN];
    if (argc < 2) { need("cat <file>"); return; }
    if (!need_fs()) return;
    if (path_abs(argv[1], path, sizeof(path)) || fat_open(path, &f) || f.is_dir) { kprintf("not found: %s\n", argv[1]); return; }
    int last = '\n';
    for (;;) {
        int n = fat_read(&f, buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            u8 c = buf[i];
            if (c == '\r') continue;
            con_putc((c == '\n' || c == '\t' || (c >= 32 && c < 127)) ? c : '.');
            last = c;
        }
        if (con_trygetc() == 3) { kprintf("^C"); last = 0; break; }
    }
    if (last != '\n') con_putc('\n');
}

static void cmd_run(int argc, char **argv)
{
    char path[96];
    if (argc < 2) { need("run <program>   (.bin or .bas; looks here, in /RUN and /APPS)"); return; }
    if (!need_fs()) return;
    if (app_resolve(argv[1], path, sizeof(path))) { kprintf("program not found: %s\n", argv[1]); return; }
    app_start(path);
}

static void cmd_edit(int argc, char **argv)
{
    char path[PATH_MAX_LEN];
    if (argc < 2) { need("edit <file>   (Ctrl+S save, Ctrl+Q quit)"); return; }
    if (path_abs(argv[1], path, sizeof(path))) { kprintf("path too long\n"); return; }
    editor_main(path);
}

static void cmd_cd(int argc, char **argv)
{
    if (!need_fs()) return;
    const char *p = argc > 1 ? argv[1] : "/";
    switch (cwd_set(p)) {
    case -1: kprintf("not found: %s\n", p); break;
    case -2: kprintf("not a directory: %s\n", p); break;
    case -3: kprintf("path too long\n"); break;
    }
}

static void cmd_mkdir(int argc, char **argv)
{
    char path[PATH_MAX_LEN];
    if (argc < 2) { need("mkdir <directory>...   (8.3 names)"); return; }
    if (!need_fs()) return;
    for (int i = 1; i < argc; i++) {
        int r = path_abs(argv[i], path, sizeof(path)) ? -2 : fat_mkdir(path);
        if (r) kprintf("mkdir %s: %s\n", argv[i], fat_write_error(r));
    }
}

/* the firmware and kernel files in / - deleting them makes the card unbootable */
static int boot_file(const char *path)
{
    static const char *f[] = { "/bootcode.bin", "/start.elf", "/fixup.dat", "/kernel8.img", "/config.txt" };
    for (u32 i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        const char *a = path, *b = f[i];
        while (*a && *b && (*a | 32) == (*b | 32)) { a++; b++; }
        if (!*a && !*b) return 1;
    }
    return 0;
}

/* is dir the current directory or one of its parents? */
static int in_use(const char *dir)
{
    const char *c = cwd_get();
    int n = (int)strlen(dir);
    for (int i = 0; i < n; i++) {
        char a = dir[i], b = c[i];
        if (!b) return 0;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return c[n] == 0 || c[n] == '/';
}

static void cmd_rm(int argc, char **argv)
{
    char path[PATH_MAX_LEN];
    int force = 0, first = 1;
    if (argc > 1 && !strcmp(argv[1], "-f")) { force = 1; first = 2; }
    if (argc <= first) { need("rm [-f] <file or empty directory>..."); return; }
    if (!need_fs()) return;
    for (int i = first; i < argc; i++) {
        struct fat_file f;
        if (path_abs(argv[i], path, sizeof(path)) || fat_open(path, &f)) {
            kprintf("rm %s: not found\n", argv[i]);
            continue;
        }
        if (!strcmp(path, "/")) { kprintf("rm: not the top directory\n"); continue; }
        if (f.is_dir && in_use(path)) { kprintf("rm %s: you are in it (cd somewhere else first)\n", argv[i]); continue; }
        if (boot_file(path) && !force) {
            kprintf("rm %s: a boot file - without it the card does not start (rm -f deletes it anyway)\n", argv[i]);
            continue;
        }
        int r = fat_remove(path);
        if (r) kprintf("rm %s: %s\n", argv[i], fat_write_error(r));
    }
}

static void cmd_pwd(int argc, char **argv) { (void)argc; (void)argv; kprintf("%s\n", cwd_get()); }

static int idle_break(void) { return con_trygetc() == 3; }

static void cmd_beep(int argc, char **argv)
{
    u64 f = 880, ms = 150;
    if (argc > 1) parse_num(argv[1], &f);
    if (argc > 2) parse_num(argv[2], &ms);
    if (f < 20 || f > 20000) { need("beep [frequency 20-20000 Hz] [ms]"); return; }
    if (ms > 60000) ms = 60000;
    if (sound_beep((int)f, (int)ms, idle_break)) kprintf("^C\n");
}

static void cmd_play(int argc, char **argv)
{
    char m[LINE_MAX];
    int n = 0, err = 0;
    if (argc < 2) { need("play <notes>   e.g. play T140 L8 C D E F G4 G4"); return; }
    for (int i = 1; i < argc; i++) {
        if (i > 1) m[n++] = ' ';
        for (const char *s = argv[i]; *s && n < LINE_MAX - 2; s++) m[n++] = *s;
    }
    m[n] = 0;
    int r = sound_play(m, idle_break, &err);
    sound_stop();
    if (r < 0) kprintf("?unknown character at position %d: %s\n", err, m);
    else if (r) kprintf("^C\n");
}

static void cmd_playwav(int argc, char **argv)
{
    char path[PATH_MAX_LEN];
    if (argc < 2) { need("playwav <file.wav>   (jack, GPIO18/19 or I2S DAC - see 'sound')"); return; }
    if (!need_fs()) return;
    if (path_abs(argv[1], path, sizeof(path))) { kprintf("path too long\n"); return; }
    kprintf("playing %s (Ctrl+C stops)\n", path);
    int r = sound_wav(path, idle_break);
    if (r < 0) kprintf("cannot play: %s\n", i2s_wav_error(r));
    else if (r) kprintf("^C\n");
}

static void cmd_sound(int argc, char **argv)
{
    u64 v;
    if (argc > 2 && !strcmp(argv[1], "volume") && parse_num(argv[2], &v)) sound_volume((int)v);
    else if (argc > 2 && !strcmp(argv[1], "out") && !strcmp(argv[2], "jack")) sound_output(0);
    else if (argc > 2 && !strcmp(argv[1], "out") && !strcmp(argv[2], "gpio")) sound_output(1);
    else if (argc > 2 && !strcmp(argv[1], "out") && !strcmp(argv[2], "i2s")) sound_output(2);
    else if (argc > 1 && !strcmp(argv[1], "off")) sound_stop();
    else if (argc > 1 && !strcmp(argv[1], "test")) { sound_test(); return; }
    else if (argc > 1) { need("sound [volume 0-100 | out jack|gpio|i2s | off | test]"); return; }
    kprintf("output: %s\nvolume: %d %%\n", sound_pins(), sound_get_volume());
    if (sound_output_mode() == 2 || i2s_segments()) i2s_status();
}

static void cmd_screen(int argc, char **argv)
{
    u32 nw, nh;
    fb_native(&nw, &nh);
    if (argc > 1) {
        u32 w = 0, h = 0;
        char save[24];
        int n = 0;
        if (!strcmp(argv[1], "auto")) {
            for (const char *s = "auto"; *s; s++) save[n++] = *s;
        } else {
            const char *p = argv[1];
            while (*p >= '0' && *p <= '9') w = w * 10 + (u32)(*p++ - '0');
            if (*p == 'x' || *p == 'X') p++;
            while (*p >= '0' && *p <= '9') h = h * 10 + (u32)(*p++ - '0');
            if (*p || w < 320 || h < 200 || w > 2048 || h > 1280) {
                need("screen [WIDTHxHEIGHT | auto]   (320x200 .. 2048x1280, auto = the monitor's own)");
                return;
            }
            for (const char *s = argv[1]; *s && n < 20; s++) save[n++] = *s;
        }
        if (!fb_ready()) { kprintf("no HDMI screen\n"); return; }
        if (!screen_set(w, h)) { kprintf("the firmware refused %s\n", argv[1]); screen_set(1024, 768); return; }
        save[n++] = '\r'; save[n++] = '\n'; save[n] = 0;
        if (fat_mounted()) fat_write_file("/SCREEN.TXT", save, (u32)n);
    }
    if (fb_ready()) kprintf("screen:  %ux%u pixels, %ux%u characters\n", fb_width(), fb_height(), fb_width() / 8, fb_height() / 16);
    else            kprintf("screen:  no HDMI\n");
    kprintf("monitor: %ux%u (screen auto)\n", nw, nh);
}

static void cmd_basic(int argc, char **argv)
{
    basic_main(argc > 1 ? argv[1] : 0);
}

static void cmd_sd(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "init")) {
        int which = argc > 2 ? (!strcmp(argv[2], "sdhost") ? 1 : !strcmp(argv[2], "emmc") ? 2 : -1) : 0;
        if (which < 0) { kprintf("usage: sd init [sdhost|emmc]\n"); return; }
        int e = sd_init_on(which);
        if (e) { kprintf("SD init failed: "); sd_print_fail(e); return; }
        int m = fat_mount();
        if (m) kprintf("card OK, but no FAT16/FAT32 partition (%d)\n", m);
    }
    if (!sd_ready()) { kprintf("no SD card (try 'sd init')\n"); return; }
    kprintf("card:  %s, %u MB, %u-bit bus, controller %s\n", sd_is_sdhc() ? "SDHC/SDXC" : "SDSC",
            (u32)(sd_sectors() >> 11), sd_bus_width(), sd_controller());
    if (sd_sdhost_error()) { kprintf("SDHOST not used: "); sdhost_print_fail(sd_sdhost_error()); }
    if (fat_mounted())
        kprintf("fs:    %s, %u MB, cluster %u bytes\n", fat_type(),
                (u32)(fat_volume_bytes() >> 20), fat_cluster_bytes());
    else
        kprintf("fs:    not mounted\n");
}

struct cmd {
    const char *name, *args, *help;
    void (*fn)(int, char **);
};

static void logout_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!users_console_active()) { kprintf("the shell has no login ('users console on' turns it on)\n"); return; }
    kprintf("bye, %s\n", users_console_user());
    users_logout();
}

static const struct cmd cmds[] = {
    { "help",   "",                     "list of commands",                cmd_help   },
    { "info",   "",                     "board and system information",    cmd_info   },
    { "temp",   "",                     "CPU temperature",                 cmd_temp   },
    { "uptime", "",                     "time since boot",                 cmd_uptime },
    { "echo",   "<text>",               "print text",                      cmd_echo   },
    { "led",    "on|off|blink [n]",     "control the green ACT LED",       cmd_led    },
    { "gpio",   "[pin op]",             "pin states / in out hi lo toggle read", cmd_gpio },
    { "mem",    "<addr> [length]",      "memory hexdump",                  cmd_mem    },
    { "usb",    "[reset|test|debug]",   "USB devices, keyboard test",      cmd_usb    },
    { "net",    "[who|send|name|test]", "network: status, messages",       cmd_net    },
    { "ip",     "[dhcp|set a [m] [g]]", "IP address (DHCP or static)",     ip_cmd     },
    { "ping",   "<address|name> [n]",   "ping another computer",           ping_cmd   },
    { "http",   "[on|off|token|tls]",   "web page, JSON API, HTTPS",       http_cmd   },
    { "wifi",   "[init|scan]",          "WiFi chip: firmware, scan",       wifi_cmd   },
    { "i2c",    "scan|read|write",      "I2C bus on GPIO2/3",              i2c_cmd    },
    { "bt",     "[on|scan [s]|off]",    "Bluetooth LE: scan for devices",  bt_cmd     },
    { "users",  "[add|passwd|del|console]", "logins: web page, API, shell", users_cmd },
    { "logout", "",                     "end the shell login",           logout_cmd },
    { "mqtt",   "[broker|pub|off|..]", "MQTT client (IoT broker)",       mqtt_cmd   },
    { "ai",     "<question>|@cmd ..|key|model", "ask Claude (AI, needs an API key)", ai_cmd },
    { "radio",  "[MHz|up|down|scan|vol|off]", "FM radio (RDA5807M module)",  radio_cmd  },
    { "motion", "[watch|zero|threshold mg|alarm on|off]", "tilt, acceleration, shakes (MPU-6050)", motion_cmd },
    { "thermal", "[once|range lo hi|person C]", "8x8 thermal camera (AMG8833)", thermal_cmd },
    { "voice",  "[add id cmd|del id|volume|mute|wake]", "offline voice commands (DF2301Q)", voice_cmd },
    { "dcf",    "[on|off|pin n|invert|pull|pon]", "DCF77 radio clock receiver", dcf_cmd    },
    { "display", "[scan|text|status|..]", "I2C OLED / LCD display",        display_cmd },
    { "sensors", "[scan|log min|off|now]", "temperature, humidity, ADC, RTC; CSV log", sensors_cmd },
    { "time",   "[sync|zone|server]",   "date and time (NTP), time zone",  time_cmd   },
    { "ls",     "[path]",               "list a directory on the SD card", cmd_ls     },
    { "cd",     "[dir]",                "change directory (.. = up, / = top)", cmd_cd },
    { "pwd",    "",                     "show the current directory",      cmd_pwd    },
    { "view",   "[picture|dir]",        "show pictures (JPEG PNG BMP GIF)", view_cmd  },
    { "video",  "<file.mpg>",           "play MPEG-1 video + MP2 sound",    video_cmd },
    { "mkdir",  "<dir>",                "make a directory",                cmd_mkdir  },
    { "rm",     "[-f] <file|dir>",      "delete a file or an empty directory", cmd_rm },
    { "cat",    "<file>",               "print a text file",               cmd_cat    },
    { "update", "[check|get|url|restore]", "kernel updates: download, or sent from a PC", cmd_update },
    { "edit",   "<file>",               "full-screen text editor",         cmd_edit   },
    { "run",    "<program>",            "run a .bin / .bas program",       cmd_run    },
    { "basic",  "[file]",               "BASIC interpreter",               cmd_basic  },
    { "beep",   "[Hz] [ms]",            "a tone on the audio jack",        cmd_beep   },
    { "play",   "<notes>",              "play music: play T120 C D E F G", cmd_play   },
    { "sound",  "[volume|out|off]",     "sound: volume, out, off, test",  cmd_sound },
    { "playwav", "<file>",              "play a WAV file",                 cmd_playwav },
    { "sd",     "[init [sdhost|emmc]]", "SD card info / re-initialize",    cmd_sd     },
    { "screen", "[WxH|auto]",           "screen resolution (saved)",       cmd_screen },
    { "history", "[clear]",             "last 30 commands, kept on the card", cmd_history },
    { "clear",  "",                     "clear the screen",                cmd_clear  },
    { "reboot", "",                     "reboot via watchdog",             cmd_reboot },
    { "halt",   "",                     "halt the system",                 cmd_halt   },
};
#define NCMDS (sizeof(cmds) / sizeof(cmds[0]))

static void cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (u32 i = 0; i < NCMDS; i++) {
        char full[40];
        int n = 0;
        for (const char *s = cmds[i].name; *s; s++) full[n++] = *s;
        if (cmds[i].args[0]) { full[n++] = ' '; for (const char *s = cmds[i].args; *s; s++) full[n++] = *s; }
        full[n] = 0;
        con_color(COL_YELLOW); kprintf("  %-26s", full);
        con_color(COL_DEFAULT); kprintf("%s\n", cmds[i].help);
    }
    con_color(COL_GRAY);
    con_puts("  Tab = complete, Up/Down = history (!N), Left/Right/Home/End = cursor, Ctrl+C = cancel\n");
    con_color(COL_DEFAULT);
}

/* ---------------- line editor ---------------- */

static void prompt(void)
{
    con_color(COL_GREEN); con_puts("robo@"); con_puts(board_host());
    con_color(COL_DEFAULT); con_puts(":");
    con_color(COL_CYAN);
    const char *d = cwd_get();
    int n = (int)strlen(d);
    if (n > 40) { con_puts("..."); d += n - 37; }       /* keep the prompt short */
    con_puts(d);
    con_color(COL_DEFAULT); con_puts("$ ");
}

/* Tab after the command name: complete a file or directory name */
struct pc { const char *pre; int plen; int n; char first[FAT_MAX_NAME]; int common; int dir; int list; };

static int pc_match(const struct fat_dirent *e, const struct pc *c)
{
    for (int i = 0; i < c->plen; i++) {
        char a = e->name[i], b = c->pre[i];
        if (!a) return 0;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

static int pc_cb(const struct fat_dirent *e, void *ctx)
{
    struct pc *c = ctx;
    if (!pc_match(e, c)) return 0;
    if (c->list) {
        if (e->is_dir) { con_color(COL_CYAN); kprintf("%s/  ", e->name); con_color(COL_DEFAULT); }
        else kprintf("%s  ", e->name);
        return 0;
    }
    if (!c->n++) {
        memcpy(c->first, e->name, FAT_MAX_NAME);
        c->common = (int)strlen(e->name);
        c->dir = e->is_dir;
    } else {                                     /* shorten to what all matches share */
        int i = c->plen;
        while (i < c->common && e->name[i] && (e->name[i] | 32) == (c->first[i] | 32)) i++;
        c->common = i;
    }
    return 0;
}

static int complete_path(char *line, int len)
{
    int w = len;
    while (w > 0 && line[w - 1] != ' ') w--;
    if (line[w] == '"') return len;              /* quoted names: no completion */
    int slash = -1;
    for (int i = w; i < len; i++) if (line[i] == '/') slash = i;
    char dir[PATH_MAX_LEN], rel[PATH_MAX_LEN];
    int dl = slash < 0 ? 0 : slash - w + 1;
    memcpy(rel, line + w, dl);
    rel[dl] = 0;
    if (path_abs(dl ? rel : ".", dir, sizeof(dir))) return len;
    struct pc c;
    memset(&c, 0, sizeof(c));
    c.pre = line + w + dl;
    c.plen = len - w - dl;
    fat_list(dir, pc_cb, &c);
    if (!c.n) return len;
    int add = 0, space = 0;
    for (int i = 0; i < c.common; i++) if (c.first[i] == ' ') space = 1;
    if (space && len + 2 < LINE_MAX - 2) {       /* a name with spaces: put the word in "quotes" */
        for (int i = len; i > w; i--) line[i] = line[i - 1];
        line[w] = '"';
        len++;
        for (int i = w; i < len - 1; i++) con_putc('\b');
        for (int i = w; i < len; i++) con_putc(line[i]);
    }
    if (c.common > c.plen || c.n == 1) {         /* spell the typed part as on the card */
        for (int i = 0; i < c.plen; i++) con_putc('\b');
        for (int i = 0; i < c.plen; i++) { line[len - c.plen + i] = c.first[i]; con_putc(c.first[i]); }
    }
    for (int i = c.plen; i < c.common && len < LINE_MAX - 3; i++) {
        line[len++] = c.first[i]; con_putc(c.first[i]); add++;
    }
    if (c.n == 1 && add == c.common - c.plen) {
        if (space && !c.dir) { line[len++] = '"'; con_putc('"'); }
        char end = c.dir ? '/' : ' ';
        line[len++] = end; con_putc(end);
    } else if (c.n > 1 && !add) {                /* nothing more in common: show the choices */
        c.list = 1;
        con_putc('\n');
        fat_list(dir, pc_cb, &c);
        con_putc('\n');
        prompt();
        line[len] = 0; con_puts(line);
    }
    return len;
}

static int complete(char *line, int len)
{
    const struct cmd *hit = 0;
    int matches = 0;
    if (!len || line[0] == ' ' || len >= LINE_MAX - 16) return len;
    for (int i = 0; i < len; i++)
        if (line[i] == ' ') return fat_mounted() ? complete_path(line, len) : len;
    for (u32 i = 0; i < NCMDS; i++)
        if (!strncmp(cmds[i].name, line, len)) { hit = &cmds[i]; matches++; }
    if (matches == 1) {
        for (const char *s = hit->name + len; *s; s++) { line[len++] = *s; con_putc(*s); }
        line[len++] = ' '; con_putc(' ');
    } else if (matches > 1) {
        con_putc('\n');
        for (u32 i = 0; i < NCMDS; i++)
            if (!strncmp(cmds[i].name, line, len)) kprintf("%s  ", cmds[i].name);
        con_putc('\n');
        prompt();
        line[len] = 0; con_puts(line);
    }
    return len;
}

static void execute(char *line)
{
    char *argv[ARGS_MAX];
    int argc = 0;
    char *p = line;
    while (*p && argc < ARGS_MAX) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        if (*p == '"') {                           /* "quoted argument" */
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = 0;
            continue;
        }
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    if (!argc) return;
    for (u32 i = 0; i < NCMDS; i++)
        if (!strcmp(argv[0], cmds[i].name)) { cmds[i].fn(argc, argv); return; }
    con_color(COL_RED); kprintf("unknown command: %s", argv[0]);
    con_color(COL_DEFAULT); con_puts("  (try 'help')\n");
}

/* for ai.c: run one command line, and the command list (name, arguments, help) as text */
void shell_execute(const char *cmdline)
{
    char l[LINE_MAX];
    int i = 0;
    for (; cmdline[i] && i < LINE_MAX - 1; i++) l[i] = cmdline[i];
    l[i] = 0;
    execute(l);
}

int shell_commands(char *b, int max)
{
    int n = 0;
    for (u32 i = 0; i < NCMDS; i++) {
        const char *parts[] = { cmds[i].name, cmds[i].args[0] ? " " : "", cmds[i].args, " - ", cmds[i].help, "\n" };
        for (int k = 0; k < 6; k++) for (const char *s = parts[k]; *s && n < max - 1; s++) b[n++] = *s;
    }
    b[n] = 0;
    return n;
}

void shell_run(void)
{
    char line[LINE_MAX];
    if (hist_saved < 0) hist_load();
    for (;;) {
        users_console_login();         /* 'users console on': an admin must log in first */
        prompt();
        net_notify(1);                 /* messages show up while waiting at the prompt */
        voice_at_prompt(1, prompt);    /* voice commands run only here */
        int len = line_edit(line, LINE_MAX, &hist, complete, prompt);
        voice_at_prompt(0, 0);
        net_notify(0);
        if (len > 0 && line[0] == '!' && line[1]) {
            if (!hist_expand(line)) { kprintf("%s: not in the history\n", line); hist.n--; continue; }
        }
        if (len > 0 && !strncmp(line, "ai key ", 7) && hist.n > 0 &&
            !strcmp(hist.line[(hist.n - 1) % LE_HIST], line)) hist.n--;    /* the API key never goes to the history */
        if (len > 0) { hist_save(); execute(line); }
    }
}

void shell_restart(void)
{
    con_puts("back to the shell\n");
    shell_run();
}
