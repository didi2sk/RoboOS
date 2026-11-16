/* Shell - line editor (history, Tab completion) and commands */
#include "kernel.h"
#include "usb.h"
#include "sd.h"

#define LINE_MAX  128
#define HIST_MAX  10
#define ARGS_MAX  8

static char hist[HIST_MAX][LINE_MAX];
static int  hist_n;

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

#define PM_RSTC     (MMIO_BASE + 0x10001C)
#define PM_WDOG     (MMIO_BASE + 0x100024)
#define PM_PASSWORD 0x5A000000

static void cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("rebooting...\n");
    delay_us(50000);
    mmio_write(PM_WDOG, PM_PASSWORD | 10);
    mmio_write(PM_RSTC, PM_PASSWORD | (mmio_read(PM_RSTC) & ~0x30u) | 0x20);
    for (;;) __asm__ volatile("wfe");
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
    if (argc > 1 && !strcmp(argv[1], "test")) {
        if (!kbd_present()) { kprintf("no keyboard connected\n"); return; }
        kbd_selftest(8000);
        return;
    }
    if (argc > 1 && !strcmp(argv[1], "reset")) {
        kprintf("reinitializing USB...\n");
        kprintf("devices found: %d\n", usb_init());
    } else if (argc > 1) {
        need("usb [reset|test|debug on|off]");
        return;
    }
    usb_print_tree();
    if (kbd_present()) kprintf("keyboard: %s, mode %s\n", kbd_name(), kbd_mode());
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
    const char *path = argc > 1 ? argv[1] : "/";
    int n = 0;
    int r = fat_list(path, ls_cb, &n);
    if (r == -2) kprintf("not a directory: %s\n", path);
    else if (r)  kprintf("not found: %s\n", path);
    else if (!n) kprintf("  (empty)\n");
}

static void cmd_cat(int argc, char **argv)
{
    struct fat_file f;
    u8 buf[256];
    if (argc < 2) { need("cat <file>"); return; }
    if (!need_fs()) return;
    if (fat_open(argv[1], &f) || f.is_dir) { kprintf("not found: %s\n", argv[1]); return; }
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
    if (argc < 2) { need("run <program>   (.bin or .bas; looks in /, /RUN and /APPS)"); return; }
    if (!need_fs()) return;
    if (app_resolve(argv[1], path, sizeof(path))) { kprintf("program not found: %s\n", argv[1]); return; }
    app_start(path);
}

static void cmd_basic(int argc, char **argv)
{
    basic_main(argc > 1 ? argv[1] : 0);
}

static void cmd_sd(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "init")) {
        int e = sd_init();
        if (e) { kprintf("SD init failed: "); sd_print_fail(e); return; }
        int m = fat_mount();
        if (m) kprintf("card OK, but no FAT16/FAT32 partition (%d)\n", m);
    }
    if (!sd_ready()) { kprintf("no SD card (try 'sd init')\n"); return; }
    kprintf("card:  %s, %u MB, %u-bit bus\n", sd_is_sdhc() ? "SDHC/SDXC" : "SDSC",
            (u32)(sd_sectors() >> 11), sd_bus_width());
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
    { "ls",     "[path]",               "list a directory on the SD card", cmd_ls     },
    { "cat",    "<file>",               "print a text file",               cmd_cat    },
    { "run",    "<program>",            "run a .bin / .bas program",       cmd_run    },
    { "basic",  "[file]",               "BASIC interpreter",               cmd_basic  },
    { "sd",     "[init]",               "SD card info / re-initialize",    cmd_sd     },
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
    con_puts("  Tab = complete, Up/Down = history, Ctrl+C = cancel line\n");
    con_color(COL_DEFAULT);
}

/* ---------------- line editor ---------------- */

static void prompt(void)
{
    con_color(COL_GREEN); con_puts("robo@"); con_puts(board_host());
    con_color(COL_DEFAULT); con_puts(":");
    con_color(COL_CYAN); con_puts("~");
    con_color(COL_DEFAULT); con_puts("$ ");
}

static void erase_line(int n) { while (n--) con_puts("\b \b"); }

static int complete(char *line, int len)
{
    const struct cmd *hit = 0;
    int matches = 0;
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

static int read_line(char *line)
{
    int len = 0, hpos = hist_n;
    for (;;) {
        char c = (char)con_getc();
        if (c == '\r' || c == '\n') {
            con_putc('\n');
            line[len] = 0;
            return len;
        } else if (c == 0x7F || c == '\b') {
            if (len) { len--; erase_line(1); }
        } else if (c == 3) {                       /* Ctrl+C */
            con_puts("^C\n");
            line[0] = 0;
            return 0;
        } else if (c == 12) {                      /* Ctrl+L */
            con_clear(); prompt(); line[len] = 0; con_puts(line);
        } else if (c == '\t') {
            if (!len || line[0] == ' ') continue;
            int sp = 0;
            for (int i = 0; i < len; i++) if (line[i] == ' ') sp = 1;
            if (!sp && len < LINE_MAX - 16) len = complete(line, len);
        } else if (c == 27) {                      /* ESC [ A/B */
            if (con_getc() != '[') continue;
            char k = (char)con_getc();
            int target = hpos;
            int oldest = hist_n > HIST_MAX ? hist_n - HIST_MAX : 0;
            if (k == 'A' && hpos > oldest) target = hpos - 1;
            else if (k == 'B' && hpos < hist_n) target = hpos + 1;
            if (target == hpos) continue;
            hpos = target;
            erase_line(len);
            if (hpos == hist_n) len = 0;
            else {
                const char *h = hist[hpos % HIST_MAX];
                for (len = 0; h[len]; len++) line[len] = h[len];
            }
            line[len] = 0; con_puts(line);
        } else if (c >= 32 && c < 127 && len < LINE_MAX - 1) {
            line[len++] = c;
            con_putc(c);
        }
    }
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

void shell_run(void)
{
    char line[LINE_MAX];
    for (;;) {
        prompt();
        int len = read_line(line);
        if (!len) continue;
        /* save to history (skip duplicate of the previous line) */
        if (!hist_n || strcmp(hist[(hist_n - 1) % HIST_MAX], line)) {
            memcpy(hist[hist_n % HIST_MAX], line, len + 1);
            hist_n++;
        }
        execute(line);
    }
}

void shell_restart(void)
{
    con_puts("back to the shell\n");
    shell_run();
}
