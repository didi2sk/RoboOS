/*
 * PC test of src/display.c: fake SSD1306 / SH1106 / HD44780-on-PCF8574 chips decode every I2C byte
 * the driver sends, then the picture is read back as text (OLED pixels matched against the font).
 * build + run:  cc -O1 -o /tmp/test_display tools/test_display.c src/font.c src/font5x7.c && /tmp/test_display
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int64_t i64;
struct fat_file { int is_dir; int pos; };
static const char *cfg_text;                      /* the fake /DISPLAY.TXT */
static char saved[256];
static int fat_mounted(void) { return 1; }
static int fat_open(const char *p, struct fat_file *f) { (void)p; f->is_dir = 0; f->pos = 0; return cfg_text ? 0 : -1; }
static int fat_read(struct fat_file *f, void *b, u32 n) { (void)f; int l = (int)strlen(cfg_text); if (l > (int)n) l = (int)n; memcpy(b, cfg_text, (size_t)l); return l; }
static int fat_write_file(const char *p, const void *d, u32 n) { (void)p; memcpy(saved, d, n); saved[n] = 0; return 0; }
static int memcmp_n(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static int parse_num(const char *s, u64 *o) { char *e; *o = strtoull(s, &e, 0); return e != s && !*e; }
static u64 now_us;
static u64 timer_us(void) { return now_us; }
static void delay_us(u64 us) { now_us += us; }
#define kprintf printf
static void i2c_init(void) {}
static const char *net_name(void) { return "rpi3b-1234"; }
static int ip_started(void) { return 1; }
static const char *ip_text = "192.168.1.104";
static const char *ip_address_str(void) { return ip_text; }
static int wifi_joining(void) { return 0; }
static int wifi_joined(void) { return 1; }
static const char *wifi_ssid(void) { return "iot"; }
static int link_up(void) { return 1; }
static int link_wifi(void) { return 1; }
static u64 clock_now(void) { return 1795000000; }
static int clock_format(char *b, u64 t, int iso) { (void)t; (void)iso; strcpy(b, "2026-11-21 11:01:27"); return 19; }
static const char *os_version(void) { return "0.56"; }
static const char *board_name(void) { return "Raspberry Pi 3 Model B+"; }
static int sound_is_exclusive(void) { return 0; }

/* ---------------- the fake bus ---------------- */
enum { NONE, SSD, SH, LCD };
static int chip, chip_addr, chip_noack, chip_wonly;   /* chip_wonly: ACKs writes, not reads (SSD1309) */            /* chip_noack: never ACKs (2.42" board, D2 open) */
static u8 status_byte;
static u8 gram[8][132];
static int col, page, c0 = 0, c1 = 127, p0 = 0, p1 = 7, sh_col;
static char ddram[128];
static int l4bit, lnib_hi = -1, lhi, laddr, last_e, lbl;
static int errors;

static void ssd_cmds(const u8 *b, int n)
{
    for (int i = 0; i < n; i++) {
        u8 c = b[i];
        if (chip == SH) {
            if (c >= 0xB0 && c <= 0xB7) page = c - 0xB0;
            else if (c <= 0x0F) sh_col = (sh_col & 0xF0) | c;
            else if (c >= 0x10 && c <= 0x1F) sh_col = (sh_col & 0x0F) | ((c & 15) << 4);
            else if (c == 0x81 || c == 0xD5 || c == 0xA8 || c == 0xD3 || c == 0xDA || c == 0xD9 || c == 0xDB || c == 0xAD) i++;
            else if (c == 0x8D || c == 0x20 || c == 0x21 || c == 0x22) { printf("SH1106 got an SSD1306 command 0x%02x\n", c); errors++; }
            continue;
        }
        if (c == 0x21) { c0 = b[i + 1]; c1 = b[i + 2]; col = c0; i += 2; }
        else if (c == 0x22) { p0 = b[i + 1]; p1 = b[i + 2]; page = p0; i += 2; }
        else if (c == 0x20) { if (b[i + 1] != 0) { printf("not horizontal mode\n"); errors++; } i++; }
        else if (c == 0x81 || c == 0xD5 || c == 0xA8 || c == 0xD3 || c == 0xDA || c == 0xD9 || c == 0xDB || c == 0x8D) i++;
    }
}

static void ssd_data(const u8 *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (chip == SH) { if (sh_col < 132) gram[page][sh_col++] = b[i]; continue; }
        gram[page][col] = b[i];
        if (++col > c1) { col = c0; if (++page > p1) page = p0; }
    }
}

