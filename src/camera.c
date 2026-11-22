/*
 * camera.c - Raspberry Pi camera on the CSI ribbon cable, step 1 (0.88): power + sensor detection.
 *
 * The camera's sensor is set up over its own I2C bus, not the GPIO2/3 one: BSC0 (0x3F205000) on
 * GPIO44 = SDA0 / GPIO45 = SCL0 (ALT1). The camera board gets power (and the sensor leaves shutdown)
 * through CAM_GPIO0: the firmware GPIO expander pin 5 (= 133) on the 3B / 3B+ / 3A+, GPIO40 on the
 * Zero 2 W (Linux device trees bcm2710-rpi-3-b(-plus) / -zero-2-w: cam1_reg, i2c0mux_0_44).
 * The camera board has its own 25 MHz oscillator, so nothing else is needed to talk to the sensor.
 * OV5647 (camera v1, 5 MP) at 0x36: 16-bit register addresses, chip id 0x300A/0x300B = 0x56 0x47.
 * IMX219 (camera v2, 8 MP) at 0x10: chip id 0x0000/0x0001 = 0x02 0x19.
 * Next steps: sensor setup + the Unicam CSI-2 receiver (0x3F801000) -> RAW Bayer in RAM -> colours.
 */
#include "kernel.h"

#define B0        (MMIO_BASE + 0x205000)
#define B_C       (B0 + 0x00)
#define B_S       (B0 + 0x04)
#define B_DLEN    (B0 + 0x08)
#define B_A       (B0 + 0x0C)
#define B_FIFO    (B0 + 0x10)
#define B_DIV     (B0 + 0x14)
#define B_CLKT    (B0 + 0x1C)
#define C_I2CEN   (1u << 15)
#define C_ST      (1u << 7)
#define C_CLEAR   (3u << 4)
#define C_READ    1u
#define S_TA      1u
#define S_DONE    2u
#define S_TXD     (1u << 4)
#define S_RXD     (1u << 5)
#define S_ERR     (1u << 8)
#define S_CLKT    (1u << 9)

static int powered, bus_ready, sensor;        /* sensor: 0 none, 1 OV5647, 2 IMX219 */
static u8 saddr;
static u32 last_s;

static void settle(void)
{
    u64 end = timer_us() + 2000;
    while ((mmio_read(B_S) & S_TA) && timer_us() < end) ;
    if (mmio_read(B_S) & S_TA) { mmio_write(B_C, 0); delay_us(10); }
    mmio_write(B_S, S_DONE | S_ERR | S_CLKT);
    mmio_write(B_C, C_I2CEN | C_CLEAR);
}

/* one BSC0 transfer: 0 ok, -1 NACK, -2 timeout */
static int xfer(u8 addr, u8 *buf, int n, int rd)
{
    int i = 0;
    if (mmio_read(B_S) & S_TA) settle();
    mmio_write(B_C, C_I2CEN | C_CLEAR);
    mmio_write(B_S, S_DONE | S_ERR | S_CLKT);
    mmio_write(B_A, addr);
    mmio_write(B_DLEN, (u32)n);
    if (!rd) while (i < n && i < 16) mmio_write(B_FIFO, buf[i++]);
    mmio_write(B_C, C_I2CEN | C_ST | (rd ? C_READ : 0));
    u64 end = timer_us() + 5000 + (u64)n * 200;
    for (;;) {
        u32 s = mmio_read(B_S);
        if (rd) while ((s & S_RXD) && i < n) { buf[i++] = (u8)mmio_read(B_FIFO); s = mmio_read(B_S); }
        else while ((s & S_TXD) && i < n) { mmio_write(B_FIFO, buf[i++]); s = mmio_read(B_S); }
        if (s & (S_ERR | S_CLKT)) { last_s = s; settle(); return (s & S_ERR) ? -1 : -2; }
        if (s & S_DONE) {
            delay_us(30);
            last_s = s | mmio_read(B_S);
            if (last_s & (S_ERR | S_CLKT)) { settle(); return -1; }
            if (rd) while ((mmio_read(B_S) & S_RXD) && i < n) buf[i++] = (u8)mmio_read(B_FIFO);
            mmio_write(B_S, S_DONE);
            return i == n ? 0 : -2;
        }
        if (timer_us() > end) { last_s = s; mmio_write(B_C, 0); delay_us(10); settle(); return -2; }
    }
}

static void bus_init(void)
{
    if (bus_ready) return;
    gpio_func(44, 5);                         /* ALT1 = SDA0 */
    gpio_func(45, 5);                         /* ALT1 = SCL0 */
    gpio_pull(44, 2);
    gpio_pull(45, 2);
    u32 clk[2] = { 4, 0 };
    u32 core = mbox_prop(0x00030002, clk, 2) && clk[1] ? clk[1] : 250000000;
    mmio_write(B_DIV, core / 100000);
    mmio_write(B_CLKT, 0x40);
    mmio_write(B_C, C_I2CEN | C_CLEAR);
    mmio_write(B_S, S_DONE | S_ERR | S_CLKT);
    bus_ready = 1;
}

