/*
 * thermal.c - Panasonic AMG8833 "Grid-EYE" 8x8 thermal camera on I2C 0x69 (0x68 with AD_SELECT low) (0.87).
 *
 * Registers: 0x00 PCTL (0 = normal), 0x01 RST (0x3F = initial reset), 0x02 FPSC (0 = 10 fps),
 * 0x0E/0x0F thermistor (12 bit sign + magnitude, 0.0625 C), 0x80..0xFF = 64 pixels x 2 bytes
 * (12 bit two's complement, 0.25 C), row by row. No ID register: it is taken when the thermistor and the
 * pixels give believable temperatures (and no MPU-6050 / clock sits on that address).
 * `thermal` = a live, smoothly scaled heat map on HDMI (any key stops), `thermal once` = the numbers.
 * thermal_poll (core 0, every 0.5 s): a person / warm object = some pixels 2 C (`thermal person`) above the
 * median of the frame; a change goes to MQTT <base>/presence (1 / 0). BASIC SENSOR(14) hottest pixel
 * (0.01 C), SENSOR(15) presence; MQTT state, /api/sensors, display mode `thermal`.
 */
#include "kernel.h"

static int addr;
static int pix[64];                                     /* 0.01 C */
static int therm;                                       /* 0.01 C */
static u64 last_read, next_poll;
static int person_c100 = 200, present, rlo = 0, rhi = 0;   /* range for the map: 0 0 = automatic */
static volatile u32 n_present;

static int rd(void)
{
    u8 b[128], t[2];
    if (!addr || i2c_read_reg((u8)addr, 0x0E, t, 2)) return -1;
    int tr = (t[1] & 7) << 8 | t[0];
    therm = ((t[1] & 8) ? -tr : tr) * 625 / 100;
    for (int c = 0; c < 128; c += 32)                   /* 4 x 32 bytes (some masters dislike long reads) */
        if (i2c_read_reg((u8)addr, (u8)(0x80 + c), b + c, 32)) return -1;
    for (int i = 0; i < 64; i++) {
        int v = (b[2 * i + 1] & 0x0F) << 8 | b[2 * i];
        if (v & 0x800) v -= 0x1000;                     /* 12 bit two's complement */
        pix[i] = v * 25;
    }
    last_read = timer_us();
    return 0;
}

int thermal_present(void) { return addr != 0; }

/* 0 = ok; fills the frame + its summary */
int thermal_get(struct thermal_values *t)
{
    if (!addr) return -1;
    if (!last_read || timer_us() - last_read > 100000) if (rd()) return -1;
    int mn = pix[0], mx = pix[0]; long sum = 0;
    for (int i = 0; i < 64; i++) { if (pix[i] < mn) mn = pix[i]; if (pix[i] > mx) mx = pix[i]; sum += pix[i]; t->pix[i] = pix[i]; }
    int s[64]; for (int i = 0; i < 64; i++) s[i] = pix[i];
    for (int i = 1; i < 64; i++) { int v = s[i], j = i; while (j && s[j - 1] > v) { s[j] = s[j - 1]; j--; } s[j] = v; }
    t->min_c100 = mn; t->max_c100 = mx; t->avg_c100 = (int)(sum / 64); t->median_c100 = (s[31] + s[32]) / 2;
    t->therm_c100 = therm;
    int warm = 0; for (int i = 0; i < 64; i++) if (pix[i] - t->median_c100 >= person_c100) warm++;
    t->warm = warm;
    t->present = present;
    return 0;
}

static int sane(void)
{
    if (rd()) return 0;
    if (therm < -2000 || therm > 8000) return 0;
    int ok = 0; for (int i = 0; i < 64; i++) if (pix[i] > -2000 && pix[i] < 10000) ok++;
    int same = 1; for (int i = 1; i < 64; i++) if (pix[i] != pix[0]) same = 0;
    return ok == 64 && !same;                          /* all equal (e.g. all 0) = not a Grid-EYE */
}

static int try_at(int a)
{
    u8 v = 0;
    if (i2c_write_reg((u8)a, 0x00, &v, 1)) return 0;    /* normal mode */
    v = 0x3F; i2c_write_reg((u8)a, 0x01, &v, 1);       /* initial reset */
    delay_us(50000);
    v = 0x00; i2c_write_reg((u8)a, 0x02, &v, 1);       /* 10 fps */
    delay_us(120000);                                   /* a first frame */
    addr = a;
    if (!sane()) { addr = 0; return 0; }
    return 1;
}

