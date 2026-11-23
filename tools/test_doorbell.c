/* PC test of src/doorbell.c: a fake HC-SR04 (+ radar pin) - someone walks from 3 m to the door and away; a
 * passer-by at 2 m; radar AND distance; no echo. cc -O1 -w -Isrc -o /tmp/test_doorbell tools/test_doorbell.c && /tmp/test_doorbell */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define KERNEL_H
#define SD_H
static u64 now_us;
static u64 timer_us(void) { return now_us++; }
static void delay_us(u64 u) { now_us += u; }
static int quiet;
static void kprintf(const char *f, ...) { if (quiet) return; va_list a; va_start(a, f); vprintf(f, a); va_end(a); }
enum { COL_CYAN, COL_DEFAULT }; static void con_color(int c) { (void)c; }
static int io_held(void) { return 0; }
/* GPIO: TRIG 23, ECHO 24, radar 17 */
static int trig_level, radar_level, person_cm = 300, wired = 1;
static u64 echo_from, echo_to;
static void gpio_func(int p, int f) {} static void gpio_pull(int p, int m) {}
static void gpio_set(int p, int on) { if (p == 23) { if (trig_level && !on && wired) { echo_from = now_us + 300; echo_to = echo_from + (u64)person_cm * 58; } trig_level = on; } }
static int gpio_get(int p) { if (p == 24) return now_us >= echo_from && now_us < echo_to; if (p == 17) return radar_level; return 1; }
static u64 clock_now(void) { return 0; } static int clock_format(char *b, u64 t, int i) { strcpy(b, "x"); return 1; }
static int photos, mqtt_ons, mqtt_offs;
static int camera_take(int save, int send, char *n) { photos++; if (n) strcpy(n, "/CAM/IMG.JPG"); return 0; }
static const char *camera_take_error(int r) { return "?"; }
static int mqtt_connected(void) { return 1; }
static int mqtt_publish(const char *t, const char *p, int l, int r) { if (!strcmp(t, "radar")) { if (p[1] == 'N') mqtt_ons++; else mqtt_offs++; } return 0; }
static int mqtt_photo_busy(void) { return 0; } static void mqtt_rediscover(void) {}
static int sound_wav(const char *p, int (*i)(void)) { return 0; } static int sound_play(const char *m, int (*i)(void), int *e) { return 0; }
static int path_abs(const char *a, char *b, int m) { strcpy(b, a); return 0; }
struct fat_file { int is_dir; };
static int fat_mounted(void) { return 0; } static int fat_open(const char *p, struct fat_file *f) { return -1; }
static int fat_read(struct fat_file *f, void *b, u32 l) { return -1; } static int fat_write_file(const char *p, const void *d, u32 l) { return 0; }
#include "../src/doorbell.c"
static void run_s(double s) { u64 end = now_us + (u64)(s * 1e6); while (now_us < end) { doorbell_poll(); now_us += 1000; } }
static int errors; static void check(const char *w, int ok) { printf("  %-55s %s\n", w, ok ? "ok" : "WRONG"); if (!ok) errors++; }
int main(void)
{
    char *a1[] = { "doorbell", "distance", "23", "24", "100" };
    quiet = 1; doorbell_cmd(5, a1); quiet = 0;
    char *a2[] = { "doorbell", "radar", "mqtt", "on" }; quiet = 1; doorbell_cmd(4, a2); quiet = 0;
    run_s(3);
    check("nobody at 3 m: no photo", photos == 0);
    for (int d = 300; d >= 60; d -= 20) { person_cm = d; run_s(0.5); }
    run_s(2);
    check("walked to 60 cm: exactly one photo", photos == 1 && mqtt_ons == 1);
    check("distance seen as 60 cm", last_cm >= 58 && last_cm <= 62);
    person_cm = 250; run_s(3);
    check("walked away: OFF sent", mqtt_offs == 1);
    person_cm = 80; run_s(3);
    check("came back within 30 s: no second photo (hold-off)", photos == 1);
    person_cm = 300; run_s(30);
    person_cm = 180; run_s(5);
    check("passer-by at 180 cm: no photo", photos == 1);
    person_cm = 90; run_s(0.25); person_cm = 300; run_s(3);
    check("one stray near reading: no photo", photos == 1);
    /* radar + distance: both must agree */
    char *a3[] = { "doorbell", "radar", "17" }; quiet = 1; doorbell_cmd(3, a3); quiet = 0;
    radar_level = 1; person_cm = 300; run_s(35);
    check("radar on but nobody near: no photo", photos == 1);
    person_cm = 70; run_s(2);
    check("radar on and near: photo", photos == 2);
    person_cm = 300; run_s(35); radar_level = 0; person_cm = 70; run_s(3);
    check("near but radar quiet: no photo", photos == 2);
    wired = 0; radar_level = 0; run_s(2);
    check("no echo counted", echo_missing > 0);
    printf(errors ? "FAILED (%d)\n" : "all OK\n", errors);
    return errors != 0;
}