static void lcd_byte(u8 v)
{
    int e = v & 4;
    lbl = v & 8;
    if (last_e && !e) {                            /* falling edge of E: latch the nibble */
        int nib = v >> 4, rs = v & 1;
        if (!l4bit) { if (nib == 2) l4bit = 1; }   /* the 8-bit-mode init writes; 0x2 = to 4-bit */
        else if (lnib_hi < 0) { lnib_hi = nib; lhi = rs; }
        else {
            u8 val = (u8)(lnib_hi << 4 | nib);
            if (lhi != rs) { printf("RS changed mid-byte\n"); errors++; }
            lnib_hi = -1;
            if (rs) { ddram[laddr & 127] = (char)val; laddr++; }
            else if (val == 1) { memset(ddram, ' ', sizeof(ddram)); laddr = 0; }
            else if (val & 0x80) laddr = val & 0x7F;
        }
    }
    last_e = e;
}

static int bus_write(u8 a, const void *bv, int n);
static int i2c_write(u8 a, const void *bv, int n) { if (chip_noack) return -1; return bus_write(a, bv, n); }
static int i2c_write_noack(u8 a, const void *bv, int n) { bus_write(a, bv, n); return 0; }
static int bus_write(u8 a, const void *bv, int n)
{
    const u8 *b = bv;
    if (!chip || a != chip_addr) return -1;
    if (chip == LCD) { for (int i = 0; i < n; i++) lcd_byte(b[i]); return 0; }
    if (b[0] == 0x00) ssd_cmds(b + 1, n - 1);
    else if (b[0] == 0x40) ssd_data(b + 1, n - 1);
    else { printf("bad control byte 0x%02x\n", b[0]); errors++; }
    return 0;
}
static int i2c_read(u8 a, void *b, int n) { (void)n; if (chip_wonly || chip_noack || !chip || a != chip_addr) return -1; *(u8 *)b = status_byte; return 0; }
static int i2c_probe_write(u8 a) { u8 z = 0; return i2c_write(a, &z, 1) == 0; }
static int i2c_probe(u8 a) { u8 b; return i2c_read(a, &b, 1) == 0; }

#define DISPLAY_HOST_TEST
#include "../src/display.c"

/* ---------------- reading the picture back ---------------- */
extern const unsigned char font5x7[95][5];
static void oled_textrow_small(int r, char *out)        /* 6x8 cells, 21 columns */
{
    for (int c = 0; c < 21; c++) {
        out[c] = '?';
        for (int k = 0; k < 95; k++) if (!memcmp(&gram[r][c * 6], font5x7[k], 5) && !gram[r][c * 6 + 5]) { out[c] = (char)(32 + k); break; }
    }
    out[21] = 0;
}
static void oled_textrow_big(int r, char *out)          /* 16x32 cells, 8 columns: undouble, compare with 8x16 */
{
    for (int c = 0; c < 8; c++) {
        u8 g[16] = { 0 };
        for (int p = 0; p < 4; p++)
            for (int x = 0; x < 16; x += 2) {
                u8 v = gram[r * 4 + p][c * 16 + x];
                for (int y = 0; y < 8; y += 2) if (v & (1 << y)) g[(p * 8 + y) / 2] |= (u8)(0x80 >> (x / 2));
            }
        out[c] = '?';
        for (int k = 0; k < 95; k++) if (!memcmp(g, font8x16[k], 16)) { out[c] = (char)(32 + k); break; }
    }
    out[8] = 0;
}
static void expect_small(const char *what, int nrows, const char *const *want)
{
    for (int r = 0; r < nrows; r++) {
        char got[24], w[24];
        oled_textrow_small(r, got);
        snprintf(w, sizeof(w), "%-21.21s", want[r]);
        int good = !strcmp(got, w);
        printf("  %s row %d: [%s] %s\n", what, r + 1, got, good ? "ok" : "WRONG");
        if (!good) { printf("     want [%s]\n", w); errors++; }
    }
}
static void oled_textrow(int r, int x0, char *out)
{
    for (int c = 0; c < 16; c++) {
        u8 g[16] = { 0 };
        for (int half = 0; half < 2; half++)
            for (int x = 0; x < 8; x++) {
                u8 v = gram[r * 2 + half][x0 + c * 8 + x];
                for (int y = 0; y < 8; y++) if (v & (1 << y)) g[half * 8 + y] |= (u8)(0x80 >> x);
            }
        out[c] = '?';
        for (int k = 0; k < 95; k++) if (!memcmp(g, font8x16[k], 16)) { out[c] = (char)(32 + k); break; }
    }
    out[16] = 0;
}

static void lcd_textrow(int r, int w, char *out)
{
    static const u8 off20[4] = { 0x00, 0x40, 0x14, 0x54 }, off16[4] = { 0x00, 0x40, 0x10, 0x50 };
    int o = w == 20 ? off20[r] : off16[r];
    memcpy(out, ddram + o, (size_t)w);
    out[w] = 0;
}

