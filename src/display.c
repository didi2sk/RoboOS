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
 * 0.58: fonts on the OLED: small 5x7 (21 x 8), normal 8x16 (16 x 4), big 16x32 (8 x 2); 0.59: tiny 4x6
 * (32 x 10) - text rows are no longer page-aligned, so the OLED is drawn into fb[8][128] and only the changed
 * pages go out; modes: status,
 * text, console (a copy of the terminal: con_putc -> display_con_putc, the screen follows every 0.1 s);
 * text over the network (JSON API /api/display, MQTT set/display) is stored by core 1 and drawn by core 0.
 */
#ifndef DISPLAY_HOST_TEST
#include "kernel.h"
#include "sd.h"
#endif
extern const unsigned char font8x16[95][16];
extern const unsigned char font5x7[95][5];
extern const unsigned char font4x6[95][4];
extern const unsigned char font5x8[95][5];
extern const int clk_big_h, clk_sec_h;                  /* fontclock.c: thin DejaVu Sans digits "0123456789:-" */
extern const unsigned char clk_big_w[12], clk_sec_w[12];
extern const unsigned int clk_big_g[12][30], clk_sec_g[12][15];

enum { D_NONE, D_SSD1306, D_SH1106, D_LCD };
static const char *const tname[] = { "none", "ssd1306", "sh1106", "lcd" };

#define MAXR 10
#define MAXC 32
enum { M_TEXT, M_STATUS, M_CONSOLE, M_CLOCK, M_RADIO, M_SENSORS, M_MOTION, M_THERMAL, M_ALARM };
static const char *const mname[] = { "text", "status", "console", "clock", "radio", "sensors", "motion", "thermal", "alarm" };
static const char *const fname[] = { "small", "normal", "big", "tiny", "micro" };
#define NFONTS 5
static int dtype, daddr, dw, dh, rows, cols;    /* dw x dh: pixels (OLED) or characters (LCD) */
static int cfg_type = -1, cfg_addr, cfg_w, cfg_h, contrast = 0xCF, backlight = 1;
static int mode = M_STATUS, font = 1, cw = 8, chh = 16;   /* OLED font: 0 small 6x8, 1 normal 8x16, 2 big 16x32, 3 tiny 4x6 */
static u8 fb[8][128], fbsent[8][128];          /* the OLED picture, and what the display shows */
#define status_on (mode == M_STATUS)
static volatile int dirty;                     /* console mode: txt changed since the last flush */
static int cur_r, cur_c, esc;                  /* console mode: the cursor, inside an escape sequence */
static u8 st_byte;                              /* OLED status byte read at detection (SSD1306 vs SH1106) */
static char txt[MAXR][MAXC + 1], sent[MAXR][MAXC + 1];
static int ok, busy, noack;                    /* noack: the display never ACKs - bit-banged writes */
static int spi_bus, cfg_spi;                   /* 0.72: SH1106 on SPI0 = Waveshare 1.3" OLED HAT (DC GPIO24, RST GPIO25) */
#define HAT_DC  24
#define HAT_RST 25
/* every OLED write starts with the I2C control byte (0x00 commands / 0x40 data): on SPI it becomes the DC pin */
static int dwr(u8 a, const void *b, int n)
{
    if (spi_bus) {
        const u8 *p = b;
        gpio_set(HAT_DC, p[0] == 0x40);
        return spi_write(p + 1, n - 1);
    }
    return noack ? i2c_write_noack(a, b, n) : i2c_write(a, b, n);
}
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

/* is pixel (x, y) of character ch's cell lit? (font-dependent cell size cw x chh) */
static int glyph_px(unsigned char ch, int x, int y)
{
    if (ch < 32 || ch > 126) ch = ch ? '?' : ' ';
    switch (font) {
    case 4: return x < 4 && y < 6 && (font4x6[ch - 32][x] >> y & 1);          /* micro 4x6 */
    case 3: return x < 5 && y < 8 && (font5x8[ch - 32][x] >> y & 1);          /* tiny 5x8 (0.62) */
    case 0: return x < 5 && y < 8 && (font5x7[ch - 32][x] >> y & 1);          /* 5x7 in a 6x8 cell */
    case 1: return (font8x16[ch - 32][y] & (0x80 >> x)) != 0;                 /* rows, bit 7 = left */
    default: return (font8x16[ch - 32][y / 2] & (0x80 >> (x / 2))) != 0;      /* the 8x16 doubled */
    }
}

static int oled_send(void);

/* draw txt into fb, then send the pages that differ from what the display shows */
static int oled_flush(void)
{
    memset(fb, 0, sizeof(fb));
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            unsigned char ch = (unsigned char)txt[r][c];
            if (ch == ' ') continue;
            for (int y = 0; y < chh; y++) {
                int py = r * chh + y;
                if (py >= dh) break;
                for (int x = 0; x < cw; x++)
                    if (glyph_px(ch, x, y)) fb[py >> 3][c * cw + x] |= (u8)(1 << (py & 7));
            }
        }
    return oled_send();
}

