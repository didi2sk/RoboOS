/* PC test of src/access.c (0.121): a 4x4 keypad on GPIO, the same behind a PCF8574, an RC522 with a 4-byte and a
 * 7-byte card (REQA, anticollision CL1/CL2, SELECT with CRC_A, HLTA), Wiegand 26 / 34 bit cards and 4 / 8 bit keys.
 * cc -O1 -w -o /tmp/test_access tools/test_access.c && /tmp/test_access */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define KERNEL_H
#define MMIO_BASE 0x3F000000UL
static u64 now_us;
static u64 timer_us(void) { return now_us; }
static void delay_us(u64 u) { now_us += u; }
static void kprintf(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); }
static int con_trygetc(void) { return -1; }   /* access_keypad_test (0.132) */
static int io_core_active(void) { return 0; }
static void display_poke(void) {}
enum { KA = 6, KB = 13, KP = 19, KOK = 26, KBK = 21 };
static int knob_test;
static int knob_lvl[32];                       /* 0.145: knob pins 6 13 19 26 21, 1 = high (idle) */
static int is_knob(int p) { return p == 6 || p == 13 || p == 19 || p == 26 || p == 21; }

/* ---- GPIO: keypad rows 13 19 26 21 cols 20 7 18 6; Wiegand D0 22 D1 27 (event detect) ---- */
static int fn[32], lvl[32], key_r = -1, key_c = -1;
static const int R[4] = { 13, 19, 26, 21 }, C[4] = { 20, 7, 18, 6 };
static void gpio_func(int p, int f) { fn[p] = f; }
static void gpio_pull(int p, int m) {}
static void gpio_set(int p, int on) { lvl[p] = on; }
/* the real wiring: key (r, c) joins line PR[r] with line PC[c]; -1 = that keypad wire sits on GND (0.130) */
static int PR[4] = { 13, 19, 26, 21 }, PC[4] = { 20, 7, 18, 6 };
static int low_by(int q) { return q == -1 || (q >= 0 && fn[q] == 1 && lvl[q] == 0); }
static int gpio_get(int p)
{
    if (knob_test && is_knob(p)) return knob_lvl[p];
    if (key_r < 0) return 1;
    int a = PR[key_r], b = PC[key_c];
    if (p == b && low_by(a)) return 0;
    if (p == a && low_by(b)) return 0;
    return 1;
}
static u32 gpeds, gpfen;
static u32 mmio_read(u64 a) { return a == MMIO_BASE + 0x200040 ? gpeds : a == MMIO_BASE + 0x200058 ? gpfen : 0; }
static void mmio_write(u64 a, u32 v) { if (a == MMIO_BASE + 0x200040) gpeds &= ~v; else if (a == MMIO_BASE + 0x200058) gpfen = v; }

/* ---- PCF8574 at 0x20 ---- */
static u8 pcf_out = 0xFF; static int pcf_r = -1, pcf_c = -1, pcf_here = 1;
static int i2c_write(u8 a, const void *b, int n) { if (a != 0x20 || !pcf_here) return -1; pcf_out = ((const u8 *)b)[n - 1]; return 0; }
static int i2c_read(u8 a, void *b, int n)
{
    if (a != 0x20 || !pcf_here) return -1;
    u8 v = pcf_out;
    if (pcf_r >= 0 && !(pcf_out & (1 << pcf_r))) v &= (u8)~(0x10 << pcf_c);
    ((u8 *)b)[0] = v;
    return 0;
}

