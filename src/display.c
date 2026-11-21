/*
 * display.c - small I2C displays on GPIO2/3 (the same bus as the sensors):
 *   OLED 128x64 / 128x32 with an SSD1306 or SH1106 controller at 0x3C / 0x3D (text in the 8x16 font:
 *        16 columns x 4 rows, or 2 rows on 128x32),
 *   text LCD 16x2 / 20x4 (HD44780) behind a PCF8574 "backpack" at 0x27 / 0x3F (any 0x20-0x27 / 0x38-0x3F
 *        with 'display addr'): P0 RS, P1 RW, P2 E, P3 backlight, P4-P7 = D4-D7, 4-bit mode.
 * Found at boot (display_boot: reads only, writes nothing to other chips), /DISPLAY.TXT overrides it
 * ("type lcd|ssd1306|sh1106|none", "addr 0x27", "size 20x4", "status on|off", "contrast 0-255").
 * The text lives in txt[][]; only changed rows go out (the bus is 100 kHz: a full OLED is ~0.1 s,
 * a row ~25 ms). Status screen (default on): name, IP, time, link - refreshed once a second from
 * display_poll() on core 0 (con_trygetc); text from a command / BASIC / a program turns it off.
 */
#ifndef DISPLAY_HOST_TEST
#include "kernel.h"
#include "sd.h"
#endif
extern const unsigned char font8x16[95][16];

enum { D_NONE, D_SSD1306, D_SH1106, D_LCD };
static const char *const tname[] = { "none", "ssd1306", "sh1106", "lcd" };

#define MAXR 4
#define MAXC 20
static int dtype, daddr, dw, dh, rows, cols;    /* dw x dh: pixels (OLED) or characters (LCD) */
static int cfg_type = -1, cfg_addr, cfg_w, cfg_h, status_on = 1, contrast = 0xCF, backlight = 1;
static u8 st_byte;                              /* OLED status byte read at detection (SSD1306 vs SH1106) */
static char txt[MAXR][MAXC + 1], sent[MAXR][MAXC + 1];
static int ok, busy, noack;                    /* noack: the display never ACKs - bit-banged writes */
static int dwr(u8 a, const void *b, int n) { return noack ? i2c_write_noack(a, b, n) : i2c_write(a, b, n); }
static u64 next_status;

/* ---------------- OLED ---------------- */

static int oled_cmds(const u8 *c, int n)
{
    u8 b[32];
    b[0] = 0x00;                                 /* control byte: commands follow */
    memcpy(b + 1, c, (size_t)n);
    return dwr((u8)daddr, b, n + 1);
}

static int oled_init(void)
{
    u8 mux = (u8)(dh - 1), pins = dh == 32 ? 0x02 : 0x12;
    if (dtype == D_SH1106) {
        u8 c[] = { 0xAE, 0xD5, 0x80, 0xA8, mux, 0xD3, 0x00, 0x40, 0xAD, 0x8B, 0xA1, 0xC8,
                   0xDA, pins, 0x81, (u8)contrast, 0xD9, 0x22, 0xDB, 0x35, 0xA4, 0xA6, 0xAF };
        return oled_cmds(c, sizeof(c));
    }
    u8 c[] = { 0xAE, 0xD5, 0x80, 0xA8, mux, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8,
               0xDA, pins, 0x81, (u8)contrast, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0x2E, 0xAF };
    return oled_cmds(c, sizeof(c));
}

/* one text row = 2 pages of 128 columns; the 8x16 font is 16 row bytes, bit 7 = the left pixel */
static int oled_row(int r)
{
    u8 b[1 + 128];
    for (int half = 0; half < 2; half++) {
        int page = r * 2 + half;
        if (page * 8 >= dh) break;
        b[0] = 0x40;                             /* control byte: data follow */
        for (int c = 0; c < 16; c++) {
            unsigned char ch = (unsigned char)txt[r][c];
            if (ch < 32 || ch > 126) ch = ch ? '?' : ' ';
            const unsigned char *g = font8x16[ch - 32] + half * 8;
            for (int x = 0; x < 8; x++) {
                u8 v = 0;
                for (int y = 0; y < 8; y++) if (g[y] & (0x80 >> x)) v |= (u8)(1 << y);
                b[1 + c * 8 + x] = v;
            }
        }
        int e;
        if (dtype == D_SH1106) {                 /* page addressing; the 132-column RAM shows from column 2 */
            u8 c[] = { (u8)(0xB0 + page), 0x02, 0x10 };
            e = oled_cmds(c, 3);
        } else {
            u8 c[] = { 0x21, 0, 127, 0x22, (u8)page, (u8)page };
            e = oled_cmds(c, 6);
        }
        if (!e) e = dwr((u8)daddr, b, sizeof(b));
        if (e) return e;
    }
    return 0;
}

