/*
 * I2C master on BSC1 (0x3F804000), GPIO2 = SDA, GPIO3 = SCL (ALT0) - pins 3 and 5
 * on the 40-pin header, with the Pi's own 1.8 kOhm pull-ups. 100 kHz.
 * Polled. A spinlock makes every transfer whole: core 0 (shell, BASIC, programs)
 * and core 1 (MQTT publishing sensor values) may both use the bus.
 */
#include "kernel.h"

#define BSC        (MMIO_BASE + 0x804000)
#define BSC_C      (BSC + 0x00)
#define BSC_S      (BSC + 0x04)
#define BSC_DLEN   (BSC + 0x08)
#define BSC_A      (BSC + 0x0C)
#define BSC_FIFO   (BSC + 0x10)
#define BSC_DIV    (BSC + 0x14)
#define BSC_CLKT   (BSC + 0x1C)

#define C_I2CEN    (1u << 15)
#define C_ST       (1u << 7)
#define C_CLEAR    (3u << 4)
#define C_READ     (1u << 0)
#define S_TA       (1u << 0)
#define S_DONE     (1u << 1)
#define S_TXD      (1u << 4)
#define S_RXD      (1u << 5)
#define S_ERR      (1u << 8)            /* no ACK */
#define S_CLKT     (1u << 9)            /* the slave held SCL too long */

static int ready, lock;
static u32 speed = 100000, core_hz, last_s, last_us;   /* last_s / last_us: status + time of the last transfer (i2c probe) */

static void lk(void)  { while (__atomic_test_and_set(&lock, __ATOMIC_ACQUIRE)) ; }
static void ulk(void) { __atomic_clear(&lock, __ATOMIC_RELEASE); }

static void bb(int pin, int hi) { if (hi) gpio_func(pin, 0); else { gpio_set(pin, 0); gpio_func(pin, 1); } }

/* 0.70: a slave stuck in the middle of a byte keeps SDA low (owner's LCD backpack: "SDA LOW" in i2c scan,
 * then nothing answered until a failed transfer happened to clock it free). The standard bus clear:
 * up to 9 SCL pulses until SDA is released, then a STOP. Returns the pulses needed, 0 = SDA was free,
 * -1 = still low (a short?). No lock taken - callers hold it or run before anyone uses the bus. */
static int recover_l(void)
{
    if (gpio_get(2)) return 0;
    gpio_func(2, 0);                                      /* SDA: input (released) */
    int n = 0;
    while (n < 9 && !gpio_get(2)) {
        bb(3, 0); delay_us(5); bb(3, 1); delay_us(5); n++;
    }
    bb(2, 0); delay_us(5); bb(3, 1); delay_us(5); bb(2, 1); delay_us(5);   /* STOP */
    int ok = gpio_get(2);
    gpio_func(2, 4); gpio_func(3, 4);
    return ok ? (n ? n : 1) : -1;
}
static int recovered;                                     /* for i2c scan: what the last clear did */

void i2c_init(void)
{
    if (ready) return;
    gpio_func(2, 4);
    gpio_func(3, 4);
    u32 clk[2] = { 4, 0 };                                /* core clock */
    u32 core = mbox_prop(0x00030002, clk, 2) && clk[1] ? clk[1] : 250000000;
    core_hz = core;
    mmio_write(BSC_DIV, core / speed);
    mmio_write(BSC_CLKT, 0x40);
    mmio_write(BSC_C, C_I2CEN | C_CLEAR);
    mmio_write(BSC_S, S_DONE | S_ERR | S_CLKT);
    ready = 1;
    recovered = recover_l();
}

/* one transfer; 0 = ok, -1 = no answer (NACK), -2 = timeout / bus stuck */
static int xfer2(u8 addr, u8 *buf, int n, int rd);
static int xfer(u8 addr, u8 *buf, int n, int rd)
{
    u64 t0 = timer_us();
    if (!gpio_get(2)) recovered = recover_l();            /* SDA held low before we start: clear it */
    int r = xfer2(addr, buf, n, rd);
    if (r == -2 && !gpio_get(2)) recovered = recover_l();
    last_us = (u32)(timer_us() - t0);
    return r;
}
/* 0.81: after a NACK the BSC still finishes the transfer (STOP) with TA set; starting the next one at
 * once spoiled it (owner, 3B+: 'i2c probe' saw 0x38 + 0x77 answer, but 'i2c scan' - one address after
 * another - found nothing; the first transfer after a NACK always timed out). So wait for the end. */
