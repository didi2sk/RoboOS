/*
 * motion.c - MPU-6050 / MPU-6500 / MPU-9250 (GY-521 board) on I2C 0x68 / 0x69 (0.83).
 *
 * 3-axis accelerometer (+-2 g, 16384 LSB/g), 3-axis gyroscope (+-250 deg/s, 131 LSB/(deg/s)) and a die
 * thermometer. Registers: WHO_AM_I 0x75 (0x68 = 6050, 0x70 = 6500, 0x71 / 0x73 = 9250 / 9255),
 * PWR_MGMT_1 0x6B (0x01 = awake, PLL clock), SMPLRT_DIV 0x19, CONFIG 0x1A (DLPF), GYRO_CONFIG 0x1B,
 * ACCEL_CONFIG 0x1C, data 0x3B..0x48 = ax ay az temp gx gy gz (big endian, signed).
 * Tilt (pitch / roll) from gravity with an integer CORDIC atan2 - the kernel has no floating point.
 * motion_poll (core 0, con_trygetc) reads it every 50 ms: a shake (|a| off 1 g by more than the threshold
 * or a gyro axis over 150 deg/s) counts, prints with 'motion alarm on' and goes to MQTT <base>/motion.
 * /MOTION.TXT: threshold <mg>, alarm on|off, zero <pitch> <roll> (0.1 deg, from 'motion zero').
 */
#include "kernel.h"
#ifndef MOTION_HOST_TEST
#include "sd.h"
#endif

static int addr, kind;                                  /* 0x68 / 0x69, WHO_AM_I value */
static int ax, ay, az, gx, gy, gz, traw;                /* last raw values */
static int pitch0, roll0;                               /* 'motion zero' reference, 0.1 deg */
static int thresh = 400, alarm_on;                      /* shake threshold in mg */
static volatile u32 shakes;
static u64 next_poll, last_shake, last_read;
static int gx0, gy0, gz0;                               /* gyro offsets (raw) from 'motion zero' */

/* ---- integer helpers ---- */
static const int atan_tab[14] = { 4500, 2657, 1404, 713, 358, 179, 90, 45, 22, 11, 6, 3, 1, 1 };  /* atan(2^-i), 0.01 deg */

/* atan2(y, x) in 0.1 degrees, -1800..1800 */
static int iatan2(long y, long x)
{
    if (!x && !y) return 0;
    long ang = 0;
    if (x < 0) { ang = y >= 0 ? 18000 : -18000; x = -x; y = -y; }
    x <<= 8; y <<= 8;
    for (int i = 0; i < 14; i++) {
        long nx;
        if (y > 0) { nx = x + (y >> i); y = y - (x >> i); ang += atan_tab[i]; }
        else       { nx = x - (y >> i); y = y + (x >> i); ang -= atan_tab[i]; }
        x = nx;
    }
    if (ang > 18000) ang -= 36000;
    if (ang < -18000) ang += 36000;
    return (int)((ang + (ang >= 0 ? 5 : -5)) / 10);
}

static u32 isqrt(u64 v)
{
    u64 r = 0, b = (u64)1 << 62;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return (u32)r;
}

/* ---- chip ---- */
static int wr(u8 reg, u8 v) { return i2c_write_reg((u8)addr, reg, &v, 1); }

static int setup(void)
{
    if (wr(0x6B, 0x80)) return 0;                       /* reset */
    delay_us(100000);
    wr(0x6B, 0x01);                                     /* awake, gyro PLL clock */
    delay_us(10000);
    wr(0x19, 7);                                        /* sample rate 1 kHz / 8 */
    wr(0x1A, 3);                                        /* low-pass ~44 Hz */
    wr(0x1B, 0);                                        /* +-250 deg/s */
    wr(0x1C, 0);                                        /* +-2 g */
    return 1;
}

static int read_raw(void)
{
    u8 d[14];
    if (!addr || i2c_read_reg((u8)addr, 0x3B, d, 14)) return -1;
    ax = (short)(d[0] << 8 | d[1]); ay = (short)(d[2] << 8 | d[3]); az = (short)(d[4] << 8 | d[5]);
    traw = (short)(d[6] << 8 | d[7]);
    gx = (short)(d[8] << 8 | d[9]); gy = (short)(d[10] << 8 | d[11]); gz = (short)(d[12] << 8 | d[13]);
    last_read = timer_us();
    return 0;
}

int motion_present(void) { return addr != 0; }