/* send the pages of fb that differ from what the display shows */
static int oled_send(void)
{
    u8 b[1 + 128];
    for (int page = 0; page < dh / 8; page++) {
        if (!memcmp_n(fb[page], fbsent[page], 128)) continue;
        b[0] = 0x40;                             /* control byte: data follow */
        memcpy(b + 1, fb[page], 128);
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
        memcpy(fbsent[page], fb[page], 128);
    }
    return 0;
}

static int ap(char *b, int n, const char *s);
static int flush(void);
static void set_row(int r, const char *s);

/* ---- the clock (0.60): big digits drawn straight into fb ---- */
static void px_on(int x, int y) { if (x >= 0 && x < 128 && y >= 0 && y < dh) fb[y >> 3][x] |= (u8)(1 << (y & 7)); }

static void draw_big(const char *s, int x, int y, int k)        /* the 8x16 font, k times bigger */
{
    for (; *s; s++, x += 8 * k) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const unsigned char *g = font8x16[ch - 32];
        for (int gy = 0; gy < 16; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (g[gy] & (0x80 >> gx))
                    for (int dy = 0; dy < k; dy++) for (int dx = 0; dx < k; dx++) px_on(x + gx * k + dx, y + gy * k + dy);
    }
}

static void draw_small(const char *s, int x, int y)             /* the 5x7 font, 6 px a character */
{
    for (; *s; s++, x += 6) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        for (int gx = 0; gx < 5; gx++)
            for (int gy = 0; gy < 8; gy++) if (font5x7[ch - 32][gx] >> gy & 1) px_on(x + gx, y + gy);
    }
}

/* clock digits (0.61): every digit in a cell as wide as the widest one (no jumping), the colon narrow */
static int clk_idx(char c) { return c >= '0' && c <= '9' ? c - '0' : c == ':' ? 10 : 11; }
static int clk_cell(const unsigned char *w) { int m = 0; for (int i = 0; i < 10; i++) if (w[i] > m) m = w[i]; return m; }
static int clk_width(const char *s, const unsigned char *w, int gap)
{
    int n = 0, cell = clk_cell(w);
    for (; *s; s++) n += (*s == ':' ? w[10] : cell) + (s[1] ? gap : 0);
    return n;
}
static void clk_draw(const char *s, int x, int y, const unsigned char *w, const unsigned int *g, int h, int gap)
{
    int cell = clk_cell(w);
    for (; *s; s++) {
        int i = clk_idx(*s), cw2 = *s == ':' ? w[10] : cell, off = (cw2 - w[i]) / 2;
        for (int r = 0; r < h; r++) {
            unsigned int bits = g[i * h + r];
            for (int c = 0; c < w[i]; c++) if (bits & (0x80000000u >> c)) px_on(x + off + c, y + r);
        }
        x += cw2 + gap;
    }
}

/* "YYYY-MM-DD HH:MM:SS" -> weekday (Sakamoto) */
static const char *weekday(const char *t)
{
    static const char *const wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const int tt[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = (t[0] - '0') * 1000 + (t[1] - '0') * 100 + (t[2] - '0') * 10 + (t[3] - '0');
    int m = (t[5] - '0') * 10 + (t[6] - '0'), d = (t[8] - '0') * 10 + (t[9] - '0');
    if (m < 1 || m > 12) return "";
    if (m < 3) y--;
    return wd[(y + y / 4 - y / 100 + y / 400 + tt[m - 1] + d) % 7];
}

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* the clock screen: OLED 128x64 = HH:MM 3x (48 px) + "Sat 21.11.2026  :56" below,
 * 128x32 = HH:MM:SS 2x (fills it); LCD = time on row 1, weekday + date on row 2 */
static void clock_draw(void)
{
    char t[24], hm[9], date[24];
    u64 now = clock_now();
    if (now) {
        clock_format(t, now, 0);                 /* "YYYY-MM-DD HH:MM:SS" */
        int n = ap(date, 0, weekday(t));
        char d[12] = { ' ', t[8], t[9], '.', t[5], t[6], '.', t[0], t[1], t[2], t[3], 0 };
        ap(date, n, d);
    } else { ap(t, 0, "0000-00-00 --:--:--"); ap(date, 0, "no time yet (NTP)"); }
    if (dtype == D_LCD) {
        for (int r = 0; r < rows; r++) set_row(r, "");
        char l[MAXC + 1]; int pad = (cols - 8) / 2, k = 0;
        while (k < pad) l[k++] = ' ';
        for (int i = 11; i < 19; i++) l[k++] = t[i];
        l[k] = 0;
        set_row(0, l);
        if (rows > 1) {
            int dl = slen(date); pad = cols > dl ? (cols - dl) / 2 : 0; k = 0;
            while (k < pad) l[k++] = ' ';
            for (int i = 0; date[i] && k < MAXC; i++) l[k++] = date[i];
            l[k] = 0;
            set_row(1, l);
        }
        flush();
        return;
    }
    memset(fb, 0, sizeof(fb));
    /* HH:MM in the thin big digits, the seconds smaller beside them (bottom-aligned), the date below */
    for (int i = 0; i < 5; i++) hm[i] = now ? t[11 + i] : (i == 2 ? ':' : '-');
    hm[5] = 0;
    char sec[3] = { now ? t[17] : '-', now ? t[18] : '-', 0 };
    int bw = clk_width(hm, clk_big_w, 3), sw = clk_width(sec, clk_sec_w, 1), gap = 4;
    int x = (128 - bw - gap - sw) / 2;
    if (x < 0) x = 0;
    int y = dh >= 64 ? 3 : (dh - clk_big_h) / 2;
    clk_draw(hm, x, y, clk_big_w, &clk_big_g[0][0], clk_big_h, 3);
    clk_draw(sec, x + bw + gap, y + clk_big_h - clk_sec_h, clk_sec_w, &clk_sec_g[0][0], clk_sec_h, 1);
    if (dh >= 64) {                              /* "Sat 21.11.2026" in the 8x16 font (14 x 8 = 112 px) */
        int dl = slen(date);
        if (dl <= 16) draw_big(date, (128 - dl * 8) / 2, 46, 1);
        else draw_small(date, (128 - dl * 6) / 2 > 0 ? (128 - dl * 6) / 2 : 0, 50);
    }
    for (int r = 0; r < MAXR; r++) memset(sent[r], 0, MAXC + 1);   /* text mode redraws fully later */
    if (oled_send()) ok = 0;
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
        static const int fw[] = { 6, 8, 16, 5, 4 }, fh[] = { 8, 16, 32, 8, 6 };
        cw = fw[font]; chh = fh[font];
        cols = 128 / cw; rows = dh / chh;
    }
    if (cur_r >= rows) cur_r = rows - 1;
    if (cur_c >= cols) cur_c = 0;
}