static void reset(int c, int addr, u8 st, const char *cfg)
{
    chip = c; chip_addr = addr; chip_noack = 0; chip_wonly = 0; status_byte = st; cfg_text = cfg;
    memset(gram, 0, sizeof(gram)); memset(ddram, 0, sizeof(ddram));
    l4bit = 0; lnib_hi = -1; last_e = 0; col = page = sh_col = 0;
    cfg_type = -1; cfg_addr = cfg_w = cfg_h = 0; mode = M_STATUS; font = 1; contrast = 0xCF; backlight = 1; ok = 0; busy = 0;
    cur_r = cur_c = 0; dirty = 0; npend = 0;
    now_us = 0;
}

static void expect_rows(const char *what, int lcdw, int x0, int nrows, const char *const *want)
{
    for (int r = 0; r < nrows; r++) {
        char got[24];
        if (lcdw) lcd_textrow(r, lcdw, got); else oled_textrow(r, x0, got);
        char w[24];
        snprintf(w, sizeof(w), "%-*.*s", lcdw ? lcdw : 16, lcdw ? lcdw : 16, want[r]);
        int good = !strcmp(got, w);
        printf("  %s row %d: [%s] %s\n", what, r + 1, got, good ? "ok" : "WRONG");
        if (!good) { printf("     want [%s]\n", w); errors++; }
    }
}

static void check(const char *what, int cond) { printf("  %s: %s\n", what, cond ? "ok" : "WRONG"); if (!cond) errors++; }