/* values for the shell, display, API, MQTT, BASIC */
int motion_get(struct motion_values *m)
{
    if (!addr) return -1;
    if (!last_read || timer_us() - last_read > 40000) if (read_raw()) return -1;
    m->ax_mg = ax * 1000 / 16384; m->ay_mg = ay * 1000 / 16384; m->az_mg = az * 1000 / 16384;
    m->gx_d10 = (gx - gx0) * 10 / 131; m->gy_d10 = (gy - gy0) * 10 / 131; m->gz_d10 = (gz - gz0) * 10 / 131;
    m->temp_c100 = kind == 0x68 ? traw * 100 / 340 + 3653 : traw * 100 / 334 + 2100;
    m->g_mg = (int)isqrt((u64)((long)m->ax_mg * m->ax_mg + (long)m->ay_mg * m->ay_mg + (long)m->az_mg * m->az_mg));
    m->pitch_d10 = iatan2(-(long)ax, (long)isqrt((u64)((long)ay * ay + (long)az * az))) - pitch0;
    m->roll_d10 = iatan2((long)ay, (long)az) - roll0;
    m->shakes = shakes;
    return 0;
}

#ifndef MOTION_HOST_TEST
static void save(void)
{
    char b[96]; int n = 0;
    #define PUT(s) do { const char *s_ = (s); while (*s_) b[n++] = *s_++; } while (0)
    #define NUM(v) do { int v_ = (v); char t_[12]; int k_ = 0; if (v_ < 0) { b[n++] = '-'; v_ = -v_; } do t_[k_++] = (char)('0' + v_ % 10); while ((v_ /= 10)); while (k_) b[n++] = t_[--k_]; } while (0)
    PUT("threshold "); NUM(thresh); PUT("\nalarm "); PUT(alarm_on ? "on" : "off");
    PUT("\nzero "); NUM(pitch0); PUT(" "); NUM(roll0); PUT("\n");
    #undef PUT
    #undef NUM
    if (fat_mounted()) fat_write_file("/MOTION.TXT", b, (u32)n);
}

static int snum(const char **p)
{
    int neg = 0, v = 0;
    while (**p == ' ') (*p)++;
    if (**p == '-') { neg = 1; (*p)++; }
    while (**p >= '0' && **p <= '9') v = v * 10 + (*(*p)++ - '0');
    return neg ? -v : v;
}