static int flush(void)
{
    if (!ok) return -1;
    int changed = 0;
    for (int r = 0; r < rows; r++) {
        if (!memcmp_n(txt[r], sent[r], MAXC + 1)) continue;
        if (dtype == D_LCD) { if (lcd_row(r)) { ok = 0; return -1; } }   /* gone: 'display scan' finds it again */
        memcpy(sent[r], txt[r], MAXC + 1);
        changed = 1;
    }
    if (dtype != D_LCD && changed && oled_flush()) { ok = 0; return -1; }
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
    memset(fbsent, 0xA5, sizeof(fbsent));
    ok = 0;
    if (dtype == D_NONE) return -1;
    if (spi_bus) {                                /* the HAT: DC + RST outputs, a reset pulse */
        spi_init(64);
        gpio_func(HAT_DC, 1); gpio_func(HAT_RST, 1);
        gpio_set(HAT_RST, 0); delay_us(10000); gpio_set(HAT_RST, 1); delay_us(10000);
    }
    int e = dtype == D_LCD ? lcd_init() : oled_init();
    if (e) return e;
    ok = 1;
    return flush();
}

/* find a display: the /DISPLAY.TXT type + address, else 0x3C/0x3D = OLED, 0x27/0x3F = LCD backpack */
static void detect(void)
{
    dtype = D_NONE; daddr = 0; st_byte = 0; noack = 0; spi_bus = 0;
    if (cfg_spi) { dtype = cfg_type > 0 && cfg_type != D_LCD ? cfg_type : D_SH1106; spi_bus = 1; return; }   /* SPI: nothing to ask */
    i2c_init();
    if (cfg_type == D_NONE) return;
    if (cfg_type > 0) {
        int a = cfg_addr ? cfg_addr : cfg_type == D_LCD ? 0x27 : 0x3C;
        if (cfg_type == D_LCD && !cfg_addr && !i2c_probe(0x27) && i2c_probe(0x3F)) a = 0x3F;
        if (cfg_type != D_LCD && !cfg_addr && !i2c_probe(0x3C) && !i2c_probe_write(0x3C) && (i2c_probe(0x3D) || i2c_probe_write(0x3D))) a = 0x3D;
        if (i2c_probe((u8)a) || (cfg_type != D_LCD && i2c_probe_write((u8)a))) { dtype = cfg_type; daddr = a; }
        else if (cfg_type != D_LCD) { dtype = cfg_type; daddr = a; noack = 1; }   /* an OLED that never ACKs */
        return;
    }
    static const u8 oled[] = { 0x3C, 0x3D }, lcd[] = { 0x27, 0x3F };
    for (int i = 0; i < 2 && !dtype; i++) {
        u8 s;
        if (i2c_read(oled[i], &s, 1)) {
            /* no answer to a read: an SSD1309 (the 2.42" boards) is write-only on I2C - try a write */
            if (!i2c_probe_write(oled[i])) continue;
            st_byte = 0xFF; dtype = D_SSD1306; daddr = oled[i];   /* (0xFF in 'display' = no status byte) */
            continue;
        }
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
        } else if (kl == 6 && !memcmp_n(k, "status", 6)) mode = !strcmp(vv, "on") ? M_STATUS : M_TEXT;   /* (0.55 files) */
        else if (kl == 3 && !memcmp_n(k, "bus", 3)) cfg_spi = !strcmp(vv, "spi");
        else if (kl == 4 && !memcmp_n(k, "mode", 4)) { for (int i = 0; i < 9; i++) if (!strcmp(vv, mname[i])) mode = i; }
        else if (kl == 4 && !memcmp_n(k, "font", 4)) { for (int i = 0; i < NFONTS; i++) if (!strcmp(vv, fname[i])) font = i; }
        else if (kl == 8 && !memcmp_n(k, "contrast", 8) && parse_num(vv, &x) && x < 256) contrast = (int)x;
    }
}

