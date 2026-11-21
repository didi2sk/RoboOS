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
static u64 pend_at, edge_at;
static volatile int nbits;
static int pos_known;                                   /* 0.77: the last minute had 59 bits */
static u8 bits[60];
/* 0.76: how long each level lasted - for 'dcf' (a photo shows at once whether there is a 1 s rhythm) */
static volatile u32 hist[2][7];                         /* <40, 40-140, 141-260, 261-699, 700-1000, 1001-2100, >2100 ms */
static volatile u16 per_ms[12]; static volatile u8 per_lv[12]; static volatile u32 per_n;

/* what 'dcf' shows */
static volatile u32 n_edges, n_pulses, n_bad, n_frames, n_bad_frames, n_set;
static volatile u64 last_time, last_time_at, set_at;   /* last decoded frame (UTC) + when */
static volatile int last_cest;
static const char *volatile last_err = "";
static char bitstr[61];
static volatile u64 prev_frame, prev_at;                         /* the frame before, for the 2-in-a-row check */
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

/* a sure time t (UTC) for the minute that started at t_us: set the clock */
static void set_from(u64 t, u64 t_us)
{
    synced_ok = 1;
    if (set_at && timer_us() - set_at < 30000000) return;        /* both decoders agreed on this minute */
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

static void minute_mark(u64 t_us)
{
    int n = nbits;
    for (int i = 0; i < n && i < 60; i++) bitstr[i] = (char)('0' + bits[i]);
    bitstr[n < 60 ? n : 60] = 0;
    nbits = 0;
    pos_known = n == 59;
    if (n != 59) {
        if (n) { n_bad_frames++; last_err = n < 59 ? "bits missing (weak signal / noise)" : "too many bits (noise)"; }
        return;
    }
    int cest;
    u64 t = decode(bits, &cest);
    if (!t) { n_bad_frames++; return; }
    n_frames++;
    last_time = t; last_time_at = t_us; last_cest = cest; last_err = "";
    /* 0.77: two good minutes that fit together (bad ones in between are fine, up to 30 minutes apart) */
    u64 mins = prev_frame ? (t_us - prev_at + 30000000) / 60000000 : 0;
    int agree = prev_frame && mins >= 1 && mins <= 30 && t == prev_frame + 60 * mins;
    prev_frame = t; prev_at = t_us;
    if (!agree) return;
    set_from(t, t_us);
}

/* one clean level change: level lv starts at time t (us) */
static void edge(int lv, u64 t)
{
    u64 len_ms = edge_at ? (t - edge_at) / 1000 : 0;
    static int edge_at_prev_ok;
    if (!edge_at) edge_at_prev_ok = 0;
    int old = lv ^ 1;
    edge_at = t;
    n_edges++;
    if (edge_at_prev_ok) {
        int b = len_ms < 40 ? 0 : len_ms <= 140 ? 1 : len_ms <= 260 ? 2 : len_ms < 700 ? 3 : len_ms <= 1000 ? 4 : len_ms <= 2100 ? 5 : 6;
        hist[old][b]++;
        per_ms[per_n % 12] = (u16)(len_ms > 65000 ? 65000 : len_ms); per_lv[per_n % 12] = (u8)old; per_n++;
    }
    edge_at_prev_ok = 1;
}

/* ---- 0.77: phase-locked decoder (noise tolerant) ----
 * The owner's first module, next to the Pi + monitor, gave ~5 level changes a second with many spikes
 * < 40 ms: an edge-based decoder never saw a clean pulse. But the 1 s rhythm is still there, so:
 * the pin is averaged in 10 ms slots, every slot of the second (100) keeps a slow average, and the phase
 * where 0-100 ms differs most from 200-1000 ms is the pulse start (its sign = the pulse level). Then each
 * second is judged as a whole: pulse level in 0-100 ms most of the time = a pulse (else second 59 = the
 * minute mark), pulse level in 100-200 ms more than half the time = bit 1. */
static u64 t_base, slot_now = (u64)-1;
static u32 slot_hi, slot_n;
static u8 frac_ring[200];                               /* last 2 s of 10 ms slots: high fraction 0-255 */
static u32 avg[100];                                    /* slow average per slot of the second (x16) */
static volatile int phase = -1, lock_q, lock_pol = -1;  /* pulse start slot, quality %, pulse level */
static u32 seconds_seen;
static volatile u32 n_secs;
static volatile u8 sec_w0[8], sec_w1[8]; static volatile u32 sec_n;   /* last seconds: % pulse level */
static u64 mark_due;                                    /* the minute starts then (after a missing pulse) */

/* ---- 0.78: minute lock + soft decoding over several minutes ----
 * At ~25 % lock quality (owner, 3B+ next to the monitor) nearly every minute had a wrong bit. But the
 * minute mark comes at the same place every 60 judged seconds, the hour / date / CET bits stay the same and
 * the minute only goes up by one: so every second's "how much like a 1" (w1 - 50) is summed over minutes.
 *   mavg[60]  pulse presence per second of the minute -> the lowest one is second 59 (minute lock)
 *   msc[60]   score of every possible minute value, shifted by one each minute (the minute goes +1)
 *   hsc[24]   the same for the hour (shifted when the minute wraps to 0)
 *   dacc[23]  soft sum of bits 36-58 (date), cacc[2] bits 17/18 (CEST/CET)
 * A result is used when the best minute and hour clearly beat the second best, the date bits are sure
 * and their parity is right, and the next minute's result is exactly 60 s later. */
static u32 sec_idx;                                     /* judged seconds since the lock */
static u8 ring_w1[60];                                  /* w1 of the last 60 judged seconds */
static u32 mavg[60];                                    /* x16, w0 per second-of-minute */
static volatile int mlock_k = -1, mlock_c;              /* second 59 = sec_idx % 60 == mlock_k; contrast */
static u8 pend_soft[59]; static int pend_have;
static long msc[60], hsc[24], dacc[23], cacc[2];
static volatile int soft_n, soft_mm = -1, soft_hh = -1, soft_mmargin, soft_hmargin, soft_dok;
static u64 prev_soft, prev_soft_at;
static volatile u32 n_soft;

static void minute_lock_update(u32 k, u32 w0)
{
    mavg[k] = mavg[k] - mavg[k] / 4 + w0 * 4;           /* x16 steady state, ~4 minutes memory */
    int lo = 0; for (int i = 1; i < 60; i++) if (mavg[i] < mavg[lo]) lo = i;
    u32 rest = 0; for (int i = 0; i < 60; i++) if (i != lo) rest += mavg[i];
    rest /= 59;
    mlock_c = (int)(rest > mavg[lo] ? (rest - mavg[lo]) / 16 : 0);
    if (mavg[lo] / 16 < 45 && mlock_c >= 25) mlock_k = lo;
    else if (mlock_c < 12) mlock_k = -1;
}

static int pbit(int v, int n, int j)                    /* bit j of the BCD code of v (n bits) */
{
    static const int w[8] = { 1, 2, 4, 8, 10, 20, 40, 80 };
    int b[8] = { 0 }, x = v;
    for (int i = n - 1; i >= 0; i--) if (x >= w[i]) { b[i] = 1; x -= w[i]; }
    if (j < n) return b[j];
    int par = 0; for (int i = 0; i < n; i++) par ^= b[i];
    return par;                                         /* j == n: the even parity bit */
}

static long corr(const int *sv, int from, int v, int n)  /* how well the n bits + parity at from fit v */
{
    long c = 0;
    for (int j = 0; j <= n; j++) c += pbit(v, n, j) ? sv[from + j] : -sv[from + j];
    return c;
}

static void soft_minute(const u8 *w1s, u64 t_us)
{
    int sv[59];
    for (int i = 0; i < 59; i++) sv[i] = (int)w1s[i] - 50;
    long nm[60];
    for (int m = 0; m < 60; m++) nm[m] = msc[(m + 59) % 60] * 7 / 8 + corr(sv, 21, m, 7);
    for (int m = 0; m < 60; m++) msc[m] = nm[m];
    int bm = 0; for (int m = 1; m < 60; m++) if (msc[m] > msc[bm]) bm = m;
    if (bm == 0) { long nh[24]; for (int h = 0; h < 24; h++) nh[h] = hsc[(h + 23) % 24]; for (int h = 0; h < 24; h++) hsc[h] = nh[h]; }
    for (int h = 0; h < 24; h++) hsc[h] = hsc[h] * 7 / 8 + corr(sv, 29, h, 6);
    int bh = 0; for (int h = 1; h < 24; h++) if (hsc[h] > hsc[bh]) bh = h;
    if (bm == 0 && bh == 0) for (int i = 0; i < 23; i++) dacc[i] = 0;   /* midnight: a new date */
    for (int i = 0; i < 23; i++) dacc[i] = dacc[i] * 7 / 8 + sv[36 + i];
    cacc[0] = cacc[0] * 7 / 8 + sv[17]; cacc[1] = cacc[1] * 7 / 8 + sv[18];
    soft_n++;
    long m2 = -1000000, h2 = -1000000;
    for (int m = 0; m < 60; m++) if (m != bm && msc[m] > m2) m2 = msc[m];
    for (int h = 0; h < 24; h++) if (h != bh && hsc[h] > h2) h2 = hsc[h];
    soft_mm = bm; soft_hh = bh;
    soft_mmargin = (int)(msc[bm] - m2); soft_hmargin = (int)(hsc[bh] - h2);
    u8 db[59]; int sure = 1;
    for (int i = 0; i < 23; i++) { db[36 + i] = dacc[i] > 0; if ((dacc[i] < 0 ? -dacc[i] : dacc[i]) < 60) sure = 0; }
    soft_dok = sure && even(db, 36, 58);
    if (soft_mmargin < 80 || soft_hmargin < 80 || !soft_dok || cacc[0] == cacc[1]) return;
    int d = bcd(db, 36, 6), wd = bcd(db, 42, 3), mo = bcd(db, 45, 5), y = bcd(db, 50, 8);
    if (d < 1 || d > 31 || wd < 1 || wd > 7 || mo < 1 || mo > 12 || y > 99) { soft_dok = 0; return; }
    int cest = cacc[0] > cacc[1];
    u64 t = clock_make(2000 + y, mo, d, bh, bm, 0) - (u64)(cest ? 7200 : 3600);
    u64 mins = prev_soft ? (t_us - prev_soft_at + 30000000) / 60000000 : 0;
    int agree = prev_soft && mins >= 1 && mins <= 30 && t == prev_soft + 60 * mins;
    prev_soft = t; prev_soft_at = t_us;
    if (!agree) return;
    n_soft++;
    last_time = t; last_time_at = t_us; last_cest = cest;
    set_from(t, t_us);
}

static void find_phase(void)
{
    int best_p = -1; long best = 0;
    for (int p = 0; p < 100; p++) {
        long a = 0, b = 0;
        for (int j = 0; j < 10; j++) a += avg[(p + j) % 100];
        for (int j = 20; j < 100; j++) b += avg[(p + j) % 100];
        long c = a * 8 - b;                             /* = 80 * (mean 0-100 ms - mean 200-1000 ms) */
        if (invert == 0 && c < 0) continue;
        if (invert == 1 && c > 0) continue;
        if ((c < 0 ? -c : c) > (best < 0 ? -best : best)) { best = c; best_p = p; }
    }
    long full = 80L * 16 * 255;                         /* the biggest possible difference */
    lock_q = (int)((best < 0 ? -best : best) * 100 / full);
    if (best_p >= 0 && lock_q >= 20 && seconds_seen >= 8) {
        if (phase >= 0 && best_p != phase && lock_pol == (best > 0)) {
            int d = (best_p - phase + 100) % 100;       /* small drifts follow; a jump needs a clearly better phase */
            if (d > 2 && d < 98) { long a = 0, b = 0;
                for (int j = 0; j < 10; j++) a += avg[(phase + j) % 100];
                for (int j = 20; j < 100; j++) b += avg[(phase + j) % 100];
                long cur = a * 8 - b; if (cur < 0) cur = -cur;
                if ((best < 0 ? -best : best) < cur * 5 / 4) return; }
        }
        phase = best_p; lock_pol = best > 0;
        pol = lock_pol;
    } else if (lock_q < 10) { phase = -1; lock_pol = -1; if (invert < 0) pol = -1; }
}

/* the second that started at slot s0 (absolute slot number) is complete enough: judge it */
static void judge_second(u64 s0)
{
    u32 w0 = 0, w1 = 0;
    for (int j = 0; j < 10; j++) {
        u32 f0 = frac_ring[(s0 + (u64)j) % 200], f1 = frac_ring[(s0 + 10 + (u64)j) % 200];
        if (!lock_pol) { f0 = 255 - f0; f1 = 255 - f1; }
        w0 += f0; w1 += f1;
    }
    w0 = w0 * 100 / 2550; w1 = w1 * 100 / 2550;        /* % of the time at the pulse level */
    sec_w0[sec_n & 7] = (u8)w0; sec_w1[sec_n & 7] = (u8)w1; sec_n++;
    n_secs++;
    u64 start_us = t_base + s0 * 10000;
    u32 k = sec_idx % 60;
    ring_w1[k] = (u8)w1;
    minute_lock_update(k, w0);
    sec_idx++;
    if (mlock_k >= 0) {                                 /* 0.78: we know which second is 59 */
        if ((int)k == mlock_k) {
            for (int i = 0; i < 59; i++) { pend_soft[i] = ring_w1[(k + 1 + (u32)i) % 60]; bits[i] = pend_soft[i] > 50; }
            nbits = 59; pend_have = 1;
            mark_due = start_us + 1000000;
            return;
        }
        if (w0 < 60) { n_bad++; last_err = "weak / noisy second"; }
        n_pulses++;
        return;                                         /* the bits are taken from ring_w1 at second 59 */
    }
    /* once a minute had exactly 59 bits we know where second 59 is: a noisy second elsewhere is not
     * taken for the minute mark, and second 59 is one even if a spike looks a bit like a pulse */
    int mark = pos_known ? (nbits >= 59 && w0 < 60) : (w0 < 35);
    if (mark) { mark_due = start_us + 1000000; return; }
    if (w0 < 35) { n_bad++; last_err = "pulse missing (noise)"; }
    if (w0 < 60) { n_bad++; last_err = "weak / noisy second"; }
    n_pulses++;
    if (nbits < 60) bits[nbits++] = (u8)(w1 > 50);
    else last_err = "too many bits (noise)";
}

static void slot_done(u64 slot)
{
    u32 f = slot_n ? slot_hi * 255 / slot_n : (slot ? frac_ring[(slot - 1) % 200] : 0);
    frac_ring[slot % 200] = (u8)f;
    u32 i = (u32)(slot % 100);
    avg[i] = avg[i] - avg[i] / 16 + f;                  /* ~16 s memory */
    if (i == 99) { seconds_seen++; find_phase(); }
    if (phase >= 0 && slot >= 100 && (u32)((slot + 100 - 25) % 100) == (u32)phase)
        judge_second(slot - 25);                        /* 250 ms after a pulse start: 0-200 ms are in */
}

static void phase_sample(int lv, u64 t)
{
    if (!t_base) t_base = t;
    u64 slot = (t - t_base) / 10000;
    if (slot != slot_now) {
        if (slot_now != (u64)-1) for (u64 s = slot_now; s < slot; s++) { slot_done(s); slot_hi = slot_n = 0; }
        slot_now = slot;
    }
    slot_hi += (u32)lv; slot_n++;
    if (mark_due && t >= mark_due) {
        u64 m = mark_due; mark_due = 0;
        int soft = pend_have; pend_have = 0;
        minute_mark(m);
        if (soft) soft_minute(pend_soft, m);
    }
}

static void dcf_reset_state(void)
{
    stable = -1; pol = invert >= 0 ? (invert ? 0 : 1) : -1;
    nbits = 0; per_n = 0; edge_at = 0; prev_frame = 0; synced_ok = 0;
    for (int i = 0; i < 7; i++) hist[0][i] = hist[1][i] = 0;
    t_base = 0; slot_now = (u64)-1; slot_hi = slot_n = 0; seconds_seen = 0; mark_due = 0;
    phase = -1; lock_pol = -1; lock_q = 0; pos_known = 0;
    sec_idx = 0; mlock_k = -1; mlock_c = 0; pend_have = 0; soft_n = 0; soft_mm = soft_hh = -1; prev_soft = 0;
    for (int i = 0; i < 60; i++) { mavg[i] = 50 * 16; msc[i] = 0; }
    for (int i = 0; i < 24; i++) hsc[i] = 0;
    for (int i = 0; i < 23; i++) dacc[i] = 0;
    cacc[0] = cacc[1] = 0;
    for (int i = 0; i < 100; i++) avg[i] = 0;
}

/* the raw pin level at time t: the phase decoder + (for the statistics) a 15 ms glitch filter, then edge() */
static void dcf_sample(int lv, u64 t)
{
    phase_sample(lv, t);
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
    dcf_reset_state();
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
    kprintf("), level now %d\n", gpio_get(pin) ? 1 : 0);
    if (phase >= 0) kprintf("  1 s rhythm LOCKED: quality %d%%, pulses %s (%s), seconds judged %u\n",
                            lock_q, lock_pol ? "HIGH" : "LOW", invert < 0 ? "auto" : "set by hand", n_secs);
    else kprintf("  1 s rhythm not found yet (quality %d%%, needs 20%% after 8 s)%s\n", lock_q,
                 lock_q < 5 ? " - no signal or only noise" : "");
    kprintf("  level changes %u, seconds with a pulse %u (noisy %u), minutes ok %u / bad %u, clock set %u times\n",
            n_edges, n_pulses, n_bad, n_frames, n_bad_frames, n_set);
    kprintf("  how long each level lasted (ms):  <40 40-140 141-260 261-699 700-1000 1001-2100 >2100\n");
    for (int l = 1; l >= 0; l--) {
        kprintf("    %s", l ? "HIGH" : "LOW ");
        kprintf(" %4u %7u %8u %8u %9u %10u %6u", hist[l][0], hist[l][1], hist[l][2], hist[l][3], hist[l][4], hist[l][5], hist[l][6]);
        kprintf("\n");
    }
    kprintf("  (a good signal: one level mostly 40-140/141-260, the other 700-1000 + a few 1001-2100;\n"
            "   many <40 = noise / interference)\n  last periods:");
    { u32 n = per_n, k2 = n < 12 ? n : 12;
      for (u32 i = 0; i < k2; i++) { u32 j = (n - k2 + i) % 12; kprintf(" %c%u", per_lv[j] ? 'H' : 'L', (u32)per_ms[j]); }
      kprintf(k2 ? "\n" : " none\n"); }
    if (sec_n) {
        u32 k = sec_n < 8 ? sec_n : 8;
        kprintf("  last seconds (%% at the pulse level in 0-100 / 100-200 ms):");
        for (u32 i = 0; i < k; i++) { u32 j = (sec_n - k + i) & 7; kprintf(" %u/%u", (u32)sec_w0[j], (u32)sec_w1[j]); }
        kprintf("\n  (a pulse = first > 60; second > 50 = bit 1; no pulse = the minute mark)\n");
    }
    if (mlock_k >= 0) kprintf("  minute found (contrast %d%%), %u minutes summed: best %02d:%02d (sure: minute %d, hour %d, date %s)\n",
                              mlock_c, (u32)soft_n, soft_hh, soft_mm, soft_mmargin, soft_hmargin, soft_dok ? "yes" : "not yet");
    else kprintf("  minute start not found yet (contrast %d%%, needs 25%%; takes 2-4 minutes)\n", mlock_c);
    kprintf("  bits this minute: %d\n", nbits > 59 ? 59 : nbits);
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