static void bsc_settle(void)
{
    u64 end = timer_us() + 2000;
    while ((mmio_read(BSC_S) & S_TA) && timer_us() < end) ;
    if (mmio_read(BSC_S) & S_TA) { mmio_write(BSC_C, 0); delay_us(10); }   /* still busy: reset the controller */
    mmio_write(BSC_S, S_DONE | S_ERR | S_CLKT);
    mmio_write(BSC_C, C_I2CEN | C_CLEAR);
}

static int xfer2(u8 addr, u8 *buf, int n, int rd)
{
    int i = 0;
    if (mmio_read(BSC_S) & S_TA) bsc_settle();            /* a previous transfer still running */
    mmio_write(BSC_C, C_I2CEN | C_CLEAR);
    mmio_write(BSC_S, S_DONE | S_ERR | S_CLKT);
    mmio_write(BSC_A, addr);
    mmio_write(BSC_DLEN, (u32)n);
    if (!rd) while (i < n && i < 16) mmio_write(BSC_FIFO, buf[i++]);   /* fill the FIFO first */
    mmio_write(BSC_C, C_I2CEN | C_ST | (rd ? C_READ : 0));
    u64 end = timer_us() + 5000 + (u64)n * 200;
    for (;;) {
        u32 s = mmio_read(BSC_S);
        if (rd) while ((s & S_RXD) && i < n) { buf[i++] = (u8)mmio_read(BSC_FIFO); s = mmio_read(BSC_S); }
        else while ((s & S_TXD) && i < n) { mmio_write(BSC_FIFO, buf[i++]); s = mmio_read(BSC_S); }
        if (s & (S_ERR | S_CLKT)) {
            last_s = s;
            bsc_settle();                                 /* let the STOP go out before anything else */
            return (s & S_ERR) ? -1 : -2;
        }
        if (s & S_DONE) {
            delay_us(30);                                 /* 0.68: a late ERR (no ACK) must not pass as done */
            last_s = s | mmio_read(BSC_S);
            if (last_s & (S_ERR | S_CLKT)) { bsc_settle(); return -1; }
            if (rd) while ((mmio_read(BSC_S) & S_RXD) && i < n) buf[i++] = (u8)mmio_read(BSC_FIFO);
            mmio_write(BSC_S, S_DONE);
            return i == n ? 0 : -2;
        }
        if (timer_us() > end) { last_s = s; mmio_write(BSC_C, 0); delay_us(10); bsc_settle(); return -2; }
    }
}

int i2c_write(u8 addr, const void *buf, int n)
{
    i2c_init();
    lk();
    int r = xfer(addr, (u8 *)buf, n, 0);
    ulk();
    return r;
}

int i2c_read(u8 addr, void *buf, int n)
{
    i2c_init();
    lk();
    int r = xfer(addr, buf, n, 1);
    ulk();
    return r;
}

/* write a register number, then read n bytes (a stop in between: fine for the usual sensors) */
int i2c_read_reg(u8 addr, u8 reg, void *buf, int n)
{
    i2c_init();
    lk();
    int r = xfer(addr, &reg, 1, 0);
    if (!r) r = xfer(addr, buf, n, 1);
    ulk();
    return r;
}

int i2c_write_reg(u8 addr, u8 reg, const void *data, int n)
{
    u8 b[17];
    if (n > 16) return -3;
    b[0] = reg;
    memcpy(b + 1, data, (size_t)n);
    return i2c_write(addr, b, n + 1);
}

/* write WITHOUT looking at the ACK bits, bit-banged on GPIO2/3 (~90 kHz): for displays that never
 * acknowledge (2.42" SSD1309 OLED boards with the D2 jumper open - ESP / Arduino software I2C ignores
 * ACK, but the BSC stops at a missing address ACK). Open drain: low = output 0, high = input (the Pi's
 * 1.8k pull-ups). The pins go back to ALT0 (BSC1) afterwards. */