/* ---------------- LCD on a PCF8574 ---------------- */

#define L_RS 0x01
#define L_E  0x04
#define L_BL 0x08

/* bytes for one 8-bit value (two nibbles, each E high then low) */
static int lcd_bytes(u8 *o, u8 v, u8 rs)
{
    u8 bl = backlight ? L_BL : 0, hi = (u8)(v & 0xF0), lo = (u8)(v << 4);
    o[0] = hi | bl | rs | L_E; o[1] = hi | bl | rs;
    o[2] = lo | bl | rs | L_E; o[3] = lo | bl | rs;
    return 4;
}

static int lcd_cmd(u8 v)
{
    u8 o[4];
    lcd_bytes(o, v, 0);
    int e = dwr((u8)daddr, o, 4);
    delay_us(v < 4 ? 2000 : 50);                 /* clear / home take 1.5 ms */
    return e;
}

static int lcd_init(void)
{
    u8 bl = backlight ? L_BL : 0;
    delay_us(50000);
    for (int i = 0; i < 3; i++) {                /* to 8-bit mode for sure, then 4-bit (HD44780 datasheet fig. 24) */
        u8 o[2] = { (u8)(0x30 | bl | L_E), (u8)(0x30 | bl) };
        if (dwr((u8)daddr, o, 2)) return -1;
        delay_us(i ? 200 : 4500);
    }
    u8 o[2] = { (u8)(0x20 | bl | L_E), (u8)(0x20 | bl) };
    if (dwr((u8)daddr, o, 2)) return -1;
    delay_us(200);
    lcd_cmd(0x28);                               /* 4-bit, 2 lines (also 4-line modules), 5x8 */
    lcd_cmd(0x0C);                               /* display on, no cursor */
    lcd_cmd(0x06);                               /* entry: move right */
    return lcd_cmd(0x01);                        /* clear */
}

static int lcd_row(int r)
{
    static const u8 off20[4] = { 0x00, 0x40, 0x14, 0x54 }, off16[4] = { 0x00, 0x40, 0x10, 0x50 };
    u8 o[4 * MAXC];
    int n = 0;
    if (lcd_cmd((u8)(0x80 | (cols == 20 ? off20[r] : off16[r])))) return -1;
    for (int c = 0; c < cols; c++) {
        char ch = txt[r][c];
        if (ch < 32 || ch > 126) ch = ch ? '?' : ' ';
        n += lcd_bytes(o + n, (u8)ch, L_RS);
    }
    return dwr((u8)daddr, o, n);
}

/* ---------------- common ---------------- */

/* tiny string builders (no sprintf in the kernel) */
static int ap(char *b, int n, const char *s) { while (*s) b[n++] = *s++; b[n] = 0; return n; }
static int apn(char *b, int n, u32 v) { char t[12]; int k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k) b[n++] = t[--k]; b[n] = 0; return n; }
static int aphex(char *b, int n, u32 v) { b[n++] = '0'; b[n++] = 'x'; b[n++] = "0123456789abcdef"[(v >> 4) & 15]; b[n++] = "0123456789abcdef"[v & 15]; b[n] = 0; return n; }

static void geometry(void)
{
    if (dtype == D_LCD) {
        cols = dw = cfg_w ? cfg_w : 16; rows = dh = cfg_h ? cfg_h : 2;
        if (cols > MAXC) cols = dw = MAXC;
        if (rows > MAXR) rows = dh = MAXR;
    } else {
        dw = 128; dh = cfg_h == 32 ? 32 : 64;
        cols = 16; rows = dh / 16;
    }
}

static int flush(void)
{
    if (!ok) return -1;
    for (int r = 0; r < rows; r++) {
        if (!memcmp_n(txt[r], sent[r], MAXC + 1)) continue;
        int e = dtype == D_LCD ? lcd_row(r) : oled_row(r);
        if (e) { ok = 0; return e; }             /* gone from the bus: 'display scan' finds it again */
        memcpy(sent[r], txt[r], MAXC + 1);
    }
    return 0;
}

