/*
 * PC test of src/radio.c with a fake RDA5807M: register writes on 0x11 (reset, tune, seek, volume),
 * sequential reads on 0x10 (0x0A..0x0F with STC, stereo, channel, RSSI and RDS groups).
 * build + run:  cc -O1 -w -o /tmp/test_radio tools/test_radio.c && /tmp/test_radio
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
static u64 now_us;
static u64 timer_us(void) { return now_us; }
static void delay_us(u64 us) { now_us += us; }
#define kprintf printf
static int parse_num(const char *s, u64 *o) { char *e; *o = strtoull(s, &e, 0); return e != s && !*e; }

/* ---- the fake chip ---- */
static u16 regs[16];
static int chan, stc, sf, resets;
static const struct { u32 f; int rssi, st; } stations[] = { { 9230, 35, 1 }, { 10440, 50, 1 }, { 10620, 28, 0 } };
#define NST 3
static u16 rds_q[64][4]; static int rds_n, rds_r;

static int station_at(int ch) { u32 f = 8700 + (u32)ch * 10; for (int i = 0; i < NST; i++) if (stations[i].f == f) return i; return -1; }

static int i2c_write(u8 a, const void *bv, int n)
{
    const u8 *b = bv;
    if (a != 0x11 || n != 3) return -1;
    u16 v = (u16)(b[1] << 8 | b[2]);
    int r = b[0];
    if (r == 2 && (v & 0x0002)) { resets++; memset(regs, 0, sizeof(regs)); return 0; }
    regs[r] = v;
    if (r == 3 && (v & 0x10)) { chan = v >> 6; stc = 1; sf = 0; }
    if (r == 2 && (v & 0x0100)) {                    /* seek */
        int up = (v & 0x0200) != 0, c = chan;
        sf = 1;
        for (int i = 0; i < 211; i++) {
            c = up ? (c + 1) % 211 : (c + 210) % 211;
            if (station_at(c) >= 0) { chan = c; sf = 0; break; }
        }
        stc = 1;
    }
    return 0;
}

static int i2c_read(u8 a, void *bv, int n)
{
    u8 *b = bv;
    if (a != 0x10) return -1;
    int s = station_at(chan);
    u16 o[6] = { 0 };
    int have = rds_r < rds_n;
    o[0] = (u16)((have ? 0x8000 : 0) | (stc ? 0x4000 : 0) | (sf ? 0x2000 : 0) | (s >= 0 && stations[s].st ? 0x0400 : 0) | chan);
    o[1] = (u16)(((s >= 0 ? stations[s].rssi : 5) << 9) | (s >= 0 ? 0x0100 : 0));
    if (have) { for (int i = 0; i < 4; i++) o[2 + i] = rds_q[rds_r][i]; rds_r++; }
    for (int i = 0; i < n / 2 && i < 6; i++) { b[2 * i] = (u8)(o[i] >> 8); b[2 * i + 1] = (u8)o[i]; }
    return 0;
}
static int i2c_probe(u8 a) { return a == 0x10 || a == 0x11; }

static void rds_ps(const char *ps)                   /* group 0A x 4 segments */
{
    for (int seg = 0; seg < 4; seg++) {
        u16 *g = rds_q[rds_n++];
        g[0] = 0x2201; g[1] = (u16)(0x0000 | seg); g[2] = 0; g[3] = (u16)((u8)ps[seg * 2] << 8 | (u8)ps[seg * 2 + 1]);
    }
}
static void rds_rt(const char *t, int ab)            /* group 2A, 4 chars per segment */
{
    int len = (int)strlen(t);
    for (int seg = 0; seg * 4 < len; seg++) {
        char c[4]; for (int i = 0; i < 4; i++) c[i] = seg * 4 + i < len ? t[seg * 4 + i] : '\r';
        u16 *g = rds_q[rds_n++];
        g[0] = 0x2201; g[1] = (u16)(0x2000 | (ab << 4) | seg);
        g[2] = (u16)((u8)c[0] << 8 | (u8)c[1]); g[3] = (u16)((u8)c[2] << 8 | (u8)c[3]);
    }
}

#define RADIO_HOST_TEST
#include "../src/radio.c"

static int errors;
static void check(const char *what, int ok) { printf("  %-48s %s\n", what, ok ? "ok" : "WRONG"); if (!ok) errors++; }
static void cmd(const char *line)
{
    char b[64], *av[6]; int ac = 0; strcpy(b, line);
    for (char *p = strtok(b, " "); p && ac < 6; p = strtok(0, " ")) av[ac++] = p;
    printf("> %s\n", line);
    radio_cmd(ac, av);
}
static void poll_all(void) { for (int i = 0; i < 200; i++) { now_us += 40000; radio_poll(); } }

int main(void)
{
    printf("frequency parsing:\n");
    check("104.4 -> 10440", parse_freq("104.4") == 10440);
    check("104,4 -> 10440", parse_freq("104,4") == 10440);
    check("1044 -> 10440", parse_freq("1044") == 10440);
    check("87 -> 8700", parse_freq("87") == 8700);
    check("108.1 refused", parse_freq("108.1") == 0);
    check("abc refused", parse_freq("abc") == 0);

    printf("tune + status:\n");
    cmd("radio 104.4");
    check("soft reset sent", resets == 1);
    check("reg 2: enable + unmute + RDS + DHIZ", (regs[2] & 0xC009) == 0xC009);
    check("reg 3: channel 174 + TUNE", (regs[3] >> 6) == 174 && (regs[3] & 0x10));
    check("reg 4: de-emphasis 50 us", regs[4] & 0x0800);
    check("reg 5: volume 8", (regs[5] & 15) == 8);
    check("freq 104.40", radio_freq() == 10440);
    read_status();
    check("stereo, RSSI 50", radio_stereo() && radio_rssi() == 50);

    printf("RDS:\n");
    rds_ps("RADIO  1"); rds_ps("RADIO  1"); rds_rt("Hello RoboOS", 0); rds_rt("Hello RoboOS", 0);
    poll_all();
    check("station name 'RADIO  1'", !memcmp(radio_ps(), "RADIO  1", 8));
    check("radio text 'Hello RoboOS'", !strcmp(radio_rt(), "Hello RoboOS"));
    rds_ps("XXXXXXXX");                               /* only once: not confirmed */
    poll_all();
    check("unconfirmed noise ignored", !memcmp(radio_ps(), "RADIO  1", 8));
    rds_rt("Next song", 1); rds_rt("Next song", 1);  /* A/B flag flips: text cleared */
    poll_all();
    check("new radio text after the A/B flip", !strcmp(radio_rt(), "Next song"));
    cmd("radio");

    printf("seek:\n");
    cmd("radio up");
    check("up from 104.4 -> 106.2", radio_freq() == 10620);
    check("RDS cleared on a new station", radio_ps()[0] == 0);
    cmd("radio up");
    check("up again wraps -> 92.3", radio_freq() == 9230);
    cmd("radio down");
    check("down -> 106.2", radio_freq() == 10620);

    printf("volume / off / network command:\n");
    cmd("radio vol 0");
    check("volume 0 = muted (no DMUTE)", !(regs[2] & 0x4000));
    cmd("radio vol 12");
    check("volume 12, unmuted", (regs[5] & 15) == 12 && (regs[2] & 0x4000));
    radio_net("104.4", 5); radio_poll();
    check("network command '104.4' tuned", radio_freq() == 10440);
    radio_net("off", 3); radio_poll();
    check("network 'off'", !radio_on() && regs[2] == 0);

    printf(errors ? "\n%d ERRORS\n" : "\nall OK\n", errors);
    return errors != 0;
}
