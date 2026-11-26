/*
 * access.c - code / card readers for the alarm (0.121): they only READ, alarm.c decides what a key or card means.
 *
 *   keypad gpio [<r1> <r2> <r3> <r4> <c1> <c2> <c3> [c4]]   (no pins = rows 5 6 13 19, columns 12 16 20 21 =
 *        header pins 29 31 33 35 / 32 36 38 40 - the keypad's 8 wires in order, 0.129)
 *        a 4x3 / 4x4 membrane keypad straight on GPIO pins: columns are inputs with the pull-up, one row at a
 *        time is driven LOW (the others float), a low column = that key. Scanned every 10 ms, a key counts after
 *        2 equal scans, once per press. Layout 123A / 456B / 789C / *0#D.
 *   keypad i2c [addr] [swap]
 *        the same keypad behind a PCF8574 (default 0x20; PCF8574A = 0x38-0x3F - but 0x38 is also the AHT20):
 *        P0-P3 rows, P4-P7 columns ("swap" = the other way round). Every 30 ms: 4 x (write the row byte, read).
 *   rfid rc522 [rst]
 *        MFRC522 13.56 MHz reader on SPI0 (CE0 GPIO8, MISO 9, MOSI 10, SCLK 11, 3.3 V!), RST on a GPIO (held
 *        high) or wired to 3.3 V. Every 250 ms: REQA, anticollision (cascade level 1 + 2 = 4 or 7 byte UID),
 *        SELECT, HLTA - a card held at the reader is read once.
 *   rfid wiegand <d0> <d1>
 *        a Wiegand reader / keypad (most 125 kHz EM readers and outdoor keypads; their 5 V data lines need a
 *        divider or a level shifter!). The falling edges are latched by the GPIO event detect (GPFEN / GPEDS) and
 *        collected by core 1 (access_core1_poll from io_poll_all); 30 ms without a bit ends a frame. 4 bits or
 *        8 bits (nibble + inverted nibble) = a key, 26 / 34 bits = a card (parity checked), other lengths = a
 *        card made of all the bits.
 *
 * Events for alarm.c: access_poll() -> 1 = a key ('0'-'9', '*', '#', 'A'-'D'), 2 = a card UID in hex.
 * Cores: everything on core 0 except the Wiegand bit collector (core 1); a small ring with atomics between them.
 */
#include "kernel.h"

#define GPEDS0 (MMIO_BASE + 0x200040)
#define GPFEN0 (MMIO_BASE + 0x200058)

static const char lay[4][5] = { "123A", "456B", "789C", "*0#D" };

/* settings */
static int kp_type;                        /* 0 none, 1 gpio, 2 i2c */
static int kr[4], kc[4], nkc = 3;
static int ki2c = 0x20, kswap;
static int rf_type;                        /* 0 none, 1 rc522, 2 wiegand */
static int rf_rst = -1, wd0 = -1, wd1 = -1;

/* state */
static u64 next_kp, next_rf, kp_retry;
static int kp_last = -1, kp_cand = -1, kp_cnt, kp_err;
static char last_key;
static int rc_ver = -1;                    /* RC522 version register, -1 = not answering */
static u64 rc_retry;
static char last_uid[24], held_uid[24];
static u64 held_until;
static u32 cards_read, keys_read, wg_frames, wg_bad;
static int wg_last_bits;

/* key / card queue for core 0 (keys from the keypads and from Wiegand) */
static char evq[8][24];                    /* [0] = 1 key / 2 card, then the text */
static int eh, et;
static void push(int kind, const char *t)
{
    int h = eh;
    if ((h + 1) % 8 == et) return;
    evq[h][0] = (char)kind;
    int i = 0; for (; t[i] && i < 22; i++) evq[h][1 + i] = t[i];
    evq[h][1 + i] = 0;
    eh = (h + 1) % 8;
}

/* ---------------- settings ---------------- */

static int num(const char *s, int *v)
{
    int x = 0, k = 0, hex = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { hex = 1; s += 2; }
    for (; s[k]; k++) {
        int d = s[k] >= '0' && s[k] <= '9' ? s[k] - '0' : hex && s[k] >= 'a' && s[k] <= 'f' ? s[k] - 'a' + 10 : hex && s[k] >= 'A' && s[k] <= 'F' ? s[k] - 'A' + 10 : -1;
        if (d < 0) return -1;
        x = x * (hex ? 16 : 10) + d;
    }
    if (!k) return -1;
    *v = x;
    return 0;
}
static int gp(const char *s, int *v) { return num(s, v) || *v < 2 || *v > 27 ? -1 : 0; }