static void set_row(int r, const char *s)
{
    int i = 0;
    for (; i < cols && s[i] && s[i] != '\n'; i++) txt[r][i] = s[i];
    for (; i < MAXC; i++) txt[r][i] = ' ';
    txt[r][MAXC] = 0;
}

static int start(void)
{
    geometry();
    memset(txt, ' ', sizeof(txt));
    for (int r = 0; r < MAXR; r++) txt[r][MAXC] = 0;
    memset(sent, 0, sizeof(sent));               /* everything differs: the whole screen goes out */
    ok = 0;
    if (dtype == D_NONE) return -1;
    int e = dtype == D_LCD ? lcd_init() : oled_init();
    if (e) return e;
    ok = 1;
    return flush();
}

/* find a display: the /DISPLAY.TXT type + address, else 0x3C/0x3D = OLED, 0x27/0x3F = LCD backpack */
static void detect(void)
{
    dtype = D_NONE; daddr = 0; st_byte = 0; noack = 0;
    i2c_init();
    if (cfg_type == D_NONE) return;
    if (cfg_type > 0) {
        int a = cfg_addr ? cfg_addr : cfg_type == D_LCD ? 0x27 : 0x3C;
        if (cfg_type == D_LCD && !cfg_addr && !i2c_probe(0x27) && i2c_probe(0x3F)) a = 0x3F;
        if (cfg_type != D_LCD && !cfg_addr && !i2c_probe(0x3C) && i2c_probe(0x3D)) a = 0x3D;
        if (i2c_probe((u8)a)) { dtype = cfg_type; daddr = a; }
        else if (cfg_type != D_LCD) { dtype = cfg_type; daddr = a; noack = 1; }   /* an OLED that never ACKs */
        return;
    }
    static const u8 oled[] = { 0x3C, 0x3D }, lcd[] = { 0x27, 0x3F };
    for (int i = 0; i < 2 && !dtype; i++) {
        u8 s;
        if (i2c_read(oled[i], &s, 1)) continue;
        /* ASSUMED: the SH1106 status byte reads 0x08 (bits 3:0) when the display is off, the SSD1306
         * gives 0x03 / 0x06 / 0x07 - the usual trick of the Arduino auto-detect sketches. If the picture is
         * shifted by 2 pixels or the right edge is noise, 'display type sh1106' (or ssd1306) fixes it. */
        st_byte = s;
        dtype = (s & 0x0F) == 0x08 ? D_SH1106 : D_SSD1306;
        daddr = oled[i];
    }
    for (int i = 0; i < 2 && !dtype; i++)
        if (cfg_addr ? i2c_probe((u8)cfg_addr) : i2c_probe(lcd[i])) { dtype = D_LCD; daddr = cfg_addr ? cfg_addr : lcd[i]; }
}