static void load(void)
{
    struct fat_file f; char b[128];
    if (!fat_mounted() || fat_open("/MOTION.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    for (char *l = b; *l; ) {
        char *e = l; while (*e && *e != '\n' && *e != '\r') e++;
        char sv = *e; *e = 0;
        const char *p = l;
        if (!strncmp(l, "threshold ", 10)) { p += 10; int v = snum(&p); if (v >= 50 && v <= 4000) thresh = v; }
        else if (!strncmp(l, "alarm ", 6)) alarm_on = !strcmp(l + 6, "on");
        else if (!strncmp(l, "zero ", 5)) { p += 5; pitch0 = snum(&p); roll0 = snum(&p); }
        *e = sv; l = *e ? e + 1 : e; while (*l == '\n' || *l == '\r') l++;
    }
}
#endif

static int probe_at(int a)
{
    u8 who = 0;
    if (i2c_read_reg((u8)a, 0x75, &who, 1)) return 0;
    if (who != 0x68 && who != 0x70 && who != 0x71 && who != 0x73) return 0;
    addr = a; kind = who;
    return 1;
}

void motion_boot(void)
{
    if (!probe_at(0x68) && !probe_at(0x69)) return;
    if (!setup()) { addr = 0; return; }
#ifndef MOTION_HOST_TEST
    load();
#endif
    kprintf("motion sensor MPU-%s at 0x%02x\n", kind == 0x68 ? "6050" : kind == 0x70 ? "6500" : "9250", (u32)addr);
}

/* core 0, con_trygetc: shake detection every 50 ms */
void motion_poll(void)
{
    if (!addr || io_held() || timer_us() < next_poll) return;
    next_poll = timer_us() + 50000;
    struct motion_values m;
    if (motion_get(&m)) return;
    int dev = m.g_mg - 1000; if (dev < 0) dev = -dev;
    int spin = 0;
    if (m.gx_d10 > 1500 || m.gx_d10 < -1500 || m.gy_d10 > 1500 || m.gy_d10 < -1500 || m.gz_d10 > 1500 || m.gz_d10 < -1500) spin = 1;
    if ((dev > thresh || spin) && timer_us() - last_shake > 1000000) {
        last_shake = timer_us();
        shakes++;
        if (alarm_on) kprintf("\n[MOTION] shake %u (%d mg off 1 g%s)\n", shakes, dev, spin ? ", turning fast" : "");
#ifndef MOTION_HOST_TEST
        if (mqtt_connected()) { char p[16]; int k = 0; u32 s = shakes; char t[10]; int j = 0; do t[j++] = (char)('0' + s % 10); while ((s /= 10)); while (j) p[k++] = t[--j]; mqtt_publish("motion", p, k, 0); }
#endif
    }
}

static void put_d10(const char *what, int v, const char *unit)
{
    int a = v < 0 ? -v : v;
    kprintf("  %-10s %s%d.%d %s\n", what, v < 0 ? "-" : "", a / 10, a % 10, unit);
}

void motion_cmd(int argc, char **argv)
{
    const char *a = argc > 1 ? argv[1] : "";
    if (!strcmp(a, "scan")) { addr = 0; motion_boot(); if (!addr) kprintf("no MPU-6050 at 0x68 / 0x69\n"); return; }
    if (!addr) { kprintf("no motion sensor (MPU-6050 / GY-521 at I2C 0x68 or 0x69; 'motion scan' looks again)\n"); return; }
    struct motion_values m;
#ifndef MOTION_HOST_TEST
    if (!strcmp(a, "watch")) {                          /* live, until a key */
        kprintf("pitch / roll (deg), |a| (g) - any key stops\n");
        while (!con_trygetc()) {
            if (motion_get(&m) == 0) {
                int p = m.pitch_d10, r = m.roll_d10;
                kprintf("\r  pitch %s%d.%d  roll %s%d.%d  |a| %d.%02d   ", p < 0 ? "-" : " ", (p < 0 ? -p : p) / 10, (p < 0 ? -p : p) % 10,
                        r < 0 ? "-" : " ", (r < 0 ? -r : r) / 10, (r < 0 ? -r : r) % 10, m.g_mg / 1000, m.g_mg % 1000 / 10);
            }
            delay_us(100000);
        }
        kprintf("\n");
        return;
    }
    if (!strcmp(a, "zero")) {                           /* this position = level, gyro at rest = 0 */
        long sx = 0, sy = 0, sz = 0;
        pitch0 = roll0 = 0;
        for (int i = 0; i < 50; i++) { read_raw(); sx += gx; sy += gy; sz += gz; delay_us(5000); }
        gx0 = (int)(sx / 50); gy0 = (int)(sy / 50); gz0 = (int)(sz / 50);
        motion_get(&m);
        pitch0 = m.pitch_d10; roll0 = m.roll_d10;
        save();
        kprintf("this position is level now (saved in /MOTION.TXT); keep it still while zeroing\n");
        return;
    }
    if (!strcmp(a, "threshold") && argc > 2) {
        u64 v; if (!parse_num(argv[2], &v) || v < 50 || v > 4000) { kprintf("threshold 50-4000 mg\n"); return; }
        thresh = (int)v; save(); kprintf("shake threshold %d mg\n", thresh); return;
    }
    if (!strcmp(a, "alarm") && argc > 2) { alarm_on = !strcmp(argv[2], "on"); save(); kprintf("shake messages %s\n", alarm_on ? "on" : "off"); return; }
    if (*a) {
        kprintf("usage: motion                 values (tilt, acceleration, rotation, temperature)\n"
                "       motion watch           live tilt (a spirit level), any key stops\n"
                "       motion zero            this position = level (+ gyro offsets)\n"
                "       motion threshold <mg>  shake = this far off 1 g (default 400)\n"
                "       motion alarm on|off    print every shake (MQTT <base>/motion always)\n"
                "       motion scan\n");
        return;
    }
#endif
    if (motion_get(&m)) { kprintf("motion sensor not answering\n"); return; }
    kprintf("MPU-%s at 0x%02x:\n", kind == 0x68 ? "6050" : kind == 0x70 ? "6500" : "9250", (u32)addr);
    put_d10("pitch", m.pitch_d10, "deg");
    put_d10("roll", m.roll_d10, "deg");
    kprintf("  %-10s x %d  y %d  z %d mg  (|a| %d mg)\n", "accel", m.ax_mg, m.ay_mg, m.az_mg, m.g_mg);
    kprintf("  %-10s x %d  y %d  z %d (0.1 deg/s)\n", "gyro", m.gx_d10, m.gy_d10, m.gz_d10);
    { int t = m.temp_c100, at = t < 0 ? -t : t; kprintf("  %-10s %s%d.%02d C (chip)\n", "temp", t < 0 ? "-" : "", at / 100, at % 100); }
    kprintf("  %-10s %u (threshold %d mg, messages %s)\n", "shakes", m.shakes, thresh, alarm_on ? "on" : "off");
}
