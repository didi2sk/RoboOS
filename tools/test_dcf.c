/*
 * PC test of src/dcf.c: a synthetic DCF77 signal (CET and CEST minutes, both pulse polarities,
 * short glitches, a lost pulse) through the glitch filter + decoder; checks the time it sets.
 *   cc -O1 -w -o /tmp/test_dcf tools/test_dcf.c && /tmp/test_dcf
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define KERNEL_H
static u64 now_us;
static u64 timer_us(void) { return now_us; }
static long dfc(int y, int m, int d) { y -= m <= 2; long era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; return era * 146097 + (long)doe - 719468; }
static u64 clock_make(int y, int mo, int d, int h, int mi, int s) { return (u64)dfc(y, mo, d) * 86400 + h * 3600 + mi * 60 + s; }
static u64 set_t, set_count; static int set_by, how;
static void clock_set_time(u64 t, int by) { set_t = t; set_by = by; set_count++; }
static int clock_how(void) { return how; }
static u64 clock_synced_ago(void) { return 10; }
#define DCF_HOST_TEST
#include "../src/dcf.c"

static int level_for(int pulse, int p) { return pulse ? p : !p; }
static int noise_pml;                                    /* per-mille chance per ms to start a 3-35 ms spike */
static u32 rng = 12345;
static u32 rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 16; }
static int spike_left, spike_lv;
static void run(int lv, u64 ms)
{
    for (u64 i = 0; i < ms; i++) {
        now_us += 1000;
        int v = lv;
        if (noise_pml) {
            if (!spike_left && (int)(rnd() % 1000) < noise_pml) { spike_left = 3 + (int)(rnd() % 33); spike_lv = !lv; }
            if (spike_left) { v = spike_lv; spike_left--; }
        }
        dcf_sample(v, now_us);
    }
}

/* one minute of local time y-mo-d h:mi (cest), polarity p; glitch = add 5 ms spikes */
static void minute(int y, int mo, int d, int wd, int h, int mi, int cest, int p, int glitch, int drop_sec)
{
    u8 b[59]; memset(b, 0, sizeof(b));
    b[17] = (u8)cest; b[18] = (u8)!cest; b[20] = 1;
    static const int w[8] = { 1, 2, 4, 8, 10, 20, 40, 80 };
    #define PUT(v, from, n) do { int _v = (v); for (int k = (n) - 1; k >= 0; k--) if (_v >= w[k]) { b[(from) + k] = 1; _v -= w[k]; } } while (0)
    PUT(mi, 21, 7); PUT(h, 29, 6); PUT(d, 36, 6); PUT(wd, 42, 3); PUT(mo, 45, 5); PUT(y % 100, 50, 8);
    int par = 0; for (int i = 21; i <= 27; i++) par ^= b[i]; b[28] = (u8)par;
    par = 0; for (int i = 29; i <= 34; i++) par ^= b[i]; b[35] = (u8)par;
    par = 0; for (int i = 36; i <= 57; i++) par ^= b[i]; b[58] = (u8)par;
    for (int s = 0; s < 59; s++) {
        int len = b[s] ? 200 : 100;
        if (s == drop_sec) { run(level_for(0, p), 1000); continue; }
        run(level_for(1, p), (u64)len);
        if (glitch && s % 7 == 3) { run(level_for(0, p), 400); run(level_for(1, p), 5); run(level_for(0, p), (u64)(1000 - len - 405)); }
        else run(level_for(0, p), (u64)(1000 - len));
    }
    run(level_for(0, p), 1000);                          /* second 59: no pulse */
}

static int errors;
static void check(const char *what, int ok) { printf("  %-50s %s\n", what, ok ? "ok" : "WRONG"); if (!ok) errors++; }
static void reset_all(void) { dcf_reset_state(); set_count = 0; set_t = 0; n_frames = n_bad_frames = 0; }

int main(void)
{
    printf("CET, high pulses, auto polarity, glitches:\n");
    run(0, 3000);
    for (int m = 0; m < 4; m++) minute(2026, 11, 21, 6, 21, 37 + m, 0, 1, 1, -1);
    run(1, 100);                                           /* the minute mark: bits sent before it say 21:40 */
    check("polarity found = HIGH", pol == 1);
    check("minutes decoded", n_frames >= 3);
    check("clock set by DCF (how 4)", set_count >= 1 && set_by == 4);
    u64 want = clock_make(2026, 11, 21, 21 - 1, 40, 0);
    printf("  set to %llu, want ~%llu (diff %lld s)\n", (unsigned long long)set_t, (unsigned long long)want, (long long)(set_t - want));
    check("time = 20:40 UTC (+-1 s)", set_t + 1 >= want && set_t <= want + 1);

    printf("CEST, LOW pulses (inverted module), one lost pulse:\n");
    reset_all(); now_us += 5000000;
    run(1, 3000);
    minute(2026, 7, 4, 6, 12, 0, 1, 0, 0, 30);            /* this minute is spoiled */
    for (int m = 1; m < 4; m++) minute(2026, 7, 4, 6, 12, m, 1, 0, 0, -1);
    run(0, 100);
    check("polarity found = LOW", pol == 0);
    check("the minute with a lost pulse was refused", n_bad_frames >= 1);
    want = clock_make(2026, 7, 4, 10, 3, 0);
    check("time = 10:03 UTC (CEST - 2 h)", set_count && set_t + 1 >= want && set_t <= want + 1);

    printf("fresh NTP: DCF only compares:\n");
    reset_all(); how = 1; now_us += 5000000; run(0, 3000);
    for (int m = 0; m < 3; m++) minute(2026, 11, 21, 6, 22, m, 0, 1, 0, -1);
    run(1, 100);
    check("decoded but clock not touched", n_frames >= 2 && set_count == 0 && synced_ok);
    how = 0;

    printf("heavy interference (spikes 3-35 ms, ~3 per second, like the owner's photo):\n");
    reset_all(); now_us += 5000000; noise_pml = 3; set_count = 0;
    run(0, 3000);
    for (int m = 0; m < 5; m++) minute(2026, 11, 22, 7, 9, 10 + m, 0, 1, 0, -1);
    run(1, 100);
    noise_pml = 0;
    printf("  level changes %u, lock %d%%, minutes ok %u bad %u\n", n_edges, lock_q, n_frames, n_bad_frames);
    want = clock_make(2026, 11, 22, 8, 14, 0);
    check("time = 08:14 UTC despite the noise", set_count && set_t + 1 >= want && set_t <= want + 1);

    printf("noise only:\n");
    reset_all(); now_us += 5000000;
    for (int i = 0; i < 2000; i++) run((i * 7919) % 3 == 0, (u64)(3 + (i * 31) % 40));
    check("nothing set from noise", set_count == 0);

    printf(errors ? "\n%d ERRORS\n" : "\nall OK\n", errors);
    return errors != 0;
}