static void load_cfg(void)
{
    struct fat_file f;
    char b[200];
    if (!fat_mounted() || fat_open("/DISPLAY.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    char *p = b;
    while (*p) {
        char *k = p, *v;
        while (*p && *p != ' ' && *p != '=' && *p != '\n' && *p != '\r') p++;
        int kl = (int)(p - k);
        while (*p == ' ' || *p == '=') p++;
        v = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        char vv[24]; int vl = (int)(p - v); if (vl > 23) vl = 23;
        memcpy(vv, v, (size_t)vl); vv[vl] = 0;
        while (vl && vv[vl - 1] == ' ') vv[--vl] = 0;
        while (*p == '\n' || *p == '\r') p++;
        u64 x;
        if (kl == 4 && !memcmp_n(k, "type", 4)) {
            for (int t = 0; t < 4; t++) if (!strcmp(vv, tname[t])) cfg_type = t;
            if (!strcmp(vv, "auto")) cfg_type = -1;
            if (!strcmp(vv, "oled")) cfg_type = D_SSD1306;
        } else if (kl == 4 && !memcmp_n(k, "addr", 4) && parse_num(vv, &x)) cfg_addr = (int)x;
        else if (kl == 4 && !memcmp_n(k, "size", 4)) {
            u64 w = 0, h = 0; char *q = vv;
            while (*q >= '0' && *q <= '9') w = w * 10 + (u64)(*q++ - '0');
            if (*q == 'x' || *q == 'X') q++;
            while (*q >= '0' && *q <= '9') h = h * 10 + (u64)(*q++ - '0');
            if (w && h) { cfg_w = (int)w; cfg_h = (int)h; }
        } else if (kl == 6 && !memcmp_n(k, "status", 6)) status_on = !strcmp(vv, "on");
        else if (kl == 8 && !memcmp_n(k, "contrast", 8) && parse_num(vv, &x) && x < 256) contrast = (int)x;
    }
}

static void save_cfg(void)
{
    char b[200];
    int n = ap(b, 0, "type ");
    n = ap(b, n, cfg_type < 0 ? "auto" : tname[cfg_type]);
    n = ap(b, n, "\n");
    if (cfg_addr) { n = ap(b, n, "addr "); n = aphex(b, n, (u32)cfg_addr); n = ap(b, n, "\n"); }
    if (cfg_w) { n = ap(b, n, "size "); n = apn(b, n, (u32)cfg_w); n = ap(b, n, "x"); n = apn(b, n, (u32)cfg_h); n = ap(b, n, "\n"); }
    n = ap(b, n, "status "); n = ap(b, n, status_on ? "on" : "off");
    n = ap(b, n, "\ncontrast "); n = apn(b, n, (u32)contrast); n = ap(b, n, "\n");
    if (fat_mounted() && fat_write_file("/DISPLAY.TXT", b, (u32)n)) kprintf("could not write /DISPLAY.TXT\n");
}

/* the status screen: name, IP, time, link (2-row displays: IP + time) */
static void status_draw(void)
{
    char l[4][48], t[24];
    u64 now = clock_now();
    const char *ip = ip_started() ? ip_address_str() : 0;
    int have_ip = ip && strcmp(ip, "0.0.0.0");
    ap(l[0], 0, net_name());
    ap(l[1], 0, have_ip ? ip : wifi_joining() && !wifi_joined() ? "WiFi joining.." : "no network");
    if (now) {
        clock_format(t, now, 0);                 /* "YYYY-MM-DD HH:MM:SS" */
        if (cols >= 19) ap(l[2], 0, t);
        else {                                   /* "DD.MM. HH:MM:SS" */
            char d[8] = { t[8], t[9], '.', t[5], t[6], '.', ' ', 0 };
            ap(l[2], ap(l[2], 0, d), t + 11);
        }
    } else ap(l[2], ap(l[2], 0, "RoboOS "), os_version());
    if (link_up()) { if (link_wifi()) ap(l[3], ap(l[3], 0, "WiFi "), wifi_ssid()); else ap(l[3], 0, "Ethernet"); }
    else ap(l[3], ap(l[3], 0, "RoboOS "), os_version());
    if (rows == 2) { set_row(0, have_ip ? l[1] : l[0]); set_row(1, l[2]); }
    else for (int r = 0; r < 4; r++) set_row(r, l[r]);
}

/* ---------------- interface ---------------- */

void display_boot(void)
{
    load_cfg();
    detect();
    if (!dtype) return;
    if (start()) { kprintf("display: %s at 0x%02x does not answer\n", tname[dtype], (u32)daddr); return; }
    set_row(0, "RoboOS"); set_row(1, os_version());
    if (rows > 2) { set_row(2, board_name()); set_row(3, ""); }
    flush();
    next_status = timer_us() + 2000000;          /* the boot text for 2 s, then the status screen */
}

int display_found(void) { return ok; }
int display_rows(void) { return ok ? rows : 0; }
int display_cols(void) { return ok ? cols : 0; }

/* core 0, from con_trygetc: the status screen once a second */
void display_poll(void)
{
    if (!ok || !status_on || busy || timer_us() < next_status) return;
    if (sound_is_exclusive()) return;            /* a WAV / video plays: no 25 ms pauses */
    busy = 1;
    next_status = timer_us() + 1000000;
    status_draw();
    flush();
    busy = 0;
}

/* text from BASIC / programs / the shell: row 0.., -1 = clear all; turns the status screen off */
int display_text(int row, const char *s)
{
    if (!ok) return -1;
    if (busy) return -2;
    busy = 1;
    status_on = 0;
    if (row < 0) for (int r = 0; r < rows; r++) set_row(r, "");
    else if (row < rows) set_row(row, s);
    int e = flush();
    busy = 0;
    return e;
}

void display_cmd(int argc, char **argv)
{
    const char *a = argc > 1 ? argv[1] : "";
    u64 x;
    if (!strcmp(a, "scan") || !strcmp(a, "init")) { busy = 1; detect(); if (dtype) start(); busy = 0; }
    else if (!strcmp(a, "clear")) { display_text(-1, ""); return; }
    else if (!strcmp(a, "text") && argc > 2 && parse_num(argv[2], &x)) {
        char b[64]; int n = 0;
        for (int i = 3; i < argc; i++)
            for (const char *s = argv[i]; *s && n < 62; s++) { b[n++] = *s; if (!s[1] && i + 1 < argc) b[n++] = ' '; }
        b[n] = 0;
        int e = display_text((int)x - 1, b);
        if (e) kprintf("no display (try 'display scan')\n");
        return;
    }
    else if (!strcmp(a, "status") && argc > 2) {
        status_on = !strcmp(argv[2], "on"); next_status = 0;
        if (!status_on) display_text(-1, "");
        save_cfg(); return;
    }
    else if (!strcmp(a, "type") && argc > 2) {
        int t = -2;
        for (int i = 0; i < 4; i++) if (!strcmp(argv[2], tname[i])) t = i;
        if (!strcmp(argv[2], "auto")) t = -1;
        if (!strcmp(argv[2], "oled")) t = D_SSD1306;
        if (t == -2) { kprintf("types: auto lcd ssd1306 sh1106 none\n"); return; }
        cfg_type = t; save_cfg();
        busy = 1; detect(); if (dtype) start(); busy = 0;
    }
    else if (!strcmp(a, "addr") && argc > 2 && parse_num(argv[2], &x) && x < 0x78) {
        cfg_addr = (int)x; save_cfg();
        busy = 1; detect(); if (dtype) start(); busy = 0;
    }
    else if (!strcmp(a, "size") && argc > 2) {
        u64 w = 0, h = 0; const char *q = argv[2];
        while (*q >= '0' && *q <= '9') w = w * 10 + (u64)(*q++ - '0');
        if (*q == 'x' || *q == 'X') q++;
        while (*q >= '0' && *q <= '9') h = h * 10 + (u64)(*q++ - '0');
        if (!w || !h) { kprintf("size: 16x2, 20x4, 16x4 (LCD) or 128x64, 128x32 (OLED)\n"); return; }
        cfg_w = (int)w; cfg_h = (int)h; save_cfg();
        busy = 1; if (dtype) start(); busy = 0;
    }
    else if (!strcmp(a, "contrast") && argc > 2 && parse_num(argv[2], &x) && x < 256) {
        contrast = (int)x; save_cfg();
        if (ok && dtype != D_LCD) { u8 c[2] = { 0x81, (u8)x }; oled_cmds(c, 2); }
    }
    else if (!strcmp(a, "light") && argc > 2) {
        backlight = !strcmp(argv[2], "on");
        if (ok && dtype == D_LCD) { u8 o = backlight ? L_BL : 0; dwr((u8)daddr, &o, 1); }
    }
    else if (!strcmp(a, "on") || !strcmp(a, "off")) {
        int on = !strcmp(a, "on");
        if (ok && dtype == D_LCD) lcd_cmd(on ? 0x0C : 0x08);
        else if (ok) { u8 c = on ? 0xAF : 0xAE; oled_cmds(&c, 1); }
    }
    else if (a[0]) {
        kprintf("usage: display [scan | clear | text <row> <words> | status on|off | on | off\n"
                "               | type auto|lcd|ssd1306|sh1106|none | addr <0x..> | size <WxH>\n"
                "               | contrast <0-255> | light on|off]\n");
        return;
    }
    if (!dtype) {
        kprintf("no display found (OLED at 0x3c/0x3d, LCD backpack at 0x27/0x3f)%s\n",
                cfg_type == D_NONE ? " - type is 'none'" : "");
        if (cfg_type < 0) kprintf("  an OLED that does not answer (no ACK, e.g. 2.42\" boards with D2 open):\n"
                                  "  'display type ssd1306' (or sh1106) uses it anyway\n");
        return;
    }
    kprintf("display: %s at 0x%02x, ", tname[dtype], (u32)daddr);
    if (dtype == D_LCD) kprintf("%dx%d characters", cols, rows);
    else kprintf("%dx%d pixels = %dx%d characters, status byte 0x%02x", dw, dh, cols, rows, (u32)st_byte);
    kprintf(", %s, status screen %s\n", noack ? "no ACK: written blind (bit-banged)" : ok ? "working" : "NOT answering",
            status_on ? "on" : "off");
    if (cfg_type < 0) kprintf("  (found by itself; the size is a guess - 'display size' if rows are missing)\n");
}
