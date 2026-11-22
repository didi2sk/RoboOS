/* PC test of src/motion.c: integer atan2 against libm, tilt from a fake MPU-6050 at known angles,
 * gyro / temperature scaling, shake detection + its 1 s hold-off.
 *   cc -O1 -w -o /tmp/test_motion tools/test_motion.c -lm && /tmp/test_motion */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define KERNEL_H
#define MOTION_HOST_TEST
struct motion_values { int ax_mg, ay_mg, az_mg, g_mg, gx_d10, gy_d10, gz_d10, temp_c100, pitch_d10, roll_d10; u32 shakes; };
static u64 now_us; static u64 timer_us(void) { return now_us; } static void delay_us(u64 u) { now_us += u; }
#define kprintf printf
static int parse_num(const char *s, u64 *o) { char *e; *o = strtoull(s, &e, 0); return e != s && !*e; }
static int io_held(void) { return 0; }
static short reg_a[3], reg_g[3], reg_t; static int woke;
static int i2c_read_reg(u8 a, u8 r, void *bv, int n)
{
    u8 *b = bv;
    if (a != 0x68) return -1;
    if (r == 0x75) { b[0] = 0x68; return 0; }
    if (r == 0x6B) { b[0] = woke ? 0x00 : 0x40; return 0; }
    if (r == 0x3B && n == 14 && !woke) { memset(b, 0, 14); return 0; }   /* asleep: all 0 */
    if (r == 0x3B && n == 14) {
        short v[7] = { reg_a[0], reg_a[1], reg_a[2], reg_t, reg_g[0], reg_g[1], reg_g[2] };
        for (int i = 0; i < 7; i++) { b[2 * i] = (u8)((uint16_t)v[i] >> 8); b[2 * i + 1] = (u8)v[i]; }
        return 0;
    }
    return -1;
}
static int sleepy;                                         /* the chip ignores the first wake-up writes */
static int i2c_write_reg(u8 a, u8 r, const void *b, int n) { if (a != 0x68) return -1; if (r == 0x6B && *(const u8 *)b == 0) { if (sleepy) sleepy--; else woke = 1; } return 0; }
#include "../src/motion.c"
static int errors; static void check(const char *w, int ok) { printf("  %-50s %s\n", w, ok ? "ok" : "WRONG"); if (!ok) errors++; }
static void tilt(double pitch, double roll)               /* gravity for this pitch / roll, 16384 = 1 g */
{
    double p = pitch * M_PI / 180, r = roll * M_PI / 180;
    reg_a[0] = (short)lround(-sin(p) * 16384); reg_a[1] = (short)lround(cos(p) * sin(r) * 16384); reg_a[2] = (short)lround(cos(p) * cos(r) * 16384);
    last_read = 0;
}
int main(void)
{
    int worst = 0;
    for (int y = -2000; y <= 2000; y += 37) for (int x = -2000; x <= 2000; x += 41) {
        if (!x && !y) continue;
        int got = iatan2(y, x), want = (int)lround(atan2(y, x) * 1800 / M_PI);
        int d = abs(got - want); if (d > 1800) d = 3600 - d;
        if (d > worst) worst = d;
    }
    printf("atan2 worst error %d.%d deg\n", worst / 10, worst % 10);
    check("integer atan2 within 0.2 deg", worst <= 2);
    sleepy = 1;                                            /* needs the retry in wake() */
    motion_boot();
    check("found at 0x68, woken (after a retry)", addr == 0x68 && woke);
    struct motion_values m;
    double cases[][2] = { { 0, 0 }, { 30, 0 }, { -45, 0 }, { 0, 20 }, { 0, -60 }, { 15, 25 }, { 80, 0 } };
    for (int i = 0; i < 7; i++) {
        tilt(cases[i][0], cases[i][1]); motion_get(&m);
        char w[64]; snprintf(w, sizeof w, "pitch %.0f roll %.0f -> %d.%d / %d.%d", cases[i][0], cases[i][1], m.pitch_d10 / 10, abs(m.pitch_d10 % 10), m.roll_d10 / 10, abs(m.roll_d10 % 10));
        check(w, abs(m.pitch_d10 - (int)(cases[i][0] * 10)) <= 3 && abs(m.roll_d10 - (int)(cases[i][1] * 10)) <= 3);
    }
    tilt(0, 0); reg_g[0] = 131 * 90; reg_t = (short)((25.0 - 36.53) * 340); motion_get(&m);
    check("gyro 90 deg/s -> 900", m.gx_d10 == 900);
    check("temperature 25 C (+-0.1)", abs(m.temp_c100 - 2500) <= 10);
    check("|a| 1000 mg at rest", abs(m.g_mg - 1000) <= 5);
    reg_g[0] = 0;
    for (int i = 0; i < 40; i++) { now_us += 50000; motion_poll(); }
    check("no shake at rest", shakes == 0);
    reg_a[2] = (short)(16384 * 1.8); last_read = 0; now_us += 50000; motion_poll();
    check("1.8 g -> one shake", shakes == 1);
    for (int i = 0; i < 5; i++) { now_us += 50000; last_read = 0; motion_poll(); }
    check("held 1 s: still one", shakes == 1);
    now_us += 1000000; last_read = 0; motion_poll();
    check("after 1 s: two", shakes == 2);
    woke = 0; sleepy = 1000; u32 before = shakes; last_read = 0;
    for (int i = 0; i < 40; i++) { now_us += 50000; last_read = 0; motion_poll(); }
    check("asleep (all 0): no data, no shakes counted", motion_get(&m) == -2 && shakes == before);
    sleepy = 0; now_us += 1100000; last_read = 0; motion_get(&m); last_read = 0;
    check("woken again by the next read", woke && motion_get(&m) == 0);
    printf(errors ? "\n%d ERRORS\n" : "\nall OK\n", errors);
    return errors != 0;
}
