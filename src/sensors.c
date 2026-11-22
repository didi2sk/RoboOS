/*
 * I2C sensors found by themselves at boot (and on 'sensors scan'):
 *   BME280 / BMP280 at 0x76 / 0x77   temperature, pressure (+ humidity on the BME280)
 *   BMP180 / BMP085 at 0x77          temperature, pressure (0.73: chip id 0x55, older Bosch chip)
 *   AHT20 / AHT10   at 0x38          humidity + temperature (0.79; often on one board with a BMP280)
 *   ADS1115         at 0x48 - 0x4B   4 analog inputs, +-4.096 V range (0.125 mV steps)
 *   DS3231 / DS1307 at 0x68          battery clock: the time at boot without a network,
 *                                    set again every time NTP answers
 * Used by the 'sensors' command, MQTT, the JSON API, BASIC SENSOR() and C sensorRead().
 */
#include "kernel.h"
#ifndef SENSORS_NO_LOG
#include "sd.h"
#endif
#include "bme280_math.h"
#include "bmp180_math.h"

static int bme_addr, bme_hum, bmp180, ads_addr, rtc_addr, aht, probed;
static int aht_t, aht_h;                                  /* last AHT20 values (0.01 C, 0.01 %) */
static u64 aht_at;                                        /* when measured (a measurement takes 80 ms) */

/* ---- AHT20 / AHT10 (0x38): status bit 3 = calibrated (else 0xBE 08 00), measure = 0xAC 33 00, ~80 ms,
 * then 7 bytes: status (bit 7 busy), 20 bit humidity, 20 bit temperature, CRC-8 (poly 0x31, init 0xFF) */
static u8 aht_crc(const u8 *d, int n)
{
    u8 c = 0xFF;
    for (int i = 0; i < n; i++) { c ^= d[i]; for (int b = 0; b < 8; b++) c = (u8)(c & 0x80 ? (c << 1) ^ 0x31 : c << 1); }
    return c;
}

static int aht_setup(void)
{
    u8 st = 0;
    if (i2c_read(0x38, &st, 1)) return 0;
    if (!(st & 0x08)) {
        static const u8 init[3] = { 0xBE, 0x08, 0x00 };   /* AHT20; an AHT10 wants 0xE1 */
        static const u8 init10[3] = { 0xE1, 0x08, 0x00 };
        if (i2c_write(0x38, init, 3)) i2c_write(0x38, init10, 3);
        delay_us(10000);
    }
    return 1;
}

static int aht_measure(void)
{
    if (aht_at && timer_us() - aht_at < 2000000) return 0;   /* fresh enough: keep the last values */
    static const u8 cmd[3] = { 0xAC, 0x33, 0x00 };
    if (i2c_write(0x38, cmd, 3)) return -1;
    delay_us(80000);
    u8 d[7] = { 0x80 };
    u64 end = timer_us() + 100000;
    while (!i2c_read(0x38, d, 7) && (d[0] & 0x80) && timer_us() < end) delay_us(5000);
    if (d[0] & 0x80) return -1;
    if (aht_crc(d, 6) != d[6]) return -1;
    u32 rh = (u32)d[1] << 12 | (u32)d[2] << 4 | d[3] >> 4;
    u32 rt = ((u32)d[3] & 15) << 16 | (u32)d[4] << 8 | d[5];
    aht_h = (int)(((u64)rh * 10000) >> 20);
    aht_t = (int)(((u64)rt * 20000) >> 20) - 5000;
    aht_at = timer_us();
    return 0;
}
static struct bme280_cal cal;
static struct bmp180_cal cal180;
#define BMP180_OSS 1                                       /* 2 samples, 7.5 ms per pressure reading */

static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

