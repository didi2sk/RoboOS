/*
 * PC test of src/sensors.c with a fake BMP180 at 0x77 (datasheet calibration example):
 * chip id 0x55, calibration at 0xAA, 0xF4 start commands, SCO busy bit, 0xF6 results.
 *   cc -O1 -w -o /tmp/test_sensors tools/test_sensors.c && /tmp/test_sensors
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define KERNEL_H                                          /* src/sensors.c includes kernel.h: stand-ins */
#define SV_CPU 1
#define SV_TEMP 2
#define SV_HUM 4
#define SV_PRESS 8
#define SV_AIN 16
#define SENSOR_NONE (-999999)
struct sensor_values { int have, cpu_mc, temp_c100, hum_c100, press_pa, ain_mv[4]; };
static u64 now_us;
static u64 timer_us(void) { return now_us; }
static void delay_us(u64 us) { now_us += us; }
#define kprintf printf
static int mbox_prop(u32 t, u32 *b, int n) { (void)t; (void)n; b[1] = 45000; return 1; }
static void i2c_init(void) {}
static u64 clock_make(int y, int mo, int d, int h, int mi, int s) { return 0; }
static void clock_split(u64 t, int *y, int *mo, int *d, int *h, int *mi, int *s) {}
static int clock_format(char *b, u64 t, int iso) { b[0] = 0; return 0; }

static const u16 calw[11] = { 408, (u16)-72, (u16)-14383, 32741, 32757, 23153, 6190, 4, (u16)-32768, (u16)-8711, 2868 };
static u8 f4, ready_at_set; static u64 ready_at; static int starts_t, starts_p, early_reads;
static int i2c_probe(u8 a) { return a == 0x77; }
static int i2c_read_reg(u8 a, u8 r, void *bv, int n)
{
    u8 *b = bv;
    if (a != 0x77) return -1;
    if (r == 0xD0) { b[0] = 0x55; return 0; }
    if (r == 0xAA) { for (int i = 0; i < 11 && 2 * i < n; i++) { b[2 * i] = (u8)(calw[i] >> 8); b[2 * i + 1] = (u8)calw[i]; } return 0; }
    if (r == 0xF4) { b[0] = (u8)(f4 | (now_us < ready_at ? 0x20 : 0)); return 0; }
    if (r == 0xF6) {
        if (now_us < ready_at) early_reads++;
        if (f4 == 0x2E) { b[0] = 27898 >> 8; b[1] = 27898 & 255; }
        else { int oss = f4 >> 6;                     /* the datasheet UP (oss 0) scaled to this oss, as 24 bits */
               u32 raw = (23843u << oss) << (8 - oss); b[0] = (u8)(raw >> 16); b[1] = (u8)(raw >> 8); b[2] = (u8)raw; }
        return 0;
    }
    return -1;
}
static int i2c_write_reg(u8 a, u8 r, const void *bv, int n)
{
    const u8 *b = bv;
    if (a != 0x77 || r != 0xF4) return -1;
    f4 = b[0];
    if (f4 == 0x2E) { starts_t++; ready_at = now_us + 4500; }
    else { starts_p++; ready_at = now_us + 1500 + 3000u * (2u << (f4 >> 6)); }
    return 0;
}
#include "../src/sensors.c"

int main(void)
{
    int errors = 0;
    struct sensor_values v;
    sensors_probe();
    sensors_read(&v);
    printf("have 0x%x, temp %d (0.01 C), pressure %d Pa\n", v.have, v.temp_c100, v.press_pa);
    if (!(v.have & SV_TEMP) || v.temp_c100 != 1500) { printf("temperature WRONG\n"); errors++; }
    /* oss 1 with UP doubled = same physical pressure as the datasheet example (+- rounding) */
    if (!(v.have & SV_PRESS) || v.press_pa < 69950 || v.press_pa > 69980) { printf("pressure WRONG\n"); errors++; }
    if (v.have & SV_HUM) { printf("humidity should not be there\n"); errors++; }
    if (starts_t != 1 || starts_p != 1) { printf("starts %d/%d WRONG\n", starts_t, starts_p); errors++; }
    if (early_reads) { printf("read before the conversion ended\n"); errors++; }
    printf("sensor_value(1) = %d, (3) = %d, (2) = %d\n", sensor_value(1), sensor_value(3), sensor_value(2));
    if (sensor_value(2) != SENSOR_NONE) errors++;
    sensors_cmd(1, (char *[]){ "sensors" });
    printf(errors ? "\n%d ERRORS\n" : "\nall OK\n", errors);
    return errors != 0;
}