static void save_cfg(void)
{
    char b[200];
    int n = ap(b, 0, "type ");
    n = ap(b, n, cfg_type < 0 ? "auto" : tname[cfg_type]);
    n = ap(b, n, "\n");
    if (cfg_spi) n = ap(b, n, "bus spi\n");
    if (cfg_addr) { n = ap(b, n, "addr "); n = aphex(b, n, (u32)cfg_addr); n = ap(b, n, "\n"); }
    if (cfg_w) { n = ap(b, n, "size "); n = apn(b, n, (u32)cfg_w); n = ap(b, n, "x"); n = apn(b, n, (u32)cfg_h); n = ap(b, n, "\n"); }
    n = ap(b, n, "mode "); n = ap(b, n, mname[mode]);
    n = ap(b, n, "\nfont "); n = ap(b, n, fname[font]);
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
    for (int r = 0; r < rows; r++) set_row(r, "");
    if (rows == 1) set_row(0, have_ip ? l[1] : l[2]);
    else if (rows == 2) { set_row(0, have_ip ? l[1] : l[0]); set_row(1, l[2]); }
    else {
        int r0 = rows >= 8 ? 1 : 0;              /* small font: a title line and a gap */
        if (r0) { char tt[32]; ap(tt, ap(tt, 0, "RoboOS "), os_version()); set_row(0, tt); }
        for (int r = 0; r < 4; r++) set_row(r0 + r + (r0 && r >= 2), l[r]);
        if (rows >= 8) {
            char up[32]; u64 s2 = timer_us() / 1000000;
            int n = ap(up, 0, "up ");
            n = apn(up, n, (u32)(s2 / 3600)); n = ap(up, n, "h ");
            n = apn(up, n, (u32)(s2 / 60 % 60)); ap(up, n, "m");
            set_row(7, up);
        }
    }
}

/* the FM radio screen (0.71): frequency + stereo + signal, station name (RDS PS), radio text */
static void radio_draw(void)
{
    char l[4][64], fs[12];
    u32 f = radio_freq();
    int n = 0;
    { u32 m = f / 100; char t[6]; int k = 0; do t[k++] = (char)('0' + m % 10); while ((m /= 10)); while (k) fs[n++] = t[--k]; }
    fs[n++] = '.'; fs[n++] = (char)('0' + f / 10 % 10); fs[n] = 0;
    for (int r = 0; r < 4; r++) l[r][0] = 0;
    char ps[9]; int pn = 0;
    for (int i = 0; i < 8; i++) { char c = radio_ps()[i]; if (c) ps[pn++] = c; }
    while (pn && ps[pn - 1] == ' ') pn--;
    ps[pn] = 0;
    const char *rt = radio_rt();
    if (!radio_on()) { ap(l[0], ap(l[0], 0, "radio off "), fs); }
    else {
        int k = ap(l[0], 0, fs);
        k = ap(l[0], k, " MHz");
        if (radio_stereo()) k = ap(l[0], k, " ST");
        if (cols >= 20) { k = ap(l[0], k, " "); k = apn(l[0], k, (u32)radio_rssi()); }
        ap(l[1], 0, ps[0] ? ps : "");
    }
    for (int r = 0; r < rows; r++) set_row(r, "");
    if (rows <= 2) {                                 /* "104.4 NAME" + the text, scrolling */
        char t[64]; int k = ap(t, 0, fs);
        if (ps[0]) { t[k++] = ' '; k = ap(t, k, ps); }
        set_row(0, t);
        if (rows > 1) {
            int len = 0; while (len < 64 && rt[len]) len++;
            int off = len > cols ? (int)(timer_us() / 1000000) % (len - cols + 1) : 0;
            set_row(1, rt + off);
        }
        return;
    }
    set_row(0, l[0]);
    set_row(1, l[1]);
    for (int r = 2, at = 0; r < rows && rt[at]; r++) {   /* the radio text, wrapped */
        set_row(r, rt + at);
        int k = 0; while (k < cols && rt[at + k]) k++;
        at += k;
    }
}