/* BMP180: 11 calibration words (big endian) at 0xAA; every reading is started by hand */
static int bmp180_setup(u8 a)
{
    u8 b[22];
    if (i2c_read_reg(a, 0xAA, b, 22)) return 0;
    short *sp[3] = { &cal180.ac1, &cal180.ac2, &cal180.ac3 };
    for (int i = 0; i < 3; i++) *sp[i] = (short)be16(b + 2 * i);
    cal180.ac4 = be16(b + 6); cal180.ac5 = be16(b + 8); cal180.ac6 = be16(b + 10);
    short *tp[5] = { &cal180.b1, &cal180.b2, &cal180.mb, &cal180.mc, &cal180.md };
    for (int i = 0; i < 5; i++) *tp[i] = (short)be16(b + 12 + 2 * i);
    for (int i = 0; i < 11; i++) { u16 w = be16(b + 2 * i); if (w == 0 || w == 0xFFFF) return 0; }   /* bad / no EEPROM */
    return 1;
}

static int bmp180_measure(u8 a, u8 cmd, int wait_us, u8 *d, int n)
{
    if (i2c_write_reg(a, 0xF4, &cmd, 1)) return -1;
    delay_us((u64)wait_us);
    u64 end = timer_us() + 30000;
    u8 c = 0x20;
    while (i2c_read_reg(a, 0xF4, &c, 1) == 0 && (c & 0x20) && timer_us() < end) delay_us(500);   /* SCO = still converting */
    return i2c_read_reg(a, 0xF6, d, n);
}

static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

static int bme_setup(u8 a)
{
    u8 id = 0, b[26], h[7];
    if (i2c_read_reg(a, 0xD0, &id, 1)) return 0;
    if (id == 0x55) { bmp180 = bmp180_setup(a); return bmp180; }
    if (id != 0x60 && id != 0x58) return 0;
    if (i2c_read_reg(a, 0x88, b, 24)) return 0;
    cal.t1 = le16(b); cal.t2 = (short)le16(b + 2); cal.t3 = (short)le16(b + 4);
    cal.p1 = le16(b + 6);
    short *pp[8] = { &cal.p2, &cal.p3, &cal.p4, &cal.p5, &cal.p6, &cal.p7, &cal.p8, &cal.p9 };
    for (int i = 0; i < 8; i++) *pp[i] = (short)le16(b + 8 + 2 * i);
    bme_hum = id == 0x60;
    if (bme_hum) {
        if (i2c_read_reg(a, 0xA1, &cal.h1, 1) || i2c_read_reg(a, 0xE1, h, 7)) return 0;
        cal.h2 = (short)le16(h);
        cal.h3 = h[2];
        cal.h4 = (short)((signed char)h[3] * 16 | (h[4] & 0x0F));
        cal.h5 = (short)((signed char)h[5] * 16 | (h[4] >> 4));
        cal.h6 = (signed char)h[6];
        u8 v = 1;                                         /* humidity x1 (before ctrl_meas) */
        i2c_write_reg(a, 0xF2, &v, 1);
    }
    u8 cfg = 5 << 5;                                      /* 1 s between measurements, no filter */
    i2c_write_reg(a, 0xF5, &cfg, 1);
    u8 meas = (1 << 5) | (1 << 2) | 3;                    /* temp x1, pressure x1, normal mode */
    i2c_write_reg(a, 0xF4, &meas, 1);
    return 1;
}

void sensors_probe(void)
{
    probed = 1;
    bme_addr = ads_addr = rtc_addr = 0; bmp180 = bme_hum = 0; aht = 0; aht_at = 0;
    i2c_init();
    for (u8 a = 0x76; a <= 0x77 && !bme_addr; a++) if (i2c_probe(a) && bme_setup(a)) bme_addr = a;
    aht = i2c_probe(0x38) && aht_setup();
    for (u8 a = 0x48; a <= 0x4B && !ads_addr; a++) {
        u8 c[2];
        if (i2c_read_reg(a, 0x01, c, 2) == 0 && (c[0] & 0x7F) == 0x05 && c[1] == 0x83) ads_addr = a;   /* reset value 0x8583 */
    }
    u8 who = 0;
    if (i2c_probe(0x68) && !(i2c_read_reg(0x68, 0x75, &who, 1) == 0 && (who == 0x68 || who == 0x70 || who == 0x71 || who == 0x73))) {
        /* (0.71: an MPU-6050/6500/9250 on 0x68 answers WHO_AM_I 0x68/0x70/0x71/0x73 - it is not a clock) */
        u8 r[7];
        if (i2c_read_reg(0x68, 0x00, r, 7) == 0 && (r[1] & 0x7F) < 0x60 && (r[5] & 0x1F) <= 0x12) rtc_addr = 0x68;
    }
}