/* one settings line split into words ("keypad ..." / "rfid ..."); 0 ok, -1 bad */
int access_set(int n, char **w)
{
    if (n < 2) return -1;
    if (!strcmp(w[0], "keypad")) {
        if (!strcmp(w[1], "off")) { kp_type = 0; return 0; }
        if (!strcmp(w[1], "gpio") && n == 2) {           /* 0.129: the default wiring (no shield): the end of the header */
            static const int dr[4] = { 5, 6, 13, 19 }, dc[4] = { 12, 16, 20, 21 };   /* pins 29 31 33 35 / 32 36 38 40 */
            for (int i = 0; i < 4; i++) { kr[i] = dr[i]; kc[i] = dc[i]; }
            nkc = 4; kp_type = 1;
            return 0;
        }
        if (!strcmp(w[1], "gpio")) {
            int p[8], k = n - 2;
            if (k != 7 && k != 8) return -1;
            for (int i = 0; i < k; i++) { if (gp(w[2 + i], &p[i])) return -1; for (int j = 0; j < i; j++) if (p[j] == p[i]) return -1; }
            for (int i = 0; i < 4; i++) kr[i] = p[i];
            nkc = k - 4;
            for (int i = 0; i < nkc; i++) kc[i] = p[4 + i];
            kp_type = 1;
            return 0;
        }
        if (!strcmp(w[1], "i2c")) {
            int a = 0x20, sw = 0;
            for (int i = 2; i < n; i++) { if (!strcmp(w[i], "swap")) sw = 1; else if (num(w[i], &a) || a < 8 || a > 0x77) return -1; }
            ki2c = a; kswap = sw; kp_type = 2;
            return 0;
        }
        return -1;
    }
    if (!strcmp(w[0], "rfid")) {
        if (!strcmp(w[1], "off")) { rf_type = 0; return 0; }
        if (!strcmp(w[1], "rc522")) {
            int r = -1;
            if (n > 2 && strcmp(w[2], "none") && gp(w[2], &r)) return -1;
            if (r >= 8 && r <= 11) return -1;              /* the SPI pins */
            rf_rst = r; rf_type = 1; rc_ver = -1; rc_retry = 0;
            return 0;
        }
        if (!strcmp(w[1], "wiegand") && n == 4) {
            int a, b;
            if (gp(w[2], &a) || gp(w[3], &b) || a == b) return -1;
            wd0 = a; wd1 = b; rf_type = 2;
            return 0;
        }
        return -1;
    }
    return -1;
}

static int put(char *b, int n, int max, const char *s) { while (*s && n < max - 1) b[n++] = *s++; b[n] = 0; return n; }
static int putn(char *b, int n, int max, int v) { char t[12]; int k = 0; do t[k++] = (char)('0' + v % 10); while ((v /= 10)); while (k && n < max - 1) b[n++] = t[--k]; b[n] = 0; return n; }
static int puth(char *b, int n, int max, int v) { n = put(b, n, max, "0x"); const char *h = "0123456789abcdef"; char t[3] = { h[(v >> 4) & 15], h[v & 15], 0 }; return put(b, n, max, t); }

/* the settings as /ALARM.TXT lines */
int access_text(char *b, int max)
{
    int n = 0;
    b[0] = 0;
    if (kp_type == 1) {
        n = put(b, n, max, "keypad gpio");
        for (int i = 0; i < 4; i++) { n = put(b, n, max, " "); n = putn(b, n, max, kr[i]); }
        for (int i = 0; i < nkc; i++) { n = put(b, n, max, " "); n = putn(b, n, max, kc[i]); }
        n = put(b, n, max, "\r\n");
    } else if (kp_type == 2) {
        n = put(b, n, max, "keypad i2c "); n = puth(b, n, max, ki2c); if (kswap) n = put(b, n, max, " swap"); n = put(b, n, max, "\r\n");
    }
    if (rf_type == 1) { n = put(b, n, max, "rfid rc522"); if (rf_rst >= 0) { n = put(b, n, max, " "); n = putn(b, n, max, rf_rst); } n = put(b, n, max, "\r\n"); }
    else if (rf_type == 2) {
        n = put(b, n, max, "rfid wiegand "); n = putn(b, n, max, wd0); n = put(b, n, max, " "); n = putn(b, n, max, wd1); n = put(b, n, max, "\r\n");
    }
    return n;
}

/* ---------------- RC522 ---------------- */