void thermal_boot(void)
{
    int m = motion_present() ? motion_addr() : 0;
    if (m != 0x69 && try_at(0x69)) goto found;
    if (m != 0x68 && !rtc_present() && try_at(0x68)) goto found;
    return;
found:
    kprintf("thermal camera AMG8833 at 0x%02x\n", (u32)addr);
}

void thermal_poll(void)                                 /* core 0 */
{
    if (!addr || io_held() || timer_us() < next_poll) return;
    next_poll = timer_us() + 500000;
    struct thermal_values t;
    if (thermal_get(&t)) return;
    int now = t.warm >= 2;                              /* two warm pixels: a person / something hot */
    if (now != present) {
        present = now;
        if (now) n_present++;
#ifndef THERMAL_HOST_TEST
        if (mqtt_connected()) mqtt_publish("presence", now ? "1" : "0", 1, 1);
#endif
    }
}

/* ---- the picture ---- */
static u32 heat(int v)                                  /* 0..1023 -> black, blue, magenta, red, yellow, white */
{
    if (v < 0) v = 0; if (v > 1023) v = 1023;
    int w = v * 1275 / 1023, seg = w / 255, f = w % 255;
    if (seg > 4) { seg = 4; f = 255; }
    int r, g, b;
    switch (seg) {
    case 0: r = 0; g = 0; b = f; break;                 /* black -> blue */
    case 1: r = f; g = 0; b = 255; break;               /* -> magenta */
    case 2: r = 255; g = 0; b = 255 - f; break;         /* -> red */
    case 3: r = 255; g = f; b = 0; break;               /* -> yellow */
    default: r = 255; g = 255; b = f; break;            /* -> white */
    }
    return (u32)(r << 16 | g << 8 | b);
}

static void c100s(char *b, int v)                       /* "-12.3" */
{
    int n = 0, a = v < 0 ? -v : v;
    if (v < 0) b[n++] = '-';
    char t[8]; int k = 0; int w = a / 100; do t[k++] = (char)('0' + w % 10); while ((w /= 10));
    while (k) b[n++] = t[--k];
    b[n++] = '.'; b[n++] = (char)('0' + a / 10 % 10); b[n] = 0;
}

static void draw_map(const struct thermal_values *t, int x0, int y0, int size)
{
    int lo = rlo, hi = rhi;
    if (lo == hi) { lo = t->min_c100; hi = t->max_c100; if (hi - lo < 300) { int mid = (hi + lo) / 2; lo = mid - 150; hi = mid + 150; } }
    for (int y = 0; y < size; y++) {
        u32 *row = fb_row(y0 + y) + x0;
        int fy = y * 7 * 256 / (size - 1);              /* 8.8 fixed point position in the 8x8 grid */
        int iy = fy >> 8, wy = fy & 255; if (iy >= 7) { iy = 6; wy = 256; }
        for (int x = 0; x < size; x++) {
            int fx = x * 7 * 256 / (size - 1);
            int ix = fx >> 8, wx = fx & 255; if (ix >= 7) { ix = 6; wx = 256; }
            int a = t->pix[iy * 8 + ix], b = t->pix[iy * 8 + ix + 1], c = t->pix[iy * 8 + 8 + ix], d = t->pix[iy * 8 + 9 + ix];
            long top = (long)a * (256 - wx) + (long)b * wx, bot = (long)c * (256 - wx) + (long)d * wx;
            long v = (top * (256 - wy) + bot * wy) >> 16;
            row[x] = heat((int)((v - lo) * 1023 / (hi - lo)));
        }
    }
    /* the scale bar under the map */
    for (int y = 0; y < 12; y++) {
        u32 *row = fb_row(y0 + size + 8 + y) + x0;
        for (int x = 0; x < size; x++) row[x] = heat(x * 1023 / (size - 1));
    }
    char a[16], b[16];
    c100s(a, lo); c100s(b, hi);
    fb_text_px(x0, y0 + size + 24, a, 0xFFFFFF, 0);
    int bl = 0; while (b[bl]) bl++;
    fb_text_px(x0 + size - bl * 8, y0 + size + 24, b, 0xFFFFFF, 0);
}