int sensors_have(void) { if (!probed) sensors_probe(); return (bme_addr != 0 || aht) | (bme_hum || aht ? 2 : 0) | (ads_addr ? 4 : 0) | (rtc_addr ? 8 : 0); }

int sensors_read(struct sensor_values *v)
{
    memset(v, 0, sizeof(*v));
    if (!probed) sensors_probe();
    u32 t[2] = { 0, 0 };
    if (mbox_prop(0x00030006, t, 2)) { v->cpu_mc = (int)t[1]; v->have |= SV_CPU; }
    if (bme_addr && bmp180) {
        u8 d[3];
        int b5;
        if (bmp180_measure((u8)bme_addr, 0x2E, 4500, d, 2) == 0) {
            int t = bmp180_temp(&cal180, d[0] << 8 | d[1], &b5);
            v->temp_c100 = t * 10;
            v->have |= SV_TEMP;
            if (bmp180_measure((u8)bme_addr, (u8)(0x34 + (BMP180_OSS << 6)), 7500, d, 3) == 0) {
                int up = (d[0] << 16 | d[1] << 8 | d[2]) >> (8 - BMP180_OSS);
                v->press_pa = bmp180_press(&cal180, up, b5, BMP180_OSS);
                v->have |= SV_PRESS;
            }
        }
    }
    else if (bme_addr) {
        u8 d[8];
        if (i2c_read_reg((u8)bme_addr, 0xF7, d, bme_hum ? 8 : 6) == 0) {
            int ap = (int)(d[0] << 12 | d[1] << 4 | d[2] >> 4), at = (int)(d[3] << 12 | d[4] << 4 | d[5] >> 4), tf;
            v->temp_c100 = bme280_temp(&cal, at, &tf);
            v->press_pa = (int)(bme280_press(&cal, ap, tf) / 256);
            v->have |= SV_TEMP | SV_PRESS;
            if (bme_hum) {
                int ah = d[6] << 8 | d[7];
                v->hum_c100 = (int)(bme280_hum(&cal, ah, tf) * 100 / 1024);
                v->have |= SV_HUM;
            }
        }
    }
    if (aht && aht_measure() == 0 && aht_at) {
        if (!(v->have & SV_HUM)) { v->hum_c100 = aht_h; v->have |= SV_HUM; }   /* a BME280 keeps its own */
        if (!(v->have & SV_TEMP)) { v->temp_c100 = aht_t; v->have |= SV_TEMP; }
    }
    if (ads_addr) {
        for (int ch = 0; ch < 4; ch++) {
            u16 cfg = (u16)(0x8000 | ((4 + ch) << 12) | (1 << 9) | (1 << 8) | (7 << 5) | 3);   /* single shot, AINx-GND, 4.096 V, 860 SPS */
            u8 c[2] = { (u8)(cfg >> 8), (u8)cfg };
            if (i2c_write_reg((u8)ads_addr, 0x01, c, 2)) break;
            u64 end = timer_us() + 5000;
            u8 st[2] = { 0, 0 };
            do { delay_us(300); i2c_read_reg((u8)ads_addr, 0x01, st, 2); } while (!(st[0] & 0x80) && timer_us() < end);
            u8 r[2];
            if (i2c_read_reg((u8)ads_addr, 0x00, r, 2)) break;
            v->ain_mv[ch] = (short)(r[0] << 8 | r[1]) / 8;   /* 0.125 mV per step */
            v->have |= SV_AIN;
        }
    }
    return v->have;
}