/* the sensor screen (0.82): temperature, humidity, pressure (+ the time on 4+ rows) */
static int c100(char *b, int n, int v, int dec)          /* v in 0.01 units, dec 1 or 2 decimals */
{
    if (v < 0) { b[n++] = '-'; v = -v; }
    n = apn(b, n, (u32)(v / 100));
    b[n++] = '.';
    b[n++] = (char)('0' + v / 10 % 10);
    if (dec > 1) b[n++] = (char)('0' + v % 10);
    b[n] = 0;
    return n;
}
static void sensors_draw(void)
{
    struct sensor_values v;
    sensors_read(&v);
    char l[4][40]; for (int i = 0; i < 4; i++) l[i][0] = 0;
    int k;
    for (int r = 0; r < rows; r++) set_row(r, "");
    if (!(v.have & (SV_TEMP | SV_HUM | SV_PRESS))) { set_row(0, "no sensors"); if (rows > 1) set_row(1, "(i2c scan)"); return; }
    if (rows <= 2) {                                 /* "21.4C 45%" + "1001.3 hPa" */
        k = 0;
        if (v.have & SV_TEMP) { k = c100(l[0], k, v.temp_c100, 1); k = ap(l[0], k, "C "); }
        if (v.have & SV_HUM) { k = c100(l[0], k, v.hum_c100, 1); k = ap(l[0], k, "%"); }
        if (v.have & SV_PRESS) { k = c100(l[1], 0, v.press_pa, 1); ap(l[1], k, " hPa"); }
        set_row(0, l[0]); if (rows > 1) set_row(1, l[1]);
        return;
    }
    int r = 0;
    if (v.have & SV_TEMP) { k = ap(l[r], 0, cols >= 14 ? "Temp " : "T "); k = c100(l[r], k, v.temp_c100, 2); ap(l[r], k, " C"); r++; }
    if (v.have & SV_HUM) { k = ap(l[r], 0, cols >= 14 ? "Hum  " : "H "); k = c100(l[r], k, v.hum_c100, 1); ap(l[r], k, " %"); r++; }
    if (v.have & SV_PRESS) { k = ap(l[r], 0, cols >= 14 ? "Pres " : "P "); k = c100(l[r], k, v.press_pa, 1); ap(l[r], k, cols >= 14 ? " hPa" : "hPa"); r++; }
    for (int i = 0; i < r && i < rows; i++) set_row(i, l[i]);
    if (rows > r) {                                  /* the time on a free row */
        char t[32]; u64 now = clock_now();
        if (now) { clock_format(t, now, 0); set_row(rows - 1, t + 11); }
    }
}

/* 0.121: the alarm screen - state, seconds left / cause, the code typed on the keypad as stars, the time */
static void alarm_draw(void)
{
    char l[4][40];
    int n = alarm_display(l, cols > 39 ? 39 : cols);
    for (int r = 0; r < rows; r++) set_row(r, r < n && (rows > 2 || r < 2) ? l[r] : "");
}
void display_poke(void) { if (mode == M_ALARM || (mode == M_STATUS && alarm_is_on())) next_status = 0; }

/* the tilt screen (0.83, MPU-6050): pitch / roll / |a| / shakes */
static int d10(char *b, int n, int v)
{
    if (v < 0) { b[n++] = '-'; v = -v; }
    n = apn(b, n, (u32)(v / 10)); b[n++] = '.'; b[n++] = (char)('0' + v % 10); b[n] = 0;
    return n;
}
static void motion_draw(void)
{
    struct motion_values m;
    for (int r = 0; r < rows; r++) set_row(r, "");
    if (motion_get(&m)) { set_row(0, "no MPU-6050"); return; }
    char l[4][40]; int k;
    k = ap(l[0], 0, "Pitch "); k = d10(l[0], k, m.pitch_d10); ap(l[0], k, " deg");
    k = ap(l[1], 0, "Roll  "); k = d10(l[1], k, m.roll_d10); ap(l[1], k, " deg");
    k = ap(l[2], 0, "|a| "); k = apn(l[2], k, (u32)m.g_mg); ap(l[2], k, " mg");
    k = ap(l[3], 0, "Shakes "); apn(l[3], k, m.shakes);
    if (rows <= 2) {
        char t[40]; k = ap(t, 0, "P "); k = d10(t, k, m.pitch_d10); k = ap(t, k, " R "); d10(t, k, m.roll_d10);
        set_row(0, t);
        if (rows > 1) set_row(1, l[3]);
        return;
    }
    for (int i = 0; i < 4 && i < rows; i++) set_row(i, l[i]);
}

/* the thermal camera screen (0.87, AMG8833): hottest / coldest / person */
static void thermal_draw(void)
{
    struct thermal_values t;
    for (int r = 0; r < rows; r++) set_row(r, "");
    if (thermal_get(&t)) { set_row(0, "no AMG8833"); return; }
    char l[4][40]; int k;
    k = ap(l[0], 0, "IR max "); k = c100(l[0], k, t.max_c100, 1); ap(l[0], k, " C");
    k = ap(l[1], 0, "IR min "); k = c100(l[1], k, t.min_c100, 1); ap(l[1], k, " C");
    k = ap(l[2], 0, "Avg    "); k = c100(l[2], k, t.avg_c100, 1); ap(l[2], k, " C");
    ap(l[3], 0, t.warm >= 2 ? "Person: YES" : "Person: no");
    if (rows <= 2) {
        char a[40]; k = ap(a, 0, "max "); k = c100(a, k, t.max_c100, 1); ap(a, k, " C");
        set_row(0, a); if (rows > 1) set_row(1, l[3]);
        return;
    }
    for (int i = 0; i < 4 && i < rows; i++) set_row(i, l[i]);
}

/* ---- the HAT's keys (0.72): KEY1 21, KEY2 20, KEY3 16, joystick up 6, down 19, left 5, right 26, press 13
 * (active low, pull-ups). KEY1 = next screen (info, time, radio, console); on the radio screen KEY2/KEY3 seek
 * down/up, left/right = -/+ 0.1 MHz, up/down = volume, press = radio on/off. ---- */