/* ---- RC522 ---- */
static u8 reg[64], fifo[64]; static int fl;
static int card = 0;              /* 0 none, 1 = 4 bytes de ad be ef, 2 = 7 bytes 04 11 22 33 44 55 66 */
static int halted, active_lvl;
static void crc_a(const u8 *d, int n, u8 *o)
{
    u32 c = 0x6363;
    for (int i = 0; i < n; i++) { u8 b = d[i] ^ (u8)(c & 0xFF); b ^= (u8)(b << 4); c = (c >> 8) ^ ((u32)b << 8) ^ ((u32)b << 3) ^ ((u32)b >> 4); c &= 0xFFFF; }
    o[0] = (u8)c; o[1] = (u8)(c >> 8);
}
static void answer(const u8 *d, int n) { memcpy(fifo, d, n); fl = n; reg[0x04] |= 0x30; }
static void transceive(void)
{
    u8 in[64]; int n = fl; memcpy(in, fifo, n); fl = 0;
    reg[0x04] = 0;
    static const u8 u4[4] = { 0xde, 0xad, 0xbe, 0xef }, u7[7] = { 0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    if (!card) { reg[0x04] |= 0x01; return; }
    if (n == 1 && in[0] == 0x26 && (reg[0x0D] & 7) == 7) { if (halted) { reg[0x04] |= 0x01; return; } u8 a[2] = { 0x44, 0 }; answer(a, 2); active_lvl = 0; return; }
    if (n == 2 && in[1] == 0x20 && (in[0] == 0x93 || in[0] == 0x95)) {
        u8 r[5];
        if (in[0] == 0x93) { if (card == 1) memcpy(r, u4, 4); else { r[0] = 0x88; memcpy(r + 1, u7, 3); } }
        else { if (card != 2) { reg[0x04] |= 0x01; return; } memcpy(r, u7 + 3, 4); }
        r[4] = r[0] ^ r[1] ^ r[2] ^ r[3];
        answer(r, 5); return;
    }
    if (n == 9 && in[1] == 0x70) {
        u8 c[2]; crc_a(in, 7, c);
        if (c[0] != in[7] || c[1] != in[8]) { reg[0x06] = 0x04; reg[0x04] |= 0x30; return; }
        u8 s[3] = { (u8)(in[0] == 0x93 && card == 2 ? 0x04 : 0x08) }; crc_a(s, 1, s + 1); active_lvl = 1; answer(s, 3); return;
    }
    if (n == 4 && in[0] == 0x50) { u8 c[2]; crc_a(in, 2, c); if (c[0] == in[2] && c[1] == in[3] && active_lvl) halted = 1; reg[0x04] |= 0x01; return; }
    reg[0x04] |= 0x01;
}
static void spi_init(u32 d) {}
static int spi_xfer(const void *tx, void *rx, int n)
{
    const u8 *t = tx; u8 *r = rx;
    int a = (t[0] >> 1) & 0x3F, rd = t[0] & 0x80;
    r[0] = 0;
    if (rd) {
        if (a == 0x09) r[1] = fl ? fifo[0] : 0, (fl ? (memmove(fifo, fifo + 1, --fl), 0) : 0);
        else if (a == 0x0A) r[1] = (u8)fl;
        else if (a == 0x37) r[1] = 0x92;
        else r[1] = reg[a];
        return 0;
    }
    u8 v = t[1];
    if (a == 0x09) { fifo[fl++] = v; return 0; }
    if (a == 0x0A) { if (v & 0x80) fl = 0; return 0; }
    if (a == 0x04 || a == 0x05) { if (!(v & 0x80)) reg[a] &= (u8)~v; return 0; }
    if (a == 0x01) {
        reg[1] = v;
        if (v == 0x03) { crc_a(fifo, fl, &reg[0x22]); u8 t2 = reg[0x22]; reg[0x22] = t2; reg[0x21] = reg[0x23]; fl = 0; reg[0x05] |= 0x04; }
        if (v == 0x0F) memset(reg, 0, sizeof(reg));
        return 0;
    }
    if (a == 0x0D) { reg[a] = v; if ((v & 0x80) && reg[1] == 0x0C) { reg[0x06] = 0; transceive(); } return 0; }
    reg[a] = v;
    return 0;
}

static int strcmp_(const char *a, const char *b) { return strcmp(a, b); }
#include "../src/access.c"

static int errors;
static void check(const char *w, int ok) { printf("  %-60s %s\n", w, ok ? "ok" : "WRONG"); if (!ok) errors++; }
static char got[64]; static int gn; static char gcard[32];
static void run_ms(int ms)
{
    for (int i = 0; i < ms; i++) {
        access_core1_poll();
        char k, u[32];
        int e = access_poll(&k, u, sizeof(u));
        if (e == 1 && gn < 63) { got[gn++] = k; got[gn] = 0; }
        if (e == 2) strcpy(gcard, u);
        now_us += 1000;
    }
}
static void press(int r, int c, int ms) { key_r = r; key_c = c; run_ms(ms); key_r = -1; run_ms(60); }
static void wsend(u64 v, int n)
{
    for (int i = n - 1; i >= 0; i--) { gpeds |= 1u << ((v >> i) & 1 ? 27 : 22); access_core1_poll(); now_us += 2000; access_core1_poll(); }
    run_ms(50);
}
static int setline(const char *l) { char b[96], *w[12]; int n = 0; strcpy(b, l); for (char *q = strtok(b, " "); q && n < 12; q = strtok(0, " ")) w[n++] = q; return access_set(n, w); }

int main(void)
{
    /* GPIO keypad */
    check("keypad gpio line accepted", setline("keypad gpio 13 19 26 21 20 7 18 6") == 0);
    check("a wrong line refused (pin twice)", setline("keypad gpio 13 13 26 21 20 7 18 6") != 0);
    { char t0[200]; check("keypad gpio (no pins) = 5 6 13 19 / 12 16 20 21", setline("keypad gpio") == 0 && access_text(t0, sizeof(t0)) && strstr(t0, "keypad gpio 5 6 13 19 12 16 20 21")); }
    setline("keypad gpio 13 19 26 21 20 7 18 6");
    access_setup();
    press(0, 0, 50); press(0, 1, 50); press(0, 2, 50); press(1, 0, 50); press(3, 2, 50); press(0, 3, 50); press(3, 0, 50);
    check("GPIO keypad: 1 2 3 4 # A *", !strcmp(got, "1234#A*"));
    gn = 0; got[0] = 0;
    key_r = 2; key_c = 1; run_ms(500); key_r = -1; run_ms(50);
    check("held key counts once", !strcmp(got, "8"));
    gn = 0; got[0] = 0;
    key_r = 1; key_c = 1; run_ms(5); key_r = -1; run_ms(50);
    check("5 ms glitch ignored", gn == 0);
    char t[200]; access_text(t, sizeof(t));
    check("settings text", strstr(t, "keypad gpio 13 19 26 21 20 7 18 6") != 0);
    /* 0.130: the connector straight on header pins 23-37 (pin 25 = GND) */
    {
        static const char want[] = "123A456B789C*0#D";
        int pr[4] = { 11, -1, 0, 5 }, pc[4] = { 6, 13, 19, 26 };
        memcpy(PR, pr, sizeof(pr)); memcpy(PC, pc, sizeof(pc));
        check("keypad plug accepted", setline("keypad plug") == 0);
        access_setup(); gn = 0; got[0] = 0;
        for (int k = 0; k < 16; k++) press(k / 4, k % 4, 50);
        printf("    got \"%s\"\n", got);
        check("keypad plug: all 16 keys (one wire on GND)", !strcmp(got, want));
        char t1[200]; access_text(t1, sizeof(t1));
        check("settings text: keypad gpio 11 gnd 0 5 6 13 19 26", strstr(t1, "keypad gpio 11 gnd 0 5 6 13 19 26") != 0);
        int rr[4] = { 26, 19, 13, 6 }, rc[4] = { 5, 0, -1, 11 };   /* plugged the other way round */
        memcpy(PR, rr, sizeof(rr)); memcpy(PC, rc, sizeof(rc));
        gn = 0; got[0] = 0;
        for (int k = 0; k < 16; k++) press(k / 4, k % 4, 50);
        int distinct = 1; for (int i = 0; i < gn; i++) for (int j = 0; j < i; j++) if (got[i] == got[j]) distinct = 0;
        check("plugged reversed: still 16 different keys", gn == 16 && distinct);
        check("keypad plug reversed accepted", setline("keypad plug reversed") == 0);
        access_setup(); gn = 0; got[0] = 0;
        for (int k = 0; k < 16; k++) press(k / 4, k % 4, 50);
        check("keypad plug reversed: all 16 keys right", !strcmp(got, want));
        int sr2[4] = { 13, 19, 26, 21 }, sc2[4] = { 20, 7, 18, -1 };   /* keypad shield: wire 8 on GND */
        memcpy(PR, sr2, sizeof(sr2)); memcpy(PC, sc2, sizeof(sc2));
        check("keypad shield accepted", setline("keypad shield") == 0);
        access_setup(); gn = 0; got[0] = 0;
        for (int k = 0; k < 16; k++) press(k / 4, k % 4, 50);
        check("keypad shield: all 16 keys (column 4 on GND)", !strcmp(got, want));
        check("keypad shield leaves GPIO 5 + SPI free", !access_uses_pin(5) && !access_uses_pin(8) && access_uses_pin(13));
        check("gpio line with two gnd + pins accepted", setline("keypad gpio gnd 6 13 19 26 5 0 gnd") == 0);
        check("three gnd refused", setline("keypad gpio gnd gnd 13 19 26 5 0 gnd") != 0);
        setline("keypad off");
    }
    /* I2C keypad */
    check("keypad i2c line accepted", setline("keypad i2c 0x20") == 0);
    access_setup(); gn = 0; got[0] = 0;
    pcf_r = 0; pcf_c = 1; run_ms(100); pcf_r = -1; run_ms(100);
    pcf_r = 3; pcf_c = 3; run_ms(100); pcf_r = -1; run_ms(100);
    pcf_r = 2; pcf_c = 0; run_ms(100); pcf_r = -1; run_ms(100);
    check("I2C keypad: 2 D 7", !strcmp(got, "2D7"));
    pcf_here = 0; run_ms(100); pcf_here = 1;
    check("I2C keypad missing: no keys, no hang", !strcmp(got, "2D7") && kp_err > 0);
    setline("keypad off");
    /* RC522 */
    check("rfid rc522 25 accepted", setline("rfid rc522 25") == 0);
    check("rfid rc522 on an SPI pin refused", setline("rfid rc522 9") != 0);
    setline("rfid rc522 25"); access_setup();
    run_ms(600);
    check("RC522 found (version 0x92)", rc_ver == 0x92);
    card = 1; halted = 0; gcard[0] = 0; run_ms(600);
    check("4-byte card read: deadbeef", !strcmp(gcard, "deadbeef"));
    gcard[0] = 0; run_ms(3000);
    check("card left on the reader: read once (HLTA)", gcard[0] == 0);
    card = 0; run_ms(500); card = 2; halted = 0; run_ms(600);
    check("7-byte card read: 04112233445566", !strcmp(gcard, "04112233445566"));
    card = 0;
    /* Wiegand */
    check("rfid wiegand 22 27 accepted", setline("rfid wiegand 22 27") == 0);
    access_setup(); gcard[0] = 0;
    /* 26 bit: facility 0x12, card 0x3456 -> data 0x123456; parity: first 12 data bits even, last 12 odd */
    u32 d = 0x123456; int pe = __builtin_popcount(d >> 12) & 1, po = !(__builtin_popcount(d & 0xFFF) & 1);
    u64 f = ((u64)pe << 25) | ((u64)d << 1) | (u64)po;
    wsend(f, 26);
    check("Wiegand 26: card 123456", !strcmp(gcard, "123456"));
    gcard[0] = 0; wsend(f ^ 4, 26);
    check("Wiegand 26 with a bad bit: refused", gcard[0] == 0 && wg_bad == 1);
    u32 d2 = 0xCAFE1234; pe = __builtin_popcount(d2 >> 16) & 1; po = !(__builtin_popcount(d2 & 0xFFFF) & 1);
    wsend(((u64)pe << 33) | ((u64)d2 << 1) | (u64)po, 34);
    check("Wiegand 34: card cafe1234", !strcmp(gcard, "cafe1234"));
    gn = 0; got[0] = 0;
    wsend(5, 4); wsend(0xE1, 8); wsend(11, 4); wsend(10, 4);
    printf("    got \"%s\", bad %u\n", got, wg_bad);
    check("Wiegand keys: 5 (4 bit), 1 (8 bit), #, *", !strcmp(got, "51#*"));
    access_text(t, sizeof(t));
    check("settings text has the wiegand line", strstr(t, "rfid wiegand 22 27") != 0);
    /* 0.145: rotary knob (6/13 quadrature, 19 push, 26 confirm, 21 back) */
    setline("rfid off"); setline("keypad off");
    for (int i = 0; i < 32; i++) knob_lvl[i] = 1;
    knob_test = 1;
    check("knob line accepted", setline("knob") == 0);
    access_setup(); gn = 0; got[0] = 0;
    int dg = -1;
    static const int cw[4][2] = { { 0, 1 }, { 0, 0 }, { 1, 0 }, { 1, 1 } };   /* A B from 11 */
    #define TURN(dir) for (int s = 0; s < 4; s++) { int j = s; \
        knob_lvl[KA] = dir > 0 ? cw[j][0] : cw[j][1]; knob_lvl[KB] = dir > 0 ? cw[j][1] : cw[j][0]; run_ms(2); }
    TURN(1); TURN(1); TURN(1);
    access_knob(&dg);
    int up = dg;
    TURN(-1);
    int kd2 = -1; access_knob(&kd2);
    printf("    after 3 turns one way: %d, one back: %d\n", up, kd2);
    check("knob: 3 detents = 3 steps, one back = 1 step", (up == 3 && kd2 == 2) || (up == 7 && kd2 == 8));
    knob_lvl[KP] = 0; run_ms(40); knob_lvl[KP] = 1; run_ms(40);      /* push = the digit */
    knob_lvl[KBK] = 0; run_ms(40); knob_lvl[KBK] = 1; run_ms(40);      /* back */
    knob_lvl[KP] = 0; run_ms(40); knob_lvl[KP] = 1; run_ms(40);
    knob_lvl[KOK] = 0; run_ms(40); knob_lvl[KOK] = 1; run_ms(40);      /* confirm */
    char want[8]; snprintf(want, sizeof(want), "%c<%c#", '0' + kd2, '0' + kd2);
    printf("    knob keys \"%s\" (want \"%s\")\n", got, want);
    check("knob: push / back / push / confirm -> digit < digit #", !strcmp(got, want));
    knob_lvl[KA] = 0; run_ms(2); knob_lvl[KA] = 1; run_ms(2);       /* a bounce on one line: no step */
    int d3 = -1; access_knob(&d3);
    check("knob: a single bounce is not a step", d3 == kd2);
    access_text(t, sizeof(t));
    check("settings text has the knob line", strstr(t, "knob 6 13 19 26 21") != 0);
    check("knob pins are taken", access_uses_pin(19) && access_uses_pin(21));
    printf(errors ? "%d WRONG\n" : "all OK\n", errors);
    return errors != 0;
}