static void bb_byte(u8 v)
{
    for (int i = 7; i >= 0; i--) { bb(2, (v >> i) & 1); delay_us(3); bb(3, 1); delay_us(5); bb(3, 0); delay_us(2); }
    bb(2, 1); delay_us(3); bb(3, 1); delay_us(5); bb(3, 0); delay_us(2);          /* the ACK clock: not read */
}
int i2c_write_noack(u8 addr, const void *buf, int n)
{
    const u8 *b = buf;
    i2c_init();
    lk();
    bb(2, 1); bb(3, 1); delay_us(5);
    bb(2, 0); delay_us(5); bb(3, 0); delay_us(3);                                 /* START */
    bb_byte((u8)(addr << 1));
    for (int i = 0; i < n; i++) bb_byte(b[i]);
    bb(2, 0); delay_us(3); bb(3, 1); delay_us(5); bb(2, 1); delay_us(5);          /* STOP */
    gpio_func(2, 4); gpio_func(3, 4);
    mmio_write(BSC_S, S_DONE | S_ERR | S_CLKT);
    ulk();
    return 0;
}

/* OLED controllers (SSD1306/SSD1309/SH1106) are write-only on I2C: they may not ACK a READ address.
 * Probe them with a one-byte write of 0x00 = a control byte with nothing after it (harmless).
 * Only used for the display addresses - an unknown chip could take the byte as a register number. */
int i2c_probe_write(u8 addr)
{
    u8 z = 0;
    /* 0.68: with NOTHING on the bus the owner's 3B+ still "found" 0x3C and 0x3D this way. So first a
     * write to 0x7C (a reserved address no chip may use): if that is "acknowledged" too, write
     * probes cannot be trusted on this bus and say no */
    for (int k = 0; k < 3; k++) {                         /* 0.69: three times in a row, or it is noise */
        if (i2c_write(0x7C, &z, 1) == 0) return 0;
        if (i2c_write(addr, &z, 1) != 0) return 0;
    }
    return 1;
}

/* the line levels while idle (both should be high: the Pi's 1.8k pull-ups) */
int i2c_lines(void) { i2c_init(); return (gpio_get(2) ? 1 : 0) | (gpio_get(3) ? 2 : 0); }

/* is somebody at addr? (a one-byte read is answered by every device) */
int i2c_probe(u8 addr)
{
    u8 b;
    return i2c_read(addr, &b, 1) == 0;
}

static const char *guess(u8 a)
{
    switch (a) {
    case 0x76: case 0x77: return "BME280 / BMP280 / BMP180 (temperature, humidity, pressure)";
    case 0x38: return "AHT20 / AHT10 (humidity, temperature)";
    case 0x64: return "DFRobot DF2301Q offline voice recognition";
    case 0x48: case 0x49: case 0x4A: case 0x4B: return "ADS1115 (ADC) or a TMP102 / LM75 thermometer";
    case 0x68: return "DS3231 / DS1307 (clock) or an MPU-6050 (see 'motion')";
    case 0x69: return "MPU-6050 with AD0 high (see 'motion')";
    case 0x57: return "AT24C32 EEPROM (on DS3231 boards)";
    case 0x3C: case 0x3D: return "SSD1306 / SH1106 OLED display (see 'display')";
    case 0x27: case 0x3F: return "PCF8574 LCD backpack (see 'display')";
    case 0x44: case 0x45: return "SHT3x (temperature, humidity)";
    case 0x40: return "INA219 / HTU21D / Si7021";
    case 0x23: return "BH1750 (light)";
    case 0x50: return "EEPROM (HAT ID)";
    case 0x10: case 0x11: return "RDA5807M FM radio (see 'radio')";
    }
    return "";
}