static const u8 key_pin[8] = { 21, 20, 16, 6, 19, 5, 26, 13 };
static const char *const key_name[8] = { "KEY1", "KEY2", "KEY3", "up", "down", "left", "right", "press" };
static u8 key_raw, key_stable, keys_set;
static u64 key_next;
static int in_keys;

static u8 keys_read(void)
{
    if (!keys_set) { for (int i = 0; i < 8; i++) { gpio_func(key_pin[i], 0); gpio_pull(key_pin[i], 2); } keys_set = 1; delay_us(100); }
    u8 v = 0;
    for (int i = 0; i < 8; i++) if (!gpio_get(key_pin[i])) v |= (u8)(1 << i);
    return v;
}

static void radio_do(const char *a, const char *b)
{
    char x[12], y[12], *av[3] = { "radio", x, y };
    int n = 0; for (; a[n] && n < 11; n++) x[n] = a[n]; x[n] = 0;
    if (b) { n = 0; for (; b[n] && n < 11; n++) y[n] = b[n]; y[n] = 0; }
    radio_cmd(b ? 3 : 2, av);
}

static void key_pressed(int k)
{
    static const int order[8] = { M_STATUS, M_CLOCK, M_SENSORS, M_MOTION, M_THERMAL, M_ALARM, M_RADIO, M_CONSOLE };
    if (k == 0) {                                 /* next screen */
        int i = 0; while (i < 8 && order[i] != mode) i++;
        for (int s = 1; s < 8; s++) {                 /* skip the screens of chips that are not there */
            mode = order[(i + s) % 8];
            if ((mode != M_MOTION || motion_present()) && (mode != M_THERMAL || thermal_present()) && (mode != M_ALARM || alarm_is_on())) break;
        }
        for (int r = 0; r < rows; r++) set_row(r, "");
        cur_r = cur_c = 0; next_status = 0; flush();
        return;
    }
    if (mode != M_RADIO || !radio_present()) return;
    char num[12];
    if (k == 1) radio_do("down", 0);
    else if (k == 2) radio_do("up", 0);
    else if (k == 7) radio_do(radio_on() ? "off" : "on", 0);
    else if (k == 3 || k == 4) { int v = radio_volume() + (k == 3 ? 1 : -1); if (v < 0) v = 0; if (v > 15) v = 15; apn(num, 0, (u32)v); radio_do("vol", num); }
    else if (k == 5 || k == 6) {
        u32 f = radio_freq() + (k == 6 ? 10 : (u32)-10);
        if (f < 8700) f = 10800; if (f > 10800) f = 8700;
        int n = apn(num, 0, f / 100); num[n++] = '.'; num[n++] = (char)('0' + f / 10 % 10); num[n] = 0;
        radio_do(num, 0);
    }
    next_status = 0;
}

static void keys_poll(void)
{
    if (!spi_bus || in_keys || timer_us() < key_next) return;
    key_next = timer_us() + 20000;
    int first = !keys_set;
    u8 v = keys_read();
    if (first) { key_raw = key_stable = v; return; }   /* keys held at start-up do not count */
    if (v != key_raw) { key_raw = v; return; }    /* changed: wait one more sample (debounce) */
    u8 down = (u8)(v & ~key_stable);
    key_stable = v;
    if (!down) return;
    in_keys = 1;
    for (int i = 0; i < 8; i++) if (down & (1 << i)) key_pressed(i);
    in_keys = 0;
}

/* ---------------- interface ---------------- */

void display_boot(void)
{
    load_cfg();
    detect();
    if (!dtype) return;
    if (start()) { kprintf("display: %s at 0x%02x does not answer\n", tname[dtype], (u32)daddr); return; }
    if (mode == M_CONSOLE) { cur_r = cur_c = 0; next_status = 0; return; }   /* the boot messages from here on */
    set_row(0, "RoboOS"); set_row(1, os_version());
    if (rows > 2) { set_row(2, board_name()); set_row(3, ""); }
    flush();
    next_status = timer_us() + 2000000;          /* the boot text for 2 s, then the status screen */
}

int display_found(void) { return ok; }
int display_rows(void) { return ok ? rows : 0; }
int display_cols(void) { return ok ? cols : 0; }

/* ---- text from the network (core 1: JSON API, MQTT) - stored here, drawn by core 0 ---- */
static volatile int nlock, npend;
static char ntext[MAXR][MAXC + 1];
static int nrow;                                 /* -1 = the whole screen (lines split at \n), else that row */

void display_net(int row, const char *t, int len)
{
    while (__atomic_test_and_set(&nlock, __ATOMIC_ACQUIRE)) ;
    nrow = row;
    memset(ntext, 0, sizeof(ntext));
    int r = 0, c = 0;
    for (int i = 0; i < len && r < MAXR; i++) {
        char ch = t[i];
        if (ch == '\n' && row < 0) { r++; c = 0; continue; }
        if (ch == '\r') continue;
        if (c < MAXC) ntext[r][c++] = ch;
    }
    npend = 1;
    __atomic_clear(&nlock, __ATOMIC_RELEASE);
}

/* the console copy: called for every character printed (any core, under the console lock) */
static void scroll(void)
{
    for (int r = 0; r + 1 < rows; r++) memcpy(txt[r], txt[r + 1], MAXC + 1);
    memset(txt[rows - 1], ' ', MAXC); txt[rows - 1][MAXC] = 0;
}