#ifndef THERMAL_HOST_TEST
static void live(void)
{
    struct thermal_values t;
    int fb = fb_ready() && fb_width() >= 320 && fb_height() >= 240;
    if (fb) { fb_hold(1); for (u32 y = 0; y < fb_height(); y++) { u32 *r = fb_row((int)y); for (u32 x = 0; x < fb_width(); x++) r[x] = 0; } }
    int size = fb ? (int)(fb_height() - 120) : 0;
    if (size > (int)fb_width() / 2) size = (int)fb_width() / 2;
    if (size > 640) size = 640;
    int x0 = 24, y0 = 40;
    if (fb) fb_text_px(x0, 12, "AMG8833 thermal camera - any key stops", 0xFFFFFF, 0);
    while (con_trygetc() < 0) {
        if (thermal_get(&t) == 0) {
            char mx[16], mn[16], av[16], th[16];
            c100s(mx, t.max_c100); c100s(mn, t.min_c100); c100s(av, t.avg_c100); c100s(th, t.therm_c100);
            if (fb) {
                draw_map(&t, x0, y0, size);
                char l[64]; int tx = x0 + size + 32, ty = y0;
                const char *lab[5] = { "max  ", "min  ", "avg  ", "board", "person" };
                const char *val[5] = { mx, mn, av, th, t.warm >= 2 ? "yes" : "no " };
                for (int i = 0; i < 5; i++) {
                    int n = 0; for (const char *s = lab[i]; *s; ) l[n++] = *s++;
                    l[n++] = ' '; for (const char *s = val[i]; *s; ) l[n++] = *s++;
                    if (i < 4) { l[n++] = ' '; l[n++] = 'C'; }
                    while (n < 16) l[n++] = ' ';
                    l[n] = 0;
                    fb_text_px(tx, ty + i * 24, l, i == 4 && t.warm >= 2 ? 0xFF6040 : 0xFFFFFF, 0);
                }
            }
            kprintf("\rmax %s  min %s  avg %s C  person %s   ", mx, mn, av, t.warm >= 2 ? "yes" : "no");   /* serial console */
        }
        u64 end = timer_us() + 120000;
        while (timer_us() < end) { thermal_poll(); delay_us(10000); }
    }
    if (fb) { fb_hold(0); fb_redraw(); }
    kprintf("\n");
}
#endif

void thermal_cmd(int argc, char **argv)
{
    const char *a = argc > 1 ? argv[1] : "";
    if (!strcmp(a, "scan")) { addr = 0; thermal_boot(); if (!addr) kprintf("no AMG8833 at 0x69 / 0x68\n"); return; }
    if (!addr) { kprintf("no thermal camera (AMG8833 Grid-EYE at I2C 0x69; 'thermal scan' looks again)\n"); return; }
    if (!strcmp(a, "person") && argc > 2) {
        u64 v; if (!parse_num(argv[2], &v) || v < 1 || v > 20) { kprintf("thermal person <1-20 C above the background>\n"); return; }
        person_c100 = (int)v * 100; kprintf("person / warm object = %d C above the background\n", (int)v); return;
    }
    if (!strcmp(a, "range") && argc > 2) {
        u64 l, h;
        if (!strcmp(argv[2], "auto")) { rlo = rhi = 0; kprintf("colour range: automatic\n"); return; }
        if (argc < 4 || !parse_num(argv[2], &l) || !parse_num(argv[3], &h) || h <= l || h > 100) { kprintf("thermal range auto | <low C> <high C>\n"); return; }
        rlo = (int)l * 100; rhi = (int)h * 100; kprintf("colour range %d..%d C\n", (int)l, (int)h); return;
    }
#ifndef THERMAL_HOST_TEST
    if (!*a) { live(); return; }
#endif
    if (strcmp(a, "once")) {
        kprintf("usage: thermal                 live heat map (HDMI), any key stops\n"
                "       thermal once            the 8x8 temperatures\n"
                "       thermal range auto | <low> <high>   colours (C)\n"
                "       thermal person <C>      warm pixels this far above the background = a person (default 2)\n"
                "       thermal scan\n");
        return;
    }
    struct thermal_values t;
    if (thermal_get(&t)) { kprintf("thermal camera not answering\n"); return; }
    kprintf("AMG8833 at 0x%02x, 8x8 pixels (C):\n", (u32)addr);
    for (int r = 0; r < 8; r++) {
        kprintf(" ");
        for (int c = 0; c < 8; c++) { int v = t.pix[r * 8 + c], av = v < 0 ? -v : v; kprintf(" %s%2d.%d", v < 0 ? "-" : " ", av / 100, av / 10 % 10); }
        kprintf("\n");
    }
    int bt = t.therm_c100 < 0 ? -t.therm_c100 : t.therm_c100;
    kprintf("  max %d.%02d  min %d.%02d  board %s%d.%02d C, person: %s (%d warm pixels)\n", t.max_c100 / 100, (t.max_c100 < 0 ? -t.max_c100 : t.max_c100) % 100,
            t.min_c100 / 100, (t.min_c100 < 0 ? -t.min_c100 : t.min_c100) % 100, t.therm_c100 < 0 ? "-" : "", bt / 100, bt % 100,
            t.warm >= 2 ? "yes" : "no", t.warm);
}