void i2c_cmd(int argc, char **argv)
{
    u64 a = 0, r = 0, v = 0;
    if (argc > 1 && !strcmp(argv[1], "scan")) {
        int n = 0;
        i2c_init();
        kprintf("I2C on GPIO2 (SDA) / GPIO3 (SCL), %u kHz (core clock %u MHz):\n", speed / 1000, core_hz / 1000000);
        lk(); recovered = recover_l(); ulk();
        if (recovered > 0) kprintf("  (SDA was held low by a stuck chip - freed with %d clock pulse%s)\n", recovered, recovered > 1 ? "s" : "");
        if (recovered < 0) kprintf("  !! SDA stays low even after 9 clock pulses - a short to GND or a broken module\n");
        int l = i2c_lines();
        if (l != 3) kprintf("  !! idle lines: SDA %s, SCL %s - should both be high (a short, SDA/SCL swapped,\n"
                            "     or a module without power pulling the line down)\n", l & 1 ? "high" : "LOW", l & 2 ? "high" : "LOW");
        { u8 z = 0; if (i2c_write(0x7C, &z, 1) == 0) kprintf("  !! even the reserved address 0x7c answers a write - the bus is not reliable\n"); }
        for (int ad = 0x08; ad < 0x78; ad++)
            {
                int rd = i2c_probe((u8)ad), wr = !rd && (ad == 0x3C || ad == 0x3D) && i2c_probe_write((u8)ad);
                if (rd || wr) { kprintf("  0x%02x  %s%s\n", ad, guess((u8)ad), wr ? "  [answers writes only]" : ""); n++; }
            }
        kprintf("%d device(s)\n", n);
        return;
    }
    if (argc > 3 && !strcmp(argv[1], "read") && parse_num(argv[2], &a) && parse_num(argv[3], &r)) {
        u64 cnt = 1;
        if (argc > 4) parse_num(argv[4], &cnt);
        if (cnt < 1 || cnt > 16) cnt = 1;
        u8 b[16];
        int e = i2c_read_reg((u8)a, (u8)r, b, (int)cnt);
        if (e) { kprintf("0x%02x: %s\n", (u32)a, e == -1 ? "no answer" : "timeout"); return; }
        kprintf("0x%02x reg 0x%02x:", (u32)a, (u32)r);
        for (int i = 0; i < (int)cnt; i++) kprintf(" %02x", b[i]);
        kprintf("\n");
        return;
    }
    if (argc > 4 && !strcmp(argv[1], "write") && parse_num(argv[2], &a) && parse_num(argv[3], &r) && parse_num(argv[4], &v)) {
        u8 b = (u8)v;
        int e = i2c_write_reg((u8)a, (u8)r, &b, 1);
        kprintf("%s\n", e ? (e == -1 ? "no answer" : "timeout") : "ok");
        return;
    }
    if (argc > 2 && !strcmp(argv[1], "speed") && parse_num(argv[2], &a) && a >= 10 && a <= 400) {
        speed = (u32)a * 1000; ready = 0; i2c_init();
        kprintf("I2C %u kHz (core clock %u MHz, divider %u) - until the next boot\n", (u32)a, core_hz / 1000000, core_hz / speed);
        return;
    }
    if (argc > 2 && !strcmp(argv[1], "probe") && parse_num(argv[2], &a) && a < 0x80) {
        i2c_init();
        kprintf("I2C %u kHz, core clock %u MHz, divider %u; lines SDA %s SCL %s\n", speed / 1000, core_hz / 1000000,
                core_hz / speed, gpio_get(2) ? "high" : "LOW", gpio_get(3) ? "high" : "LOW");
        for (int k = 0; k < 3; k++) {
            u8 b = 0, z = 0;
            int e = i2c_read((u8)a, &b, 1);
            kprintf("  read  0x%02x: %-9s status 0x%03x, %u us%s", (u32)a, e == 0 ? "ANSWERED" : e == -1 ? "no ACK" : "timeout",
                    last_s, last_us, e == 0 ? "" : "\n");
            if (e == 0) kprintf(", byte 0x%02x\n", (u32)b);
            e = i2c_write((u8)a, &z, 1);
            kprintf("  write 0x%02x: %-9s status 0x%03x, %u us\n", (u32)a, e == 0 ? "ANSWERED" : e == -1 ? "no ACK" : "timeout", last_s, last_us);
        }
        return;
    }
    kprintf("usage: i2c scan | probe <addr> | speed <kHz> | read <addr> <reg> [count] | write <addr> <reg> <value>   (0x.. hex)\n");
}
