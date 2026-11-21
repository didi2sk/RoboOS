/*
 * dcf.c - DCF77 time signal receiver (0.74): Mainflingen, 77.5 kHz, the official German time.
 *
 * A receiver module (ELV DCF-2, Pollin, Canaduino, "DCF77 module" with a ferrite antenna) gives a
 * digital output: once a second the carrier drops for 100 ms (= bit 0) or 200 ms (= bit 1); second 59
 * has no pulse, so a gap of ~1.8 s marks the start of the next minute. 59 bits = minute, hour, day,
 * weekday, month, year (BCD with 3 even parity bits), bit 17/18 = summer / winter time (CEST / CET).
 *
 * The output pin is read by core 1 (dcf_poll from io_poll_all, every ~0.2 ms): a 15 ms glitch filter,
 * pulse lengths, polarity found by itself (some modules give high pulses, some low - the level whose
 * periods are 40-260 ms long is the pulse), then a minute frame is decoded at each minute mark.
 * Two frames in a row that agree (1 minute apart) set the clock (clock_how 4) and the battery clock -
 * unless NTP set it less than 2 hours ago (then the DCF time is only compared).
 *
 * /DCF.TXT (core 0 at boot, written by 'dcf ...'):  pin 17 / invert auto|on|off / pull up|down|off /
 * pon 4 (some modules have a PON input: low = receiver on) / state on|off ('dcf on' / 'dcf off').
 */
#include "kernel.h"
#ifndef DCF_HOST_TEST
#include "sd.h"
#endif

static int pin = -1, pon = -1, pull = 2, invert = -1;   /* invert -1 = auto */
static volatile int on = 1;                             /* 0.75: dcf off keeps the pin, dcf on resumes */
static volatile int pol = -1;                           /* the pulse level once known */
static int stable = -1, raw_prev = -1;
static u64 pend_at, edge_at, pulse_at, last_pulse;
static int nbits;
static u8 bits[60];
static u32 shortc[2];                                   /* level periods 40-260 ms long, per level */

/* what 'dcf' shows */
static volatile u32 n_edges, n_pulses, n_bad, n_frames, n_bad_frames, n_set;
static volatile int last_len[8], last_lens;
static volatile u64 last_time, last_time_at, set_at;   /* last decoded frame (UTC) + when */
static volatile int last_cest;
static const char *volatile last_err = "";
static char bitstr[61];
static volatile u64 prev_frame;                         /* the frame before, for the 2-in-a-row check */
static volatile int synced_ok;

static int bcd(const u8 *b, int from, int n)
{
    static const int w[8] = { 1, 2, 4, 8, 10, 20, 40, 80 };
    int v = 0;
    for (int i = 0; i < n; i++) if (b[from + i]) v += w[i];
    return v;
}
static int even(const u8 *b, int from, int to) { int p = 0; for (int i = from; i <= to; i++) p ^= b[i]; return p == 0; }

/* 59 bits -> UTC seconds of the minute that starts now, 0 = bad (last_err says why) */
static u64 decode(const u8 *b, int *cest)
{
    if (b[0] != 0) { last_err = "bit 0 not 0"; return 0; }
    if (b[20] != 1) { last_err = "bit 20 not 1"; return 0; }
    if (b[17] == b[18]) { last_err = "CET/CEST bits"; return 0; }
    if (!even(b, 21, 28)) { last_err = "minute parity"; return 0; }
    if (!even(b, 29, 35)) { last_err = "hour parity"; return 0; }
    if (!even(b, 36, 58)) { last_err = "date parity"; return 0; }
    int mi = bcd(b, 21, 7), h = bcd(b, 29, 6), d = bcd(b, 36, 6), wd = bcd(b, 42, 3), mo = bcd(b, 45, 5), y = bcd(b, 50, 8);
    if (mi > 59 || h > 23 || d < 1 || d > 31 || wd < 1 || wd > 7 || mo < 1 || mo > 12 || y > 99) { last_err = "value out of range"; return 0; }
    *cest = b[17];
    return clock_make(2000 + y, mo, d, h, mi, 0) - (u64)(b[17] ? 7200 : 3600);
}

static void minute_mark(u64 t_us)
{
    int n = nbits;
    for (int i = 0; i < n && i < 60; i++) bitstr[i] = (char)('0' + bits[i]);
    bitstr[n < 60 ? n : 60] = 0;
    nbits = 0;
    if (n != 59) {
        if (n) { n_bad_frames++; last_err = n < 59 ? "bits missing (weak signal / noise)" : "too many bits (noise)"; }
        prev_frame = 0;
        return;
    }
    int cest;
    u64 t = decode(bits, &cest);
    if (!t) { n_bad_frames++; prev_frame = 0; return; }
    n_frames++;
    last_time = t; last_time_at = t_us; last_cest = cest; last_err = "";
    int agree = prev_frame && t == prev_frame + 60;
    prev_frame = t;
    if (!agree) return;
    synced_ok = 1;
    u64 now_t = t + (timer_us() - t_us) / 1000000;
    if (clock_how() == 1 && clock_synced_ago() < 7200) return;   /* NTP is fresh: only compare */
#ifndef DCF_HOST_TEST
    clock_set_time(now_t, 4);
    if (!set_at || timer_us() - set_at > 3600ULL * 1000000) rtc_write(now_t);   /* battery clock: hourly */
#else
    clock_set_time(now_t, 4);
#endif
    set_at = timer_us();
    n_set++;
}

