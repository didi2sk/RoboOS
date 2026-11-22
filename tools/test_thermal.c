/* PC test of src/thermal.c: a fake AMG8833 (pixels in 12-bit two's complement, thermistor in sign +
 * magnitude), detection (an all-0 chip is refused), summary values, presence, the scaled heat map
 * (written to /tmp/thermal.ppm to look at).
 *   cc -O1 -w -o /tmp/test_thermal tools/test_thermal.c && /tmp/test_thermal */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define KERNEL_H
#define THERMAL_HOST_TEST
struct thermal_values { int pix[64], min_c100, max_c100, avg_c100, median_c100, therm_c100, warm, present; };
static u64 now_us; static u64 timer_us(void) { return now_us; } static void delay_us(u64 u) { now_us += u; }
#define kprintf printf
static int parse_num(const char *s, u64 *o) { char *e; *o = strtoull(s, &e, 0); return e != s && !*e; }
static int io_held(void) { return 0; }
static int motion_present(void) { return 0; } static int motion_addr(void) { return 0; } static int rtc_present(void) { return 0; }
static int chip = 1, dead;
static int fake_q[64];                                    /* pixel values in 0.25 C */
static int fake_therm = 22 * 16;                          /* 0.0625 C */
static int i2c_write_reg(u8 a, u8 r, const void *b, int n) { return (chip && a == 0x69) ? 0 : -1; }
static int i2c_read_reg(u8 a, u8 r, void *bv, int n)
{
    u8 *b = bv;
    if (!chip || a != 0x69) return -1;
    if (r == 0x0E) { int v = fake_therm < 0 ? -fake_therm : fake_therm; b[0] = (u8)v; b[1] = (u8)((v >> 8) & 7) | (fake_therm < 0 ? 8 : 0); return 0; }
    if (r >= 0x80) for (int i = 0; i < n; i++) { int k = (r - 0x80 + i) / 2; int v = dead ? 0 : fake_q[k] & 0xFFF; b[i] = (u8)((r - 0x80 + i) & 1 ? v >> 8 : v); }
    return 0;
}
static u32 fbuf[600][800];
static u32 *fb_row(int y) { return fbuf[y]; }
static void fb_text_px(int x, int y, const char *s, u32 fg, u32 bg) {}
#include "../src/thermal.c"
static int errors; static void check(const char *w, int ok) { printf("  %-50s %s\n", w, ok ? "ok" : "WRONG"); if (!ok) errors++; }
static void scene(int person)
{
    for (int i = 0; i < 64; i++) fake_q[i] = 22 * 4 + (i % 3) - 1;      /* ~22 C background with a little noise */
    if (person) { fake_q[27] = 33 * 4; fake_q[28] = 34 * 4; fake_q[35] = 32 * 4; fake_q[36] = 31 * 4; }
    fake_q[63] = -3 * 4 - 1;                               /* -3.25 C: a negative pixel */
    last_read = 0;
}
int main(void)
{
    dead = 1; scene(0); thermal_boot();
    check("an all-0 chip is not taken", !thermal_present());
    dead = 0; thermal_boot();
    check("found at 0x69", thermal_present() && addr == 0x69);
    struct thermal_values t;
    thermal_get(&t);
    check("thermistor 22.00 C", t.therm_c100 == 2200);
    check("negative pixel -3.25 C", t.pix[63] == -325 && t.min_c100 == -325);
    check("no person on a flat scene", t.warm < 2);
    scene(1); thermal_get(&t);
    check("hottest 34.00 C", t.max_c100 == 3400);
    check("person: 4 warm pixels", t.warm == 4);
    for (int i = 0; i < 4; i++) { now_us += 500000; last_read = 0; thermal_poll(); }
    check("presence on", present == 1);
    scene(0); for (int i = 0; i < 4; i++) { now_us += 500000; last_read = 0; thermal_poll(); }
    check("presence off again", present == 0 && n_present == 1);
    fake_therm = -5 * 16; last_read = 0; thermal_get(&t);
    check("negative thermistor -5.00 C", t.therm_c100 == -500);
    check("palette ends: black / white", heat(0) == 0 && heat(1023) == 0xFFFFFF);
    scene(1); last_read = 0; thermal_get(&t);
    draw_map(&t, 10, 10, 400);
    FILE *f = fopen("/tmp/thermal.ppm", "wb"); fprintf(f, "P6 420 460 255\n");
    for (int y = 0; y < 460; y++) for (int x = 0; x < 420; x++) { u32 p = fbuf[y][x]; fputc(p >> 16, f); fputc(p >> 8 & 255, f); fputc(p & 255, f); } fclose(f);
    u32 centre = fbuf[10 + 200][10 + 200], corner = fbuf[10 + 5][10 + 5];
    check("map: the hot centre is brighter than a corner", ((centre >> 16) + (centre >> 8 & 255)) > ((corner >> 16) + (corner >> 8 & 255)));
    printf(errors ? "\n%d ERRORS\n" : "\nall OK\n", errors);
    return errors != 0;
}