static int spi_ok = 1;
static void rc_w(u8 reg, u8 v) { u8 t[2] = { (u8)((reg << 1) & 0x7E), v }, r[2]; if (spi_xfer(t, r, 2)) spi_ok = 0; }
static u8 rc_r(u8 reg) { u8 t[2] = { (u8)(((reg << 1) & 0x7E) | 0x80), 0 }, r[2] = { 0, 0 }; if (spi_xfer(t, r, 2)) spi_ok = 0; return r[1]; }
static void rc_set(u8 reg, u8 m) { rc_w(reg, rc_r(reg) | m); }

static int rc_init(void)
{
    spi_ok = 1;
    if (rf_rst >= 0) { gpio_func(rf_rst, 1); gpio_set(rf_rst, 0); delay_us(2000); gpio_set(rf_rst, 1); delay_us(50000); }
    rc_w(0x01, 0x0F);                                     /* SoftReset */
    delay_us(50000);
    int v = rc_r(0x37);
    if (!spi_ok || v == 0x00 || v == 0xFF) return -1;
    rc_w(0x2A, 0x80); rc_w(0x2B, 0xA9); rc_w(0x2C, 0x03); rc_w(0x2D, 0xE8);   /* timer: 25 ms */
    rc_w(0x15, 0x40);                                     /* 100 % ASK */
    rc_w(0x11, 0x3D);                                     /* CRC preset 0x6363 */
    rc_set(0x14, 0x03);                                   /* antenna on */
    return v;
}

static int rc_crc(const u8 *d, int n, u8 *out)
{
    rc_w(0x01, 0x00); rc_w(0x05, 0x04); rc_w(0x0A, 0x80);
    for (int i = 0; i < n; i++) rc_w(0x09, d[i]);
    rc_w(0x01, 0x03);
    u64 end = timer_us() + 20000;
    while (!(rc_r(0x05) & 0x04)) if (timer_us() > end || !spi_ok) return -1;
    rc_w(0x01, 0x00);
    out[0] = rc_r(0x22); out[1] = rc_r(0x21);
    return 0;
}

/* send n bytes (the last one with `bits` bits, 0 = 8), receive into rx; length or -1 none / -2 error */
static int rc_txrx(const u8 *tx, int n, int bits, u8 *rx, int rmax)
{
    rc_w(0x01, 0x00); rc_w(0x04, 0x7F); rc_w(0x0A, 0x80);
    for (int i = 0; i < n; i++) rc_w(0x09, tx[i]);
    rc_w(0x0D, (u8)bits);
    rc_w(0x01, 0x0C);
    rc_set(0x0D, 0x80);                                   /* StartSend */
    u64 end = timer_us() + 40000;
    u8 irq;
    for (;;) {
        irq = rc_r(0x04);
        if (irq & 0x30) break;
        if ((irq & 0x01) || timer_us() > end || !spi_ok) return -1;
    }
    if (rc_r(0x06) & 0x1B) return -2;
    int k = rc_r(0x0A);
    if (k > rmax) k = rmax;
    for (int i = 0; i < k; i++) rx[i] = rc_r(0x09);
    return k;
}

/* one card in the field -> its UID (4 or 7 bytes) as hex; 0 = none */
static int rc_read(char *hex)
{
    u8 r[8], uid[10]; int ul = 0;
    u8 req = 0x26;
    rc_w(0x0E, 0x00);                                     /* CollReg: values after a collision cleared */
    if (rc_txrx(&req, 1, 7, r, 2) != 2) return 0;
    for (int lvl = 0; lvl < 2; lvl++) {
        u8 cl = lvl ? 0x95 : 0x93;
        u8 ac[2] = { cl, 0x20 };
        if (rc_txrx(ac, 2, 0, r, 5) != 5) return 0;
        if ((r[0] ^ r[1] ^ r[2] ^ r[3]) != r[4]) return 0;  /* BCC */
        u8 sel[9] = { cl, 0x70, r[0], r[1], r[2], r[3], r[4] };
        if (rc_crc(sel, 7, sel + 7)) return 0;
        u8 sak[3];
        if (rc_txrx(sel, 9, 0, sak, 3) < 1) return 0;
        if (r[0] == 0x88 && !lvl) { for (int i = 1; i < 4; i++) uid[ul++] = r[i]; continue; }   /* cascade tag */
        for (int i = 0; i < 4; i++) uid[ul++] = r[i];
        break;
    }
    u8 hl[4] = { 0x50, 0x00 };
    if (!rc_crc(hl, 2, hl + 2)) rc_txrx(hl, 4, 0, r, 1);  /* HLTA: no answer expected */
    const char *h = "0123456789abcdef";
    for (int i = 0; i < ul; i++) { hex[2 * i] = h[uid[i] >> 4]; hex[2 * i + 1] = h[uid[i] & 15]; }
    hex[2 * ul] = 0;
    return ul;
}