/* one clean level change: level lv starts at time t (us) */
static void edge(int lv, u64 t)
{
    u64 len_ms = edge_at ? (t - edge_at) / 1000 : 0;
    int old = lv ^ 1;
    edge_at = t;
    n_edges++;
    if (len_ms >= 40 && len_ms <= 260 && shortc[old] < 1000000) shortc[old]++;
    if (invert >= 0) pol = invert ? 0 : 1;
    else if (pol < 0 && shortc[0] + shortc[1] >= 4) {
        if (shortc[1] >= 3 * shortc[0] + 2) pol = 1;
        else if (shortc[0] >= 3 * shortc[1] + 2) pol = 0;
    }
    if (pol < 0) return;
    if (lv == pol) {                                    /* a pulse starts */
        if (last_pulse) {
            u64 gap = (t - last_pulse) / 1000;
            if (gap > 1500 && gap < 2300) minute_mark(t);
            else if (gap >= 2300) { nbits = 0; prev_frame = 0; last_err = "signal lost"; }
            else if (gap < 850) { n_bad++; last_err = "pulse too early (noise)"; return; }
        }
        pulse_at = t;
        last_pulse = t;
    } else if (pulse_at) {                              /* a pulse ends */
        u64 len = (t - pulse_at) / 1000;
        pulse_at = 0;
        n_pulses++;
        last_len[last_lens++ & 7] = (int)len;
        int bit = len >= 40 && len <= 140 ? 0 : len >= 150 && len <= 260 ? 1 : -1;
        if (bit < 0) { n_bad++; last_err = "pulse length wrong"; nbits = 60; return; }   /* spoils this minute */
        if (nbits < 60) bits[nbits++] = (u8)bit;
    }
}

/* the raw pin level at time t: 15 ms glitch filter, then edge() */
static void dcf_sample(int lv, u64 t)
{
    if (stable < 0) { stable = lv; edge_at = 0; return; }
    if (lv == stable) { pend_at = 0; return; }
    if (!pend_at || lv != raw_prev) pend_at = t;
    raw_prev = lv;
    if (t - pend_at >= 15000) { stable = lv; edge(lv, pend_at); pend_at = 0; }
}

#ifndef DCF_HOST_TEST
static u64 next_us;
void dcf_poll(void)                                     /* core 1 */
{
    if (pin < 0 || !on) return;
    u64 t = timer_us();
    if (t < next_us) return;
    next_us = t + 1000;                                 /* 1 ms is plenty */
    dcf_sample(gpio_get(pin) ? 1 : 0, t);
}

static void setup_pins(void)
{
    if (pin < 0) return;
    gpio_func(pin, 0);
    gpio_pull(pin, pull);
    if (pon >= 0) { gpio_func(pon, 1); gpio_set(pon, on ? 0 : 1); }   /* PON high = receiver powered down */
    stable = -1; pol = invert >= 0 ? (invert ? 0 : 1) : -1;
    shortc[0] = shortc[1] = 0; nbits = 0; last_pulse = pulse_at = 0; prev_frame = 0; synced_ok = 0;
}

static void save(void)
{
    char b[96]; int n = 0;
    const char *s;
    #define PUT(x) for (s = (x); *s; ) b[n++] = *s++
    #define NUM(v) do { int _v = (v); if (_v >= 10) b[n++] = (char)('0' + _v / 10); b[n++] = (char)('0' + _v % 10); } while (0)
    if (pin >= 0) { PUT("pin "); NUM(pin); b[n++] = '\n'; } else { PUT("pin none\n"); }
    PUT("invert "); PUT(invert < 0 ? "auto" : invert ? "on" : "off"); b[n++] = '\n';
    PUT("pull "); PUT(pull == 2 ? "up" : pull == 1 ? "down" : "off"); b[n++] = '\n';
    if (pon >= 0) { PUT("pon "); NUM(pon); b[n++] = '\n'; }
    PUT(on ? "state on\n" : "state off\n");
    #undef PUT
    #undef NUM
    if (fat_mounted()) fat_write_file("/DCF.TXT", b, (u32)n);
}

static int pin_arg(const char *s)
{
    u64 v;
    if (!parse_num(s, &v) || v > 27) return -1;         /* the 40-pin header: GPIO0-27 */
    return (int)v;
}