/* sensor registers: 16-bit address, big endian; write the address, STOP, then read */
static int rd16(u8 a, u16 reg, u8 *v)
{
    u8 b[2] = { (u8)(reg >> 8), (u8)reg };
    int r = xfer(a, b, 2, 0);
    if (r) return r;
    return xfer(a, v, 1, 1);
}
static int wr16(u8 a, u16 reg, u8 v)
{
    u8 b[3] = { (u8)(reg >> 8), (u8)reg, v };
    return xfer(a, b, 3, 0);
}

static void power(int on)
{
    if (board_type() == 0x12) {               /* Zero 2 W: CAM_GPIO0 = GPIO40 */
        gpio_func(40, 1);
        gpio_set(40, on);
    } else {                                  /* firmware expander pin 5 = CAM_GPIO0 */
        u32 v[2] = { 133, (u32)on };
        mbox_prop(0x00038041, v, 2);
    }
    powered = on;
}

static const char *sname(void) { return sensor == 1 ? "OV5647 (camera v1, 5 MP)" : sensor == 2 ? "IMX219 (camera v2, 8 MP)" : "none"; }

/* power the camera, find the sensor; 1 = found */
static int detect(int verbose)
{
    bus_init();
    if (!powered) {
        power(0); delay_us(10000);
        power(1); delay_us(50000);            /* Linux: startup delay 20 ms */
    }
    sensor = 0; saddr = 0;
    u8 h = 0, l = 0;
    int r = rd16(0x36, 0x300A, &h);
    if (!r) r = rd16(0x36, 0x300B, &l);
    if (verbose) kprintf("  0x36 (OV5647):  %s", r == 0 ? "answers" : r == -1 ? "no answer" : "timeout");
    if (!r) {
        if (verbose) kprintf(", chip id %02x%02x", h, l);
        if (h == 0x56 && l == 0x47) { sensor = 1; saddr = 0x36; }
    }
    if (verbose) kprintf("\n");
    if (!sensor) {
        r = rd16(0x10, 0x0000, &h);
        if (!r) r = rd16(0x10, 0x0001, &l);
        if (verbose) kprintf("  0x10 (IMX219):  %s", r == 0 ? "answers" : r == -1 ? "no answer" : "timeout");
        if (!r) {
            if (verbose) kprintf(", chip id %02x%02x", h, l);
            if (h == 0x02 && l == 0x19) { sensor = 2; saddr = 0x10; }
        }
        if (verbose) kprintf("\n");
    }
    return sensor != 0;
}

int camera_present(void) { return sensor != 0; }

void camera_cmd(int argc, char **argv)
{
    const char *a = argc > 1 ? argv[1] : "";
    if (!*a) {
        kprintf("camera on the CSI cable: power %s, I2C0 on GPIO44/45\n",
                board_type() == 0x12 ? "GPIO40" : "firmware expander pin 5");
        detect(1);
        kprintf("  lines: SDA %s, SCL %s, last BSC0 status 0x%x\n",
                gpio_get(44) ? "high" : "LOW", gpio_get(45) ? "high" : "LOW", last_s);
        if (sensor) {
            kprintf("found: %s at 0x%02x\n", sname(), saddr);
            if (sensor == 1) {
                u8 v;
                if (!rd16(saddr, 0x0100, &v)) kprintf("  0x0100 mode select %02x (%s)\n", v, v & 1 ? "streaming" : "standby");
                if (!rd16(saddr, 0x302A, &v)) kprintf("  0x302A revision    %02x\n", v);
            }
            kprintf("(picture: not yet - next version)\n");
        } else {
            kprintf("no sensor: check the ribbon (contacts towards the HDMI port on the Pi 3,\n"
                    "the blue side towards the Ethernet port), and that it is the CAMERA socket\n");
        }
        return;
    }
    if (!strcmp(a, "off")) { power(0); sensor = 0; kprintf("camera power off\n"); return; }
    if (!strcmp(a, "reg") && argc > 2) {
        u64 reg, v;
        if (!parse_num(argv[2], &reg) || reg > 0xFFFF) { kprintf("cam reg <addr 0x0000-0xffff> [value]\n"); return; }
        if (!sensor && !detect(0)) { kprintf("no sensor found\n"); return; }
        if (argc > 3) {
            if (!parse_num(argv[3], &v) || v > 255) { kprintf("value 0-255\n"); return; }
            int r = wr16(saddr, (u16)reg, (u8)v);
            kprintf("0x%04x <- 0x%02x %s\n", (u32)reg, (u32)v, r ? "FAILED" : "ok");
        } else {
            u8 b;
            int r = rd16(saddr, (u16)reg, &b);
            if (r) kprintf("0x%04x: %s\n", (u32)reg, r == -1 ? "no answer" : "timeout");
            else kprintf("0x%04x = 0x%02x\n", (u32)reg, b);
        }
        return;
    }
    kprintf("usage: cam                 power the camera, find the sensor (OV5647 / IMX219)\n"
            "       cam reg <a> [v]     read / write a sensor register (16-bit address)\n"
            "       cam off             camera power off\n");
}