int main(void)
{
    printf("SSD1306 128x64 at 0x3C (status 0x43):\n");
    reset(SSD, 0x3C, 0x43, 0);
    display_boot();
    check("found as ssd1306", dtype == D_SSD1306 && daddr == 0x3C && rows == 4);
    { const char *w[] = { "RoboOS", "0.56", "Raspberry Pi 3 M", "" }; expect_rows("boot", 0, 0, 4, w); }
    now_us += 2100000; display_poll();
    { const char *w[] = { "rpi3b-1234", "192.168.1.104", "21.11. 11:01:27", "WiFi iot" }; expect_rows("status", 0, 0, 4, w); }
    display_text(1, "Hello, world!");
    { const char *w[] = { "rpi3b-1234", "Hello, world!", "21.11. 11:01:27", "WiFi iot" }; expect_rows("text", 0, 0, 4, w); }
    check("status off after text", !status_on);

    printf("SSD1306 128x32 (DISPLAY.TXT size 128x32) at 0x3D:\n");
    reset(SSD, 0x3D, 0x06, "type ssd1306\nsize 128x32\n");
    display_boot();
    check("2 rows", rows == 2 && dh == 32 && daddr == 0x3D);
    now_us += 2100000; display_poll();
    { const char *w[] = { "192.168.1.104", "21.11. 11:01:27" }; expect_rows("status", 0, 0, 2, w); }

    printf("SH1106 at 0x3C (status 0x08):\n");
    reset(SH, 0x3C, 0x08, 0);
    display_boot();
    check("found as sh1106", dtype == D_SH1106);
    { const char *w[] = { "RoboOS", "0.56", "Raspberry Pi 3 M", "" }; expect_rows("boot (columns 2..129)", 0, 2, 4, w); }

    printf("LCD 16x2 on a PCF8574 at 0x27:\n");
    reset(LCD, 0x27, 0xFF, 0);
    display_boot();
    check("found as lcd 16x2", dtype == D_LCD && cols == 16 && rows == 2);
    check("backlight on", lbl);
    { const char *w[] = { "RoboOS", "0.56" }; expect_rows("boot", 16, 0, 2, w); }
    now_us += 2100000; display_poll();
    { const char *w[] = { "192.168.1.104", "21.11. 11:01:27" }; expect_rows("status", 16, 0, 2, w); }

    printf("LCD 20x4 at 0x3F (DISPLAY.TXT size 20x4):\n");
    reset(LCD, 0x3F, 0xFF, "size 20x4\nstatus on\n");
    display_boot();
    check("found as lcd 20x4 at 0x3f", dtype == D_LCD && cols == 20 && rows == 4 && daddr == 0x3F);
    now_us += 2100000; display_poll();
    { const char *w[] = { "rpi3b-1234", "192.168.1.104", "2026-11-21 11:01:27", "WiFi iot" }; expect_rows("status", 20, 0, 4, w); }
    { char *av[] = { "display", "text", "4", "temp", "21.5", "C" }; display_cmd(6, av); }
    { const char *w[] = { "rpi3b-1234", "192.168.1.104", "2026-11-21 11:01:27", "temp 21.5 C" }; expect_rows("display text 4", 20, 0, 4, w); }
    { char *av[] = { "display", "status", "off" }; display_cmd(3, av); }
    check("saved DISPLAY.TXT", !strcmp(saved, "type auto\nsize 20x4\nmode text\nfont normal\ncontrast 207\n"));
    printf("    %s", saved);
    { char *av[] = { "display", "clear" }; display_cmd(2, av); }
    { const char *w[] = { "", "", "", "" }; expect_rows("clear", 20, 0, 4, w); }

    printf("nothing on the bus:\n");
    reset(NONE, 0, 0, 0);
    display_boot();
    check("no display", !display_found() && display_text(0, "x") == -1);
    { char *av[] = { "display" }; display_cmd(1, av); }

    printf("SSD1309 that ACKs writes only (no reads), auto:\n");
    reset(SSD, 0x3C, 0, 0); chip_wonly = 1;
    display_boot();
    check("found by a write, ssd1306, not blind", display_found() && dtype == D_SSD1306 && !noack && daddr == 0x3C);
    { const char *w[] = { "RoboOS", "0.56", "Raspberry Pi 3 M", "" }; expect_rows("boot", 0, 0, 4, w); }

    printf("2.42\" SSD1309 without ACK, auto: not found; type ssd1306: written blind\n");
    reset(SSD, 0x3C, 0, 0); chip_noack = 1;
    display_boot();
    check("auto: not found", !display_found());
    reset(SSD, 0x3C, 0, "type ssd1306\n"); chip_noack = 1;
    display_boot();
    check("type set: used without ACK", display_found() && noack && daddr == 0x3C);
    { const char *w[] = { "RoboOS", "0.56", "Raspberry Pi 3 M", "" }; expect_rows("boot", 0, 0, 4, w); }
    { char *av[] = { "display" }; display_cmd(1, av); }

    printf("small font (DISPLAY.TXT font small): 21 x 8, status screen\n");
    reset(SSD, 0x3C, 0x43, "font small\n");
    display_boot();
    check("21 x 8", cols == 21 && rows == 8);
    now_us += 2100000; display_poll();
    { const char *w[] = { "RoboOS 0.56", "rpi3b-1234", "192.168.1.104", "", "2026-11-21 11:01:27", "WiFi iot", "", "up 0h 0m" }; expect_small("status", 8, w); }

    printf("console mode (small font): the terminal copied, scrolling\n");
    reset(SSD, 0x3C, 0x43, "font small\nmode console\n");
    display_boot();
    for (int i = 1; i <= 9; i++) { char l[32]; snprintf(l, sizeof(l), "line %d\tx\n", i); for (char *c = l; *c; c++) display_con_putc(*c); }
    for (const char *c = "\033[1;32mrobo$ lsx\b \b"; *c; c++) display_con_putc(*c);
    now_us += 200000; display_poll();
    { const char *w[] = { "line 3  x", "line 4  x", "line 5  x", "line 6  x", "line 7  x", "line 8  x", "line 9  x", "robo$ ls" }; expect_small("console", 8, w); }
    for (const char *c = "0123456789012345678901234\n"; *c; c++) display_con_putc(*c);
    now_us += 200000; display_poll();
    { const char *w[] = { "line 5  x", "line 6  x", "line 7  x", "line 8  x", "line 9  x", "robo$ ls0123456789012", "345678901234", "" }; expect_small("wrap", 8, w); }

    printf("big font: 8 x 2\n");
    reset(SSD, 0x3C, 0x43, "font big\nmode text\n");
    display_boot();
    check("8 x 2", cols == 8 && rows == 2);
    display_text(0, "12:34"); display_text(1, "21.5 C");
    { char g0[9], g1[9]; oled_textrow_big(0, g0); oled_textrow_big(1, g1);
      check("big row 1 = '12:34   '", !strcmp(g0, "12:34   ")); check("big row 2 = '21.5 C  '", !strcmp(g1, "21.5 C  ")); }

    printf("network text (core 1 -> core 0)\n");
    reset(SSD, 0x3C, 0x43, 0);
    display_boot();
    display_net(-1, "Hello\nfrom MQTT\nrow 3", 22);
    display_poll();
    { const char *w[] = { "Hello", "from MQTT", "row 3", "" }; expect_rows("net all", 0, 0, 4, w); }
    display_net(3, "only row 4", 10);
    display_poll();
    { const char *w[] = { "Hello", "from MQTT", "row 3", "only row 4" }; expect_rows("net row", 0, 0, 4, w); }
    check("mode text after network text", mode == M_TEXT);

    printf("type none in DISPLAY.TXT (an OLED is there):\n");
    reset(SSD, 0x3C, 0x43, "type none\n");
    display_boot();
    check("not used", !display_found());

    printf(errors ? "\n%d ERRORS\n" : "\nall OK\n", errors);
    return errors != 0;
}