void dcf_boot(void)                                     /* core 0, before core 1 starts */
{
    struct fat_file f;
    char b[128];
    if (!fat_mounted() || fat_open("/DCF.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    for (char *l = b; *l; ) {
        char *e = l; while (*e && *e != '\n' && *e != '\r') e++;
        char sv = *e; *e = 0;
        if (!strncmp(l, "pin ", 4)) pin = pin_arg(l + 4);
        else if (!strncmp(l, "pon ", 4)) pon = pin_arg(l + 4);
        else if (!strncmp(l, "invert ", 7)) invert = !strcmp(l + 7, "on") ? 1 : !strcmp(l + 7, "off") ? 0 : -1;
        else if (!strncmp(l, "state ", 6)) on = strcmp(l + 6, "off") != 0;
        else if (!strncmp(l, "pull ", 5)) pull = !strcmp(l + 5, "up") ? 2 : !strcmp(l + 5, "down") ? 1 : 0;
        *e = sv; l = *e ? e + 1 : e; while (*l == '\n' || *l == '\r') l++;
    }
    setup_pins();
    if (pin >= 0 && on) kprintf("DCF77 receiver on GPIO%d (the time comes after ~2-3 minutes of good signal)\n", pin);
}

static void status(void)
{
    if (pin < 0) {
        kprintf("DCF77: off. 'dcf pin <gpio>' = the receiver's output pin (e.g. dcf pin 17)\n");
        return;
    }
    if (!on) { kprintf("DCF77: off (GPIO%d kept, 'dcf on' turns it on again)\n", pin); return; }
    u64 now = timer_us();
    kprintf("DCF77 on GPIO%d (pull %s", pin, pull == 2 ? "up" : pull == 1 ? "down" : "off");
    if (pon >= 0) kprintf(", PON GPIO%d low", pon);
    kprintf("), level now %d, pulses %s (%s)\n", gpio_get(pin) ? 1 : 0,
            pol < 0 ? "not found yet" : pol ? "HIGH" : "LOW", invert < 0 ? "auto" : "set by hand");
    kprintf("  level changes %u, pulses %u, bad %u, minutes ok %u / bad %u, clock set %u times\n",
            n_edges, n_pulses, n_bad, n_frames, n_bad_frames, n_set);
    kprintf("  last pulses (ms):");
    int k = last_lens < 8 ? last_lens : 8;
    for (int i = 0; i < k; i++) kprintf(" %d", last_len[(last_lens - k + i) & 7]);
    kprintf(k ? "   (100 = bit 0, 200 = bit 1)\n" : " none - no signal? check the pin, power, antenna\n");
    if (last_pulse) kprintf("  last pulse %u ms ago, bits this minute: %d\n", (u32)((now - last_pulse) / 1000), nbits > 59 ? 0 : nbits);
    if (bitstr[0]) kprintf("  last minute: %s\n", bitstr);
    if (last_time) {
        char b[32];
        clock_format(b, last_time, 0);
        kprintf("  last good time: %s (%s), %u s ago%s\n", b, last_cest ? "CEST" : "CET",
                (u32)((now - last_time_at) / 1000000), synced_ok ? "" : " - waiting for a 2nd minute to agree");
    }
    if (last_err[0]) kprintf("  last problem: %s\n", last_err);
    if (set_at) kprintf("  the clock was set from DCF77 %u s ago\n", (u32)((now - set_at) / 1000000));
    else if (synced_ok && clock_how() == 1) kprintf("  NTP is fresh, so DCF77 only checks it\n");
    kprintf("  tips: keep the antenna 1 m away from the Pi, monitors and chargers; the signal is best at night\n");
}

void dcf_cmd(int argc, char **argv)
{
    const char *a = argc > 1 ? argv[1] : "";
    if (!*a) { status(); return; }
    if (!strcmp(a, "pin") && argc > 2) {
        int p = !strcmp(argv[2], "none") ? -1 : pin_arg(argv[2]);
        if (p < 0 && strcmp(argv[2], "none")) { kprintf("pin: a GPIO number 0-27\n"); return; }
        pin = p;
        on = 1;
    }
    else if (!strcmp(a, "off")) on = 0;
    else if (!strcmp(a, "on")) {
        if (pin < 0) { kprintf("first 'dcf pin <gpio>' (the receiver's output pin)\n"); return; }
        on = 1;
    }
    else if (!strcmp(a, "pon") && argc > 2) pon = !strcmp(argv[2], "none") ? -1 : pin_arg(argv[2]);
    else if (!strcmp(a, "invert") && argc > 2) invert = !strcmp(argv[2], "on") ? 1 : !strcmp(argv[2], "off") ? 0 : -1;
    else if (!strcmp(a, "pull") && argc > 2) pull = !strcmp(argv[2], "up") ? 2 : !strcmp(argv[2], "down") ? 1 : 0;
    else {
        kprintf("usage: dcf                     status (signal, pulses, last time)\n"
                "       dcf on | off            start / stop reading the receiver (saved)\n"
                "       dcf pin <gpio>|none     the receiver's output pin (also turns it on)\n"
                "       dcf invert auto|on|off  pulse polarity (auto finds it)\n"
                "       dcf pull up|down|off    pull resistor (up for open-collector outputs)\n"
                "       dcf pon <gpio>|none     PON input of the module (held low = on)\n");
        return;
    }
    io_lock();                                          /* core 1 samples the pin */
    setup_pins();
    io_unlock();
    save();
    status();
}
#endif