/* one value for BASIC SENSOR(n) and C sensorRead(n): 0 CPU, 1 temperature (0.01 C),
 * 2 humidity (0.01 %), 3 pressure (Pa), 4-7 ADS1115 A0-A3 (mV); SENSOR_NONE = not there */
int sensor_value(int n)
{
    static const int need[8] = { SV_CPU, SV_TEMP, SV_HUM, SV_PRESS, SV_AIN, SV_AIN, SV_AIN, SV_AIN };
    struct sensor_values v;
#ifndef SENSORS_NO_LOG
    if (n == 14 || n == 15) {                             /* 0.87: AMG8833 */
        struct thermal_values t;
        if (thermal_get(&t)) return SENSOR_NONE;
        return n == 14 ? t.max_c100 : t.present;
    }
    if (n >= 8 && n <= 13) {                              /* 0.83: MPU-6050 */
        struct motion_values m;
        if (motion_get(&m)) return SENSOR_NONE;
        switch (n) { case 8: return m.pitch_d10; case 9: return m.roll_d10; case 10: return (int)m.shakes;
                     case 11: return m.ax_mg; case 12: return m.ay_mg; default: return m.az_mg; }
    }
#endif
    if (n < 0 || n > 7) return SENSOR_NONE;
    sensors_read(&v);
    if (!(v.have & need[n])) return SENSOR_NONE;
    switch (n) {
    case 0: return v.cpu_mc / 10;
    case 1: return v.temp_c100;
    case 2: return v.hum_c100;
    case 3: return v.press_pa;
    }
    return v.ain_mv[n - 4];
}

/* ---------------- DS3231 / DS1307: the battery clock (UTC) ---------------- */

static int bcd(u8 b) { return (b >> 4) * 10 + (b & 15); }
static u8 tobcd(int v) { return (u8)((v / 10) << 4 | v % 10); }

int rtc_present(void) { if (!probed) sensors_probe(); return rtc_addr != 0; }

/* the time from the clock chip, 0 = none / not set */
u64 rtc_read(void)
{
    if (!rtc_present()) return 0;
    u8 r[7], st = 0;
    if (i2c_read_reg((u8)rtc_addr, 0x00, r, 7)) return 0;
    if (i2c_read_reg((u8)rtc_addr, 0x0F, &st, 1) == 0 && (st & 0x80)) return 0;   /* DS3231: oscillator stopped */
    if (r[0] & 0x80) return 0;                            /* DS1307: clock halted */
    int hour = (r[2] & 0x40) ? (bcd(r[2] & 0x1F) % 12 + ((r[2] & 0x20) ? 12 : 0)) : bcd(r[2] & 0x3F);
    int y = 2000 + bcd(r[6]) + ((r[5] & 0x80) ? 100 : 0);
    if (y < 2024) return 0;
    return clock_make(y, bcd(r[5] & 0x1F), bcd(r[4] & 0x3F), hour, bcd(r[1] & 0x7F), bcd(r[0] & 0x7F));
}

void rtc_write(u64 t)
{
    if (!rtc_present() || !t) return;
    int y, mo, d, h, mi, s;
    clock_split(t, &y, &mo, &d, &h, &mi, &s);
    u8 r[7] = { tobcd(s), tobcd(mi), tobcd(h), 1, tobcd(d), (u8)(tobcd(mo) | (y >= 2100 ? 0x80 : 0)), tobcd(y % 100) };
    i2c_write_reg((u8)rtc_addr, 0x00, r, 7);
    u8 st = 0;
    if (i2c_read_reg((u8)rtc_addr, 0x0F, &st, 1) == 0 && (st & 0x80)) { st &= 0x7F; i2c_write_reg((u8)rtc_addr, 0x0F, &st, 1); }
}

/* ---------------- command ---------------- */

static void put_c100(const char *what, int v, const char *unit)
{
    int a = v < 0 ? -v : v;
    kprintf("  %-12s %s%d.%02d %s\n", what, v < 0 ? "-" : "", a / 100, a % 100, unit);
}