void display_con_putc(char c)
{
    if (!ok || mode != M_CONSOLE) return;
    if (esc) { if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) esc = 0; return; }
    if (c == 27) { esc = 1; return; }
    if (c == '\n') { cur_c = 0; if (++cur_r >= rows) { cur_r = rows - 1; scroll(); } }
    else if (c == '\r') cur_c = 0;
    else if (c == '\b') { if (cur_c) cur_c--; }
    else if (c == '\t') { do display_con_putc(' '); while (cur_c % 4); return; }
    else if ((u8)c >= 32 && (u8)c < 127) {
        if (cur_c >= cols) { cur_c = 0; if (++cur_r >= rows) { cur_r = rows - 1; scroll(); } }
        txt[cur_r][cur_c++] = c;
    } else return;
    dirty = 1;
}

/* core 0, from con_trygetc: the status screen once a second, the console copy, network text */
void display_poll(void)
{
    if (!ok || busy) return;
    if (spi_bus && !in_keys) { busy = 1; keys_poll(); busy = 0; }
    if (npend) {
        busy = 1;
        while (__atomic_test_and_set(&nlock, __ATOMIC_ACQUIRE)) ;
        int row = nrow;
        char t[MAXR][MAXC + 1];
        memcpy(t, ntext, sizeof(t));
        npend = 0;
        __atomic_clear(&nlock, __ATOMIC_RELEASE);
        mode = M_TEXT;
        if (row < 0) for (int r = 0; r < rows; r++) set_row(r, t[r]);
        else if (row < rows) set_row(row, t[0]);
        flush();
        busy = 0;
        return;
    }
    if (sound_is_exclusive()) return;            /* a WAV / video plays: no 25 ms pauses */
    if (mode == M_CONSOLE) {
        if (!dirty || timer_us() < next_status) return;
        busy = 1;
        dirty = 0;
        next_status = timer_us() + 100000;
        flush();
        busy = 0;
        return;
    }
    if ((mode != M_STATUS && mode != M_CLOCK && mode != M_RADIO && mode != M_SENSORS && mode != M_MOTION && mode != M_THERMAL && mode != M_ALARM) || timer_us() < next_status) return;
    busy = 1;
    next_status = timer_us() + (mode == M_SENSORS ? 2000000 : mode == M_MOTION ? 250000 : mode == M_THERMAL ? 500000 : mode == M_ALARM || (mode == M_STATUS && alarm_is_on()) ? 250000 : 1000000);
    if (mode == M_CLOCK) clock_draw();
    else if (mode == M_RADIO) { radio_draw(); flush(); }
    else if (mode == M_SENSORS) { sensors_draw(); flush(); }
    else if (mode == M_MOTION) { motion_draw(); flush(); }
    else if (mode == M_THERMAL) { thermal_draw(); flush(); }
    else if (mode == M_ALARM) { alarm_draw(); flush(); }
    else if (alarm_is_on()) { alarm_draw(); flush(); }   /* 0.133: the alarm is on -> the status screen shows the alarm */
    else { status_draw(); flush(); }
    busy = 0;
}

/* text from BASIC / programs / the shell: row 0.., -1 = clear all; turns the status screen off */
int display_text(int row, const char *s)
{
    if (!ok) return -1;
    if (busy) return -2;
    busy = 1;
    mode = M_TEXT;
    if (row < 0) for (int r = 0; r < rows; r++) set_row(r, "");
    else if (row < rows) set_row(row, s);
    int e = flush();
    busy = 0;
    return e;
}

/* for GET /api/display (core 1 reads; a torn row is harmless) */
const char *display_type_name(void) { return ok ? tname[dtype] : "none"; }
const char *display_mode_name(void) { return mname[mode]; }
const char *display_row_text(int r) { return r >= 0 && r < rows ? txt[r] : ""; }

static void restart(void) { busy = 1; if (dtype) start(); busy = 0; next_status = 0; cur_r = cur_c = 0; }