/* ---------------- Wiegand (bits collected on core 1) ---------------- */

static volatile u64 wbits;
static volatile int wn, wover;
static volatile u64 wlast;
static struct { u64 v; int n; } wq[4];
static volatile int wqh, wqt;

void access_core1_poll(void)
{
    if (rf_type != 2 || wd0 < 0) return;
    u32 m = (1u << wd0) | (1u << wd1);
    u32 ev = mmio_read(GPEDS0) & m;
    u64 t = timer_us();
    if (ev) {
        mmio_write(GPEDS0, ev);
        if (ev == m) wover = 1;                           /* both lines at once: noise */
        else { if (wn < 64) wbits = (wbits << 1) | ((ev >> wd1) & 1); else wover = 1; wn++; }
        wlast = t;
    } else if (wn && t - wlast > 30000) {
        int h = wqh;
        if (!wover && (h + 1) % 4 != wqt) { wq[h].v = wbits; wq[h].n = wn; __atomic_store_n(&wqh, (h + 1) % 4, __ATOMIC_RELEASE); }
        else wg_bad++;
        wbits = 0; wn = 0; wover = 0;
    }
}

static int parity(u64 v, int from, int n) { int p = 0; for (int i = 0; i < n; i++) p ^= (int)((v >> (from + i)) & 1); return p; }

static void wg_frame(u64 v, int n)
{
    wg_frames++;
    wg_last_bits = n;
    static const char kc4[16] = "0123456789*#\0\0\0\0";
    if (n == 4 || (n == 8 && ((v >> 4) & 15) == (~v & 15))) {
        int k = (int)(v & 15);
        if (kc4[k]) { char t[2] = { kc4[k], 0 }; push(1, t); }
        return;
    }
    char hex[24]; int digits; u64 d;
    if (n == 26) {                                        /* E + 24 bits + O: bit 25 = first */
        if (parity(v, 13, 13) != 0 || parity(v, 0, 13) != 1) { wg_bad++; return; }   /* bits 25..13 even, 12..0 odd */
        d = (v >> 1) & 0xFFFFFF; digits = 6;
    } else if (n == 34) {
        if (parity(v, 17, 17) != 0 || parity(v, 0, 17) != 1) { wg_bad++; return; }
        d = (v >> 1) & 0xFFFFFFFFULL; digits = 8;
    } else if (n >= 24 && n <= 64) { d = v; digits = (n + 3) / 4; }
    else { wg_bad++; return; }
    const char *h = "0123456789abcdef";
    for (int i = 0; i < digits; i++) hex[i] = h[(d >> (4 * (digits - 1 - i))) & 15];
    hex[digits] = 0;
    push(2, hex);
}

/* ---------------- keypads ---------------- */

static int scan_gpio(void)
{
    int found = -1;
    for (int r = 0; r < 4 && found < 0; r++) {
        gpio_func(kr[r], 1); gpio_set(kr[r], 0);
        delay_us(10);
        for (int c = 0; c < nkc; c++) if (!gpio_get(kc[c])) { found = r * 4 + c; break; }
        gpio_func(kr[r], 0);
    }
    return found;
}

static int scan_i2c(void)
{
    for (int r = 0; r < 4; r++) {
        u8 o = kswap ? (u8)(0xFF & ~(0x10 << r)) : (u8)(0xFF & ~(1 << r)), in = 0xFF;
        if (i2c_write((u8)ki2c, &o, 1) || i2c_read((u8)ki2c, &in, 1)) return -2;
        int cols = kswap ? (~in & 0x0F) : ((~in >> 4) & 0x0F);
        for (int c = 0; c < 4; c++) if (cols & (1 << c)) return r * 4 + c;
    }
    return -1;
}