/* ---------------- the log (0.82): one CSV per day, /LOG/YYYYMMDD.CSV ----------------
 * "time,temp_c,hum_pct,press_hpa,cpu_c" and a line every N minutes (on the minute, local time). The day's
 * file is kept in RAM and rewritten whole (fat_write_file has no append; a day at 1 minute = ~45 KB). */
#ifndef SENSORS_NO_LOG
static int log_every;                                     /* minutes, 0 = off */
static char logbuf[64 * 1024];
static int loglen = -1;                                   /* -1 = not loaded for log_day */
static char log_day[16];                                  /* "YYYYMMDD" */
static u64 log_last_min;
static u32 log_lines;
static const char *log_err = "";

int sensors_log_every(void) { return log_every; }

static void log_save_cfg(void)
{
    char b[32]; int n = 0;
    const char *s = "every "; while (*s) b[n++] = *s++;
    int v = log_every; char t[8]; int k = 0; do t[k++] = (char)('0' + v % 10); while ((v /= 10)); while (k) b[n++] = t[--k];
    b[n++] = '\n';
    if (fat_mounted()) fat_write_file("/SENSLOG.TXT", b, (u32)n);
}

void sensors_log_boot(void)
{
    struct fat_file f; char b[32];
    if (!fat_mounted() || fat_open("/SENSLOG.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    u64 v;
    char *p = b; while (*p && (*p < '0' || *p > '9')) p++;
    char *e = p; while (*e >= '0' && *e <= '9') e++;
    *e = 0;
    if (parse_num(p, &v) && v <= 1440) log_every = (int)v;
}

static int app_c100(char *b, int n, int v)
{
    if (v < 0) { b[n++] = '-'; v = -v; }
    char t[12]; int k = 0; u32 w = (u32)(v / 100); do t[k++] = (char)('0' + w % 10); while ((w /= 10)); while (k) b[n++] = t[--k];
    b[n++] = '.'; b[n++] = (char)('0' + v / 10 % 10); b[n++] = (char)('0' + v % 10);
    return n;
}

static int log_write(u64 now)
{
    char ts[32];
    clock_format(ts, now, 0);                             /* "YYYY-MM-DD HH:MM:SS" local */
    char day[16]; int d = 0;
    for (int i = 0; i < 10; i++) if (ts[i] != '-') day[d++] = ts[i];
    day[d] = 0;
    char path[24] = "/LOG/"; int pn = 5;
    for (int i = 0; day[i]; i++) path[pn++] = day[i];
    const char *ext = ".CSV"; while (*ext) path[pn++] = *ext++;
    path[pn] = 0;
    if (loglen < 0 || strcmp(day, log_day)) {             /* a new day (or the first line since boot) */
        for (int i = 0; i <= d; i++) log_day[i] = day[i];
        loglen = 0;
        struct fat_file f;
        if (fat_open(path, &f) == 0 && !f.is_dir) {
            int n = fat_read(&f, logbuf, sizeof(logbuf) - 1);
            loglen = n > 0 ? n : 0;
        } else {
            fat_mkdir("/LOG");
            const char *h = "time,temp_c,hum_pct,press_hpa,cpu_c\n";
            while (*h) logbuf[loglen++] = *h++;
        }
    }
    struct sensor_values v;
    sensors_read(&v);
    char line[96]; int n = 0;
    for (int i = 11; i < 16; i++) line[n++] = ts[i];      /* HH:MM */
    line[n++] = ',';
    if (v.have & SV_TEMP) n = app_c100(line, n, v.temp_c100);
    line[n++] = ',';
    if (v.have & SV_HUM) n = app_c100(line, n, v.hum_c100);
    line[n++] = ',';
    if (v.have & SV_PRESS) n = app_c100(line, n, v.press_pa);
    line[n++] = ',';
    if (v.have & SV_CPU) n = app_c100(line, n, v.cpu_mc / 10);
    line[n++] = '\n';
    if (loglen + n >= (int)sizeof(logbuf)) { log_err = "the day's file is full (64 KB)"; return -1; }
    for (int i = 0; i < n; i++) logbuf[loglen + i] = line[i];
    loglen += n;
    int e = fat_write_file(path, logbuf, (u32)loglen);
    if (e) { log_err = "card write failed"; return -1; }
    log_lines++; log_err = "";
    return 0;
}

void sensors_log_poll(void)                               /* core 0 */
{
    static u64 next_check;
    if (!log_every || io_held() || timer_us() < next_check) return;
    next_check = timer_us() + 1000000;
    u64 now = clock_now();
    if (!now || !fat_mounted()) return;
    u64 local_min = (now + (u64)(clock_offset(now) * 60)) / 60;
    if (local_min == log_last_min || local_min % (u64)log_every) return;
    log_last_min = local_min;
    log_write(now);
}

static void log_cmd(int argc, char **argv)
{
    u64 v;
    if (argc > 2 && !strcmp(argv[2], "off")) { log_every = 0; log_save_cfg(); kprintf("sensor log off\n"); return; }
    if (argc > 2 && !strcmp(argv[2], "now")) {
        u64 now = clock_now();
        if (!now) { kprintf("the time is not known yet (NTP / 'time set')\n"); return; }
        kprintf(log_write(now) ? "failed: %s\n" : "written to /LOG/%s.CSV\n", log_err[0] ? log_err : log_day);
        return;
    }
    if (argc > 2) {
        if (!parse_num(argv[2], &v) || v < 1 || v > 1440) { kprintf("sensors log <minutes 1-1440> | off | now\n"); return; }
        log_every = (int)v; log_save_cfg();
    }
    if (!log_every) { kprintf("sensor log off ('sensors log 5' = a line every 5 minutes to /LOG/YYYYMMDD.CSV)\n"); return; }
    kprintf("sensor log every %d min -> /LOG/YYYYMMDD.CSV (one file per day), %u lines written since boot%s%s\n",
            log_every, log_lines, log_err[0] ? ", last problem: " : "", log_err);
    if (!clock_now()) kprintf("  (waiting for the time: NTP or 'time set')\n");
}
#endif

void sensors_cmd(int argc, char **argv)
{
#ifndef SENSORS_NO_LOG
    if (argc > 1 && !strcmp(argv[1], "log")) { log_cmd(argc, argv); return; }
#endif
    if (argc > 1 && !strcmp(argv[1], "scan")) sensors_probe();
    else if (argc > 1) { kprintf("usage: sensors [scan | log <minutes>|off|now]\n"); return; }
    struct sensor_values v;
    sensors_read(&v);
    kprintf("sensors (I2C on GPIO2/3; 'i2c scan' lists every device):\n");
    if (v.have & SV_CPU) put_c100("CPU", v.cpu_mc / 10, "C");
    if ((v.have & SV_TEMP) && bme_addr) put_c100(bmp180 ? "BMP180 temp" : bme_hum ? "BME280 temp" : "BMP280 temp", v.temp_c100, "C");
    if (aht && aht_at) put_c100("AHT20 temp", aht_t, "C");
    if (v.have & SV_HUM) put_c100("humidity", v.hum_c100, "%");
    if (v.have & SV_PRESS) kprintf("  %-12s %d.%02d hPa\n", "pressure", v.press_pa / 100, v.press_pa % 100);
    if (v.have & SV_AIN)
        for (int i = 0; i < 4; i++) kprintf("  ADS1115 A%d   %d mV\n", i, v.ain_mv[i]);
    if (rtc_addr) {
        u64 t = rtc_read();
        char b[32];
        if (t) { clock_format(b, t, 0); kprintf("  %-12s %s\n", "RTC", b); }
        else kprintf("  %-12s found at 0x68, not set yet (NTP or 'time set' sets it)\n", "RTC");
    }
    if (aht && !aht_at) kprintf("  AHT20 at 0x38: no answer to the measurement\n");
    if (!bme_addr && !ads_addr && !rtc_addr && !aht) kprintf("  no BME280 / BMP180 / AHT20 / ADS1115 / DS3231 found\n");
}