void display_cmd(int argc, char **argv)
{
    const char *a = argc > 1 ? argv[1] : "";
    u64 x;
    if (!strcmp(a, "scan") || !strcmp(a, "init")) { busy = 1; detect(); busy = 0; restart(); }
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
    else if (!strcmp(a, "time") || !strcmp(a, "clock") || !strcmp(a, "info") || !strcmp(a, "radio") || !strcmp(a, "sensors") || !strcmp(a, "motion") || !strcmp(a, "thermal") || !strcmp(a, "alarm")) {
        mode = a[0] == 'i' ? M_STATUS : a[0] == 'r' ? M_RADIO : a[0] == 's' ? M_SENSORS : a[0] == 'm' ? M_MOTION : !strcmp(a, "thermal") ? M_THERMAL : a[0] == 'a' ? M_ALARM : M_CLOCK;
        next_status = 0;
        if (ok) { busy = 1; for (int r = 0; r < rows; r++) set_row(r, ""); flush(); busy = 0; }
        save_cfg();
        if (!dtype) kprintf("no display found\n");
        return;
    }
    else if ((!strcmp(a, "status") || !strcmp(a, "console")) && argc > 2) {
        int on = !strcmp(argv[2], "on");
        mode = on ? (a[0] == 's' ? M_STATUS : M_CONSOLE) : a[0] == 'c' ? M_STATUS : M_TEXT;   /* console off -> info */
        next_status = 0;
        if (ok) {
            busy = 1;
            for (int r = 0; r < rows; r++) set_row(r, "");
            cur_r = cur_c = 0;
            flush();
            busy = 0;
        }
        save_cfg();
        if (mode == M_CONSOLE && (font == 1 || font == 2) && dtype && dtype != D_LCD) kprintf("(tip: 'display font tiny' shows 25 x 8 characters, 'small' 21 x 8)\n");
        return;
    }
    else if (!strcmp(a, "font") && argc > 2) {
        int f = -1;
        for (int i = 0; i < NFONTS; i++) if (!strcmp(argv[2], fname[i])) f = i;
        if (f < 0) { kprintf("fonts: micro (32 x 10), tiny (25 x 8), small (21 x 8), normal (16 x 4), big (8 x 2) - OLED only\n"); return; }
        font = f; save_cfg(); restart();
    }
    else if (!strcmp(a, "hat")) {                /* Waveshare 1.3" OLED HAT: SH1106 on SPI + keys */
        cfg_spi = 1; cfg_type = D_SH1106; cfg_h = 64; save_cfg();
        busy = 1; detect(); busy = 0; restart();
        kprintf("Waveshare 1.3\" OLED HAT: SH1106 on SPI0 (CE0, DC GPIO24, RST GPIO25), keys on GPIO21/20/16,\n"
                "joystick GPIO6/19/5/26/13. KEY1 = next screen; on 'display radio': KEY2/KEY3 seek, left/right tune,\n"
                "up/down volume, press on/off. 'display type auto' goes back to I2C displays.\n");
    }
    else if (!strcmp(a, "keys")) {
        u8 v = keys_read();
        kprintf("keys pressed:");
        int any = 0;
        for (int i = 0; i < 8; i++) if (v & (1 << i)) { kprintf(" %s", key_name[i]); any = 1; }
        kprintf("%s\n", any ? "" : " none");
        return;
    }
    else if (!strcmp(a, "type") && argc > 2) {
        cfg_spi = 0;
        int t = -2;
        for (int i = 0; i < 4; i++) if (!strcmp(argv[2], tname[i])) t = i;
        if (!strcmp(argv[2], "auto")) t = -1;
        if (!strcmp(argv[2], "oled")) t = D_SSD1306;
        if (t == -2) { kprintf("types: auto lcd ssd1306 sh1106 none\n"); return; }
        cfg_type = t; save_cfg();
        busy = 1; detect(); busy = 0; restart();
    }
    else if (!strcmp(a, "addr") && argc > 2 && parse_num(argv[2], &x) && x < 0x78) {
        cfg_addr = (int)x; save_cfg();
        busy = 1; detect(); busy = 0; restart();
    }
    else if (!strcmp(a, "size") && argc > 2) {
        u64 w = 0, h = 0; const char *q = argv[2];
        while (*q >= '0' && *q <= '9') w = w * 10 + (u64)(*q++ - '0');
        if (*q == 'x' || *q == 'X') q++;
        while (*q >= '0' && *q <= '9') h = h * 10 + (u64)(*q++ - '0');
        if (!w || !h) { kprintf("size: 16x2, 20x4, 16x4 (LCD) or 128x64, 128x32 (OLED)\n"); return; }
        cfg_w = (int)w; cfg_h = (int)h; save_cfg();
        restart();
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
        kprintf("usage: display [scan | clear | text <row> <words> | info | time | sensors | motion | thermal | alarm | radio | console on|off\n"
                "               | hat | keys | font micro|tiny|small|normal|big | on | off | type auto|lcd|ssd1306|sh1106|none\n"
                "               | addr <0x..> | size <WxH> | contrast <0-255> | light on|off]\n");
        return;
    }
    if (!dtype) {
        kprintf("no display found (OLED at 0x3c/0x3d, LCD backpack at 0x27/0x3f)%s\n",
                cfg_type == D_NONE ? " - type is 'none'" : "");
        if (cfg_type < 0) kprintf("  an OLED that does not answer (no ACK, e.g. 2.42\" boards with D2 open):\n"
                                  "  'display type ssd1306' (or sh1106) uses it anyway\n");
        return;
    }
    if (spi_bus) kprintf("display: %s on SPI (Waveshare 1.3\" OLED HAT), ", tname[dtype]);
    else kprintf("display: %s at 0x%02x, ", tname[dtype], (u32)daddr);
    if (dtype == D_LCD) kprintf("%dx%d characters", cols, rows);
    else kprintf("%dx%d pixels, font %s = %dx%d characters", dw, dh, fname[font], cols, rows);
    if (!spi_bus) kprintf(", status byte 0x%02x", (u32)st_byte);
    kprintf(", %s, mode %s\n", noack ? "no ACK: written blind (bit-banged)" : spi_bus ? (ok ? "sending (SPI cannot see the display)" : "SPI timeout") : ok ? "working" : "NOT answering", mname[mode]);
    if (cfg_type < 0) kprintf("  (found by itself; the size is a guess - 'display size' if rows are missing)\n");
}