/* core 0: set the pins up after a settings change */
void access_setup(void)
{
    if (kp_type == 1) {
        for (int i = 0; i < 4; i++) { gpio_func(kr[i], 0); gpio_pull(kr[i], 2); }
        for (int i = 0; i < nkc; i++) { gpio_func(kc[i], 0); gpio_pull(kc[i], 2); }
    }
    kp_last = kp_cand = -1; kp_cnt = 0; kp_err = 0; kp_retry = 0;
    if (rf_type == 1) { spi_init(0); rc_ver = -1; rc_retry = 0; }
    if (rf_type == 2) {
        gpio_func(wd0, 0); gpio_func(wd1, 0); gpio_pull(wd0, 2); gpio_pull(wd1, 2);
        mmio_write(GPFEN0, mmio_read(GPFEN0) | (1u << wd0) | (1u << wd1));
        mmio_write(GPEDS0, (1u << wd0) | (1u << wd1));
    }
}

int access_active(void) { return kp_type || rf_type; }

/* core 0: 1 = a key in *key, 2 = a card (hex) in uid, 0 = nothing */
int access_poll(char *key, char *uid, int umax)
{
    u64 t = timer_us();
    if (kp_type && t >= next_kp && t >= kp_retry) {
        next_kp = t + (kp_type == 2 ? 30000 : 10000);
        int k = kp_type == 1 ? scan_gpio() : scan_i2c();
        if (k == -2) { kp_err++; kp_retry = t + 2000000; }
        else {
            if (k == kp_cand) { if (kp_cnt < 3) kp_cnt++; } else { kp_cand = k; kp_cnt = 1; }
            if (kp_cnt == 2 && kp_cand != kp_last) {
                kp_last = kp_cand;
                if (kp_cand >= 0) { char c[2] = { lay[kp_cand / 4][kp_cand % 4], 0 }; push(1, c); }
            }
        }
    }
    if (rf_type == 1 && t >= next_rf) {
        next_rf = t + 250000;
        if (rc_ver < 0) { if (t >= rc_retry) { rc_ver = rc_init(); if (rc_ver < 0) rc_retry = t + 5000000; } }
        else {
            char hex[24];
            if (rc_read(hex) > 0) {
                if (strcmp(hex, held_uid) || t >= held_until) push(2, hex);
                int i = 0; for (; hex[i] && i < 23; i++) held_uid[i] = hex[i]; held_uid[i] = 0;
                held_until = t + 2000000;
            }
            if (!spi_ok) rc_ver = -1;
        }
    }
    while (wqt != wqh) {
        int i = wqt;
        wg_frame(wq[i].v, wq[i].n);
        __atomic_store_n(&wqt, (i + 1) % 4, __ATOMIC_RELEASE);
    }
    if (et == eh) return 0;
    int kind = evq[et][0];
    if (kind == 1) { *key = evq[et][1]; last_key = *key; keys_read++; }
    else { int i = 0; for (; evq[et][1 + i] && i < umax - 1; i++) uid[i] = evq[et][1 + i]; uid[i] = 0;
           for (i = 0; uid[i] && i < 23; i++) last_uid[i] = uid[i]; last_uid[i] = 0; cards_read++; }
    et = (et + 1) % 8;
    return kind;
}

/* for 'alarm': what is configured and how it goes */
void access_info(void)
{
    if (kp_type == 1) {
        kprintf("  keypad on GPIO: rows %d %d %d %d, columns", kr[0], kr[1], kr[2], kr[3]);
        for (int i = 0; i < nkc; i++) kprintf(" %d", kc[i]);
        kprintf(" (4x%d), keys %u%s%c\n", nkc, keys_read, keys_read ? ", last " : "", keys_read ? last_key : ' ');
    } else if (kp_type == 2)
        kprintf("  keypad on I2C 0x%x (PCF8574, rows P%s): %s, keys %u\n", ki2c, kswap ? "4-7" : "0-3",
                kp_err && kp_retry > timer_us() ? "NOT answering" : "ok", keys_read);
    if (rf_type == 1) {
        if (rc_ver >= 0) kprintf("  RFID RC522 on SPI0 (version 0x%x), cards read %u%s%s\n", rc_ver, cards_read, cards_read ? ", last " : "", cards_read ? last_uid : "");
        else kprintf("  RFID RC522 on SPI0: NOT answering (wiring GPIO8-11, 3.3 V, RST high?)\n");
    } else if (rf_type == 2)
        kprintf("  Wiegand D0 GPIO%d D1 GPIO%d: frames %u (bad %u), last %d bits%s%s\n", wd0, wd1, wg_frames, wg_bad, wg_last_bits,
                cards_read ? ", last card " : "", cards_read ? last_uid : "");
    if (!kp_type && !rf_type) kprintf("  no keypad / card reader ('alarm keypad ...', 'alarm rfid ...')\n");
}
