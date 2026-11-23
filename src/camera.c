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
 *
 * Step 2 (0.89): a picture. Order and values as the Linux drivers ov5647.c + bcm2835-unicam.c (own code):
 *   power domain UNICAM1 (mailbox 0x38030, domain 13), CAM1 clock 100 MHz (CM_CAM1CTL/DIV, PLLD 500 / 5),
 *   sensor output enable + stream off (lanes to LP-11), Unicam CSI1 (0x3F801000, clock gate 0x3F802004):
 *   2 data lanes, CSI-2, non-continuous clock (the dts says clock-noncontinuous), data type 0x2B RAW10
 *   unpacked to 16 bit per pixel, image buffer by bus address (| 0xC0000000); then the sensor: common
 *   registers + the 640x480 2x2-binned mode (60 fps, BGGR), its own auto exposure / gain / white balance,
 *   MIPI_CTRL00 0x34, stream on. Frames are found by polling ISTA (frame start / end), two buffers.
 *   Colours: one RGB pixel per 2x2 Bayer square (320x240), grey-world white balance, drawn 2x on HDMI.
 */
#include "kernel.h"
#include "sd.h"
#include "jpeg.h"

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

/* step 2 */
#define UNI       (MMIO_BASE + 0x801000)
#define UNI_GATE  (MMIO_BASE + 0x802004)
#define CM_CAM1CTL (MMIO_BASE + 0x101048)
#define CM_CAM1DIV (MMIO_BASE + 0x10104C)
static int W = 1296, H = 972;                          /* 0.99: the Linux 1296x972 mode by default (works under Linux on the same board) */
#define FRAME_BYTES ((u32)(W * H * 2))
#define BUF_A 0x15000000UL                                /* 0.109: 8 MB each (1920x1080 x 2 B = 4.1 MB) */
#define BUF_B 0x15800000UL
static int streaming, cur_buf;
static int web_stream;                                   /* 0.104: started by a web request */
static int fake;                                         /* 0.104: `cam test on` = a made-up picture (QEMU, no camera) */
/* 0.106: IR light (LEDs / IR lamp through a transistor or relay) on a GPIO: off, on, photo (while the camera
 * runs), auto (while the camera runs AND it is dark: exposure at its limit and gain >= 8x; off again when
 * bright even with the light: exposure < 1/4 of the limit at gain 1x; 20 s between changes) */
static int ir_pin, ir_mode, ir_low, ir_now = -1, ir_dark;
static u64 ir_changed;
static void ir_apply(void)
{
    if (!ir_pin) return;
    int want = ir_mode == 1 || (ir_mode == 2 && streaming) || (ir_mode == 3 && streaming && ir_dark);
    if (want == ir_now) return;
    gpio_func(ir_pin, 1);
    gpio_set(ir_pin, want ^ ir_low);
    ir_now = want;
}
static u32 fake_n;
static int fake_dark;                                    /* `cam test dark`: a night scene (the IR light makes it 15x brighter) */
static u32 n_fs, n_fe, sta_or;

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

/* ---------------- step 2: Unicam + OV5647 640x480 ---------------- */

static inline void uw(u32 off, u32 v) { mmio_write(UNI + off, v); }
static inline u32 ur(u32 off) { return mmio_read(UNI + off); }
static void uwf(u32 off, u32 mask, u32 v)
{
    u32 sh = 0; while (!((mask >> sh) & 1)) sh++;
    uw(off, (ur(off) & ~mask) | ((v << sh) & mask));
}

struct rv { u16 r; u8 v; };
static const struct rv oe_on[] = { {0x3000,0x0f},{0x3001,0xff},{0x3002,0xe4} };
static const struct rv common[] = {
    {0x0100,0x00},{0x0103,0x01},{0x3034,0x1a},{0x3035,0x21},{0x303c,0x11},{0x3106,0xf5},{0x3827,0xec},
    {0x370c,0x03},{0x5000,0x06},{0x5003,0x08},{0x5a00,0x08},{0x3000,0x00},{0x3001,0x00},{0x3002,0x00},
    {0x3016,0x08},{0x3017,0xe0},{0x3018,0x44},{0x301c,0xf8},{0x301d,0xf0},{0x3a18,0x00},{0x3a19,0xf8},
    {0x3c01,0x80},{0x3b07,0x0c},{0x3630,0x2e},{0x3632,0xe2},{0x3633,0x23},{0x3634,0x44},{0x3636,0x06},
    {0x3620,0x64},{0x3621,0xe0},{0x3600,0x37},{0x3704,0xa0},{0x3703,0x5a},{0x3715,0x78},{0x3717,0x01},
    {0x3731,0x02},{0x370b,0x60},{0x3705,0x1a},{0x3f05,0x02},{0x3f06,0x10},{0x3f01,0x0a},{0x3a08,0x01},
    {0x3a0f,0x58},{0x3a10,0x50},{0x3a1b,0x58},{0x3a1e,0x50},{0x3a11,0x60},{0x3a1f,0x28},{0x4001,0x02},
    {0x4000,0x09},{0x3503,0x03},
};
static const struct rv binned[] = {                     /* 1296x972, 2x2 binned, full field of view (Linux mode 2) */
    {0x3036,0x69},{0x3821,0x01},{0x3820,0x41},{0x3612,0x59},{0x3618,0x00},{0x5002,0x41},{0x3800,0x00},
    {0x3801,0x00},{0x3802,0x00},{0x3803,0x00},{0x3804,0x0a},{0x3805,0x3f},{0x3806,0x07},{0x3807,0xa3},
    {0x3808,0x05},{0x3809,0x10},{0x380a,0x03},{0x380b,0xcc},{0x3811,0x0c},{0x3813,0x06},{0x3814,0x31},
    {0x3815,0x31},{0x3a09,0x28},{0x3a0a,0x00},{0x3a0b,0xf6},{0x3a0d,0x08},{0x3a0e,0x06},{0x4004,0x04},
    {0x4837,0x16},{0x4800,0x24},{0x350a,0x00},{0x350b,0x10},{0x3500,0x00},{0x3501,0x1a},{0x3502,0xf0},
    {0x3212,0xa0},
    {0x380c,0x07},{0x380d,0x68},{0x380e,0x05},{0x380f,0x9b},          /* HTS 1896, VTS 0x59b (mode values) */
};
/* 0.94: the register list the Pi firmware writes for 1296x976 (2x2 binned), as logged off the I2C bus
 * (raspiraw ov5647_modes.h mode 4, BSD licence, Raspberry Pi Foundation / Dave Stevenson). Note 0x4800 = 0x24:
 * the firmware never turns on line sync packets (0x34), which Linux and our 0.89-0.93 did. */
static const struct rv fwmode[] = {
    {0x0100,0x00},{0x0103,0x01},{0x3034,0x1A},{0x3035,0x21},{0x3036,0x62},{0x303C,0x11},{0x3106,0xF5},
    {0x3827,0xEC},{0x370C,0x03},{0x3612,0x59},{0x3618,0x00},{0x5000,0x06},{0x5002,0x40},{0x5003,0x08},
    {0x5A00,0x08},{0x3000,0x00},{0x3001,0x00},{0x3002,0x00},{0x3016,0x08},{0x3017,0xE0},{0x3018,0x44},
    {0x301C,0xF8},{0x301D,0xF0},{0x3A18,0x00},{0x3A19,0xF8},{0x3C01,0x80},{0x3B07,0x0C},{0x3800,0x00},
    {0x3801,0x00},{0x3802,0x00},{0x3803,0x00},{0x3804,0x0A},{0x3805,0x3F},{0x3806,0x07},{0x3807,0xA3},
    {0x3808,0x05},{0x3809,0x10},{0x380A,0x03},{0x380B,0xCC},{0x380C,0x07},{0x380D,0x68},{0x3811,0x10},
    {0x3813,0x06},{0x3814,0x31},{0x3815,0x31},{0x3630,0x2E},{0x3632,0xE2},{0x3633,0x23},{0x3634,0x44},
    {0x3636,0x06},{0x3620,0x64},{0x3621,0xE0},{0x3600,0x37},{0x3704,0xA0},{0x3703,0x5A},{0x3715,0x78},
    {0x3717,0x01},{0x3731,0x02},{0x370B,0x60},{0x3705,0x1A},{0x3F05,0x02},{0x3F06,0x10},{0x3F01,0x0A},
    {0x3A08,0x01},{0x3A09,0x28},{0x3A0A,0x00},{0x3A0B,0xF6},{0x3A0D,0x08},{0x3A0E,0x06},{0x3A0F,0x58},
    {0x3A10,0x50},{0x3A1B,0x58},{0x3A1E,0x50},{0x3A11,0x60},{0x3A1F,0x28},{0x4001,0x02},{0x4004,0x04},
    {0x4000,0x09},{0x4837,0x16},{0x4800,0x24},{0x3503,0x03},{0x3820,0x41},{0x3821,0x03},{0x380E,0x05},
    {0x380F,0x9B},{0x350A,0x00},{0x350B,0x10},{0x3500,0x00},{0x3501,0x1A},{0x3502,0xF0},{0x3212,0xA0},
};
/* 0.109: 1920x1080 30 fps, the centre of the full sensor without binning (Linux ov5647 mode 1) - the
 * full screen picture of the microscope, one sensor pixel per screen pixel on a 1080p screen */
static const struct rv hd1080[] = {
    {0x3036,0x69},{0x3821,0x02},{0x3820,0x00},{0x3612,0x5b},{0x3618,0x04},{0x5002,0x41},{0x3814,0x11},
    {0x3815,0x11},{0x3708,0x64},{0x3709,0x12},{0x3800,0x01},{0x3801,0x5c},{0x3802,0x01},{0x3803,0xb2},
    {0x3804,0x08},{0x3805,0xe3},{0x3806,0x05},{0x3807,0xf1},{0x3808,0x07},{0x3809,0x80},{0x380a,0x04},
    {0x380b,0x38},{0x3811,0x04},{0x3813,0x02},{0x3a09,0x4b},{0x3a0a,0x01},{0x3a0b,0x13},{0x3a0d,0x04},
    {0x3a0e,0x03},{0x4004,0x04},{0x4837,0x19},
    {0x380c,0x09},{0x380d,0x70},{0x380e,0x04},{0x380f,0x50},          /* HTS 2416, VTS 0x450 */
};
static int vga_mode = 0, gate_val = 0x15;             /* vga_mode: 2 = firmware 1296x976, 0 = Linux 1296x972, 1 = 640x480 */                /* 0.93: gate 0x155 as the firmware writes it (Linux: 0x15) */
static const struct rv vga[] = {
    {0x3036,0x46},{0x3821,0x01},{0x3820,0x41},{0x3612,0x59},{0x3618,0x00},{0x3814,0x35},{0x3815,0x35},
    {0x3708,0x64},{0x3709,0x52},{0x3800,0x00},{0x3801,0x10},{0x3802,0x00},{0x3803,0x00},{0x3804,0x0a},
    {0x3805,0x2f},{0x3806,0x07},{0x3807,0x9f},{0x3808,0x02},{0x3809,0x80},{0x380a,0x01},{0x380b,0xe0},
    {0x3a09,0x2e},{0x3a0a,0x00},{0x3a0b,0xfb},{0x3a0d,0x02},{0x3a0e,0x01},{0x4004,0x02},{0x4800,0x34},
    {0x380c,0x07},{0x380d,0x3c},{0x380e,0x01},{0x380f,0xf8},          /* HTS 1852, VTS 0x1f8 (mode values) */
};
static int bayer = -1;   /* 0.102: -1 auto (GBRG when the 1296 mode mirrors with 0x3821 = 3, like Linux), 0 BGGR, 1 GBRG, 2 GRBG, 3 RGGB */
static int test_pattern, cont_clk;                    /* 0.91: cont_clk = continuous MIPI clock (test) */

static int wr_list(const struct rv *l, int n)
{
    for (int i = 0; i < n; i++) {
        if (wr16(saddr, l[i].r, l[i].v)) return -1;
        if (l[i].r == 0x0103) delay_us(5000);            /* software reset */
    }
    return 0;
}

static int sensor_stream(int on)
{
    if (on && vga_mode == 2) {                           /* exactly the firmware's list, then only the extras */
        if (wr_list(fwmode, sizeof(fwmode) / sizeof(fwmode[0]))) return -1;
        wr16(saddr, 0x503D, test_pattern ? 0x80 : 0x00);
        wr16(saddr, 0x3503, 0x00);                         /* auto exposure + gain */
        wr16(saddr, 0x5001, 0x01);                         /* auto white balance */
        wr16(saddr, 0x0100, 0x01);
        /* 0.96: undo our stream_off - 0x4202 (frames off = 0x0f) and 0x300d survive the 0x0103 reset
         * (Linux writes them after its mode list too); without this the sensor sent only the clock */
        wr16(saddr, 0x4202, 0x00);
        return wr16(saddr, 0x300D, 0x00);
    }
    if (on) {
        if (wr_list(common, sizeof(common) / sizeof(common[0]))) return -1;
        if (vga_mode == 3 ? wr_list(hd1080, sizeof(hd1080) / sizeof(hd1080[0])) :
            vga_mode ? wr_list(vga, sizeof(vga) / sizeof(vga[0])) : wr_list(binned, sizeof(binned) / sizeof(binned[0]))) return -1;
        u8 v = 0;
        if (!rd16(saddr, 0x4814, &v)) wr16(saddr, 0x4814, v & 0x3F);   /* virtual channel 0 */
        if (vga_mode == 3) {                               /* like below, with this mode's frame length */
            static const struct rv lx3[] = { {0x3503,0x03},{0x5001,0x00},{0x350a,0x00},{0x350b,0x40},{0x380e,0x04},{0x380f,0x50} };
            wr_list(lx3, sizeof(lx3) / sizeof(lx3[0]));
        } else if (vga_mode == 0) {                        /* 0.100: exactly what Linux leaves in the sensor (owner's dump, same board) */
            static const struct rv lx[] = { {0x3503,0x03},{0x5001,0x00},{0x3500,0x00},{0x3501,0x57},{0x3502,0xe0},
                                            {0x350a,0x00},{0x350b,0x40},{0x380e,0x06},{0x380f,0x02},{0x3821,0x03} };
            wr_list(lx, sizeof(lx) / sizeof(lx[0]));
        } else {
            wr16(saddr, 0x3503, 0x00);                     /* auto exposure + auto gain */
            wr16(saddr, 0x5001, 0x01);                     /* auto white balance */
        }
        wr16(saddr, 0x503D, test_pattern ? 0x80 : 0x00);   /* colour bars */
        wr16(saddr, 0x0100, 0x01);
        wr16(saddr, 0x4800, cont_clk ? 0x04 : 0x34);       /* bus idle (+ clock gate + line sync when non-continuous) */
        wr16(saddr, 0x4202, 0x00);
        return wr16(saddr, 0x300D, 0x00);
    }
    wr16(saddr, 0x4800, 0x25);
    wr16(saddr, 0x4202, 0x0f);
    return wr16(saddr, 0x300D, 0x01);
}

static void cam1_clock(int on)
{
    u64 end;
    mmio_write(CM_CAM1CTL, 0x5a000000 | (mmio_read(CM_CAM1CTL) & 0xFFFF & ~0x10u));
    end = timer_us() + 10000;
    while ((mmio_read(CM_CAM1CTL) & 0x80) && timer_us() < end) ;
    if (!on) return;
    mmio_write(CM_CAM1DIV, 0x5a000000 | (5u << 12));            /* PLLD 500 MHz / 5 = 100 MHz */
    mmio_write(CM_CAM1CTL, 0x5a000000 | 6);                     /* source PLLD */
    mmio_write(CM_CAM1CTL, 0x5a000000 | 6 | 0x10);              /* enable */
    end = timer_us() + 10000;
    while (!(mmio_read(CM_CAM1CTL) & 0x80) && timer_us() < end) ;
}

static void set_buf(u64 a)
{
    uw(0x110, (u32)a | 0xC0000000u);                     /* IBSA0 */
    uw(0x114, ((u32)a + FRAME_BYTES) | 0xC0000000u);     /* IBEA0 */
}

static void unicam_start(void)
{
    mmio_write(UNI_GATE, 0x5a000000 | (u32)gate_val);    /* lane clock gates (0x15 = clock + 2 lanes) */
    uw(0x000, 2);                                        /* CTRL = MEM */
    uw(0x008, 4 | (7 << 4) | (7 << 8));                  /* ANA: reset, CTATADJ 7, PTATADJ 7 */
    delay_us(1500);
    uwf(0x008, 4, 0);                                    /* out of reset */
    uwf(0x000, 4, 1); uwf(0x000, 4, 0);                  /* peripheral reset */
    uwf(0x000, 1, 0);                                    /* CPE off */
    u32 v = ur(0x000);
    v &= ~((1u << 3) | (1u << 5) | (0xFu << 8) | (0x1FFu << 12));   /* CSI-2, strobe */
    v |= (0xFu << 8) | (128u << 12);                     /* packet framer timeout, OET */
    uw(0x000, v);
    uw(0x120, 0); uw(0x128, 0);                          /* IHWIN, IVWIN */
    v = ur(0x00C);
    v &= ~(0x3FFFFu);
    v |= (0xEu << 8) | (8u << 4) | (2u << 1) | 1;        /* PRI: PP 14, NP 8, PT 2, PE */
    uw(0x00C, v);
    uwf(0x008, 8, 0);                                    /* ANA DDL = 0 */
    uw(0x100, 1 | 2 | 4 | (128u << 16));                 /* ICTL: FSIE FEIE IBOB, LCIE 128 */
    uw(0x004, 0x1B6FCu);                                 /* clear STA (the Linux UNICAM_STA_MASK_ALL bits only) */
    uw(0x104, 7);                                        /* clear ISTA */
    uwf(0x014, 0xFF, 2); uwf(0x014, 0xFF00, 6);          /* CLT: tclk_term_en, tclk_settle */
    uwf(0x028, 0xFF, 2); uwf(0x028, 0xFF00, 6); uwf(0x028, 0xFF0000, 0);   /* DLT */
    uwf(0x000, 16, 0);                                   /* SOE = 0 */
    uw(0x02C, (1u << 31) | (1u << 9) | (1u << 8) | 1);   /* CMP0: PCE GI CPH, VC 0, DT 1 (frame end) */
    u32 lane = cont_clk ? (1 | 4 | 8 | 16) : (1 | 4);    /* enable + LP enable (+ HS + termination when continuous) */
    uw(0x010, lane);                                     /* CLK */
    uw(0x018, lane);                                     /* DAT0 */
    uw(0x01C, lane);                                     /* DAT1 */
    uw(0x118, (u32)W * 2);                               /* IBLS: bytes per line */
    cur_buf = 0;
    set_buf(BUF_A);
    uw(0x10C, 4 | (5u << 7));                            /* IPIPE: unpack RAW10, pack to 16 bit */
    uw(0x108, 0x2B);                                     /* IDI0: VC 0, data type RAW10 */
    uwf(0x400, 1u << 6, 1); uwf(0x400, 1u << 9, 1);      /* MISC FL0 FL1 */
    uwf(0x000, 1, 1);                                    /* CPE: enable */
    uwf(0x100, 3u << 5, 1);                              /* ICTL LIP: load the image pointers */
}

static void unicam_stop(void)
{
    uwf(0x008, 8, 1);                                    /* ANA DDL */
    uwf(0x000, 16, 1);                                   /* SOE: stop the output engine */
    uw(0x018, 0); uw(0x01C, 0);
    uw(0x010, 0);
    uwf(0x000, 4, 1); delay_us(60); uwf(0x000, 4, 0);  /* peripheral reset pulse (Linux unicam_disable) */
    uwf(0x000, 1, 0);
    uw(0x200, 0);
    mmio_write(UNI_GATE, 0x5a000000);
}

static const char *sta_names[] = { "SYN","CS","SBE","PBE","HOE","PLE","SSC","CRCE","OES","IFO","OFO","BFO","DL","PS","IS","PI0","PI1","FSI","FEI","LCI","B0R","B0N","B1R","B1N","DI" };
static void print_sta(u32 s)
{
    for (int i = 0; i < 25; i++) if (s & (1u << i)) kprintf(" %s", sta_names[i]);
}

static int sensor_first = 0;   /* 0.99: Linux order (receiver first) by default - Linux works on the same board */                            /* 0.95: start order, `cam order sensor|rx` */
static int trace = 0;                                    /* 0.90: print each step (a freeze shows where) */
#define STEP(...) do { if (trace) { kprintf(__VA_ARGS__); delay_us(30000); } } while (0)

static int cam_start(void)
{
    if (streaming) return 0;
    if (fake) { streaming = 1; n_fs = n_fe = sta_or = 0; ir_apply(); return 0; }
    if (!sensor && !detect(0)) { kprintf("no camera sensor found ('cam' shows why)\n"); return -1; }
    if (sensor != 1) { kprintf("only the OV5647 can give a picture so far\n"); return -1; }
    /* 0.101: the FIRMWARE domain number of UNICAM1 is 14 (Linux raspberrypi-power: DT index 13 + 1; Circle
     * DOMAIN_ID_UNICAM1 14). 0.89-0.100 powered 13 = UNICAM0 (the other receiver): CSI1's registers worked but
     * its analogue lane front end stayed unpowered = clock seen at most, never data sync / packets. */
    { u32 off[2] = { 13, 0 }; mbox_prop(0x00038030, off, 2); }   /* undo the stray UNICAM0 power-on of older versions */
    u32 g[2] = { 14, 0 };
    int ok = mbox_prop(0x00030030, g, 2);
    STEP("cam 1: power domain UNICAM1 was %s (%u)\n", ok ? (g[1] & 1 ? "on" : "off") : "?? (no answer)", g[1]);
    u32 pd[2] = { 14, 1 };
    ok = mbox_prop(0x00038030, pd, 2);
    g[0] = 14; g[1] = 0;
    int ok2 = mbox_prop(0x00030030, g, 2);
    STEP("cam 2: switched on: answer %s, now %s (%u)\n", ok ? "ok" : "NONE", ok2 ? (g[1] & 1 ? "on" : "OFF") : "?", g[1]);
    cam1_clock(1);
    STEP("cam 3: CAM1 clock ctl 0x%x div 0x%x\n", mmio_read(CM_CAM1CTL), mmio_read(CM_CAM1DIV));
    dcache_clean_inval((void *)BUF_A, 0x1000000);
    if (wr_list(oe_on, 3) || sensor_stream(0)) { kprintf("sensor does not answer\n"); return -1; }
    STEP("cam 4: sensor lanes in LP-11, reading the receiver...\n");
    STEP("cam 5: receiver CTRL 0x%x STA 0x%x ANA 0x%x\n", ur(0x000), ur(0x004), ur(0x008));
    delay_us(2000);
    if (sensor_first) {                                  /* 0.95: like rsta2/libcamera - sensor set up and streaming, then the receiver */
        if (sensor_stream(1)) { kprintf("sensor setup failed\n"); return -1; }
        STEP("cam 6: sensor streaming (set up first)\n");
        delay_us(20000);
        unicam_start();
        STEP("cam 7: receiver on: CTRL 0x%x STA 0x%x CLK 0x%x DAT0 0x%x, waiting for frames\n", ur(0x000), ur(0x004), ur(0x010), ur(0x018));
    } else {                                             /* Linux order: receiver first, then the sensor */
        unicam_start();
        STEP("cam 6: receiver on: CTRL 0x%x STA 0x%x CLK 0x%x DAT0 0x%x\n", ur(0x000), ur(0x004), ur(0x010), ur(0x018));
        if (sensor_stream(1)) { kprintf("sensor setup failed\n"); unicam_stop(); return -1; }
        STEP("cam 7: sensor streaming, waiting for frames: STA 0x%x ISTA 0x%x\n", ur(0x004), ur(0x104));
    }
    n_fs = n_fe = sta_or = 0;
    streaming = 1;
    ir_apply();
    return 0;
}

static void cam_stop(void)
{
    if (!streaming) return;
    if (fake) { streaming = 0; web_stream = 0; ir_apply(); return; }
    sensor_stream(0);
    unicam_stop();
    cam1_clock(0);
    { u32 pd[2] = { 14, 0 }; mbox_prop(0x00038030, pd, 2); }    /* UNICAM1 power off */
    streaming = 0;
    web_stream = 0;
    ir_apply();
}

/* wait for the next complete frame; returns its buffer or 0 (timeout ms) */
static int ae_exp_f(void);
/* 0.104 test picture: GBRG RAW10 colour bars + a grey ramp + a moving square, brightness follows exposure x gain */
static u16 *fake_frame(void)
{
    u16 *f = (u16 *)(cur_buf ? BUF_B : BUF_A);
    cur_buf ^= 1;
    u32 k = (u32)ae_exp_f();                              /* exposure x gain, 1406 x 64 = the Linux default */
    int sq = (int)(fake_n * 16 % (u32)(W - 200));
    fake_n++;
    for (int y = 0; y < H; y++) {
        u16 *row = f + y * W;
        for (int x = 0; x < W; x++) {
            int bar = x * 7 / W; u32 r, g, b;
            if (y < H / 2) { r = (bar & 4) ? 800 : 80; g = (bar & 2) ? 800 : 80; b = (bar & 1) ? 800 : 80; }
            else { r = g = b = (u32)x * 1000 / (u32)W; }
            if (x >= sq && x < sq + 160 && y >= H / 2 - 80 && y < H / 2 + 80) { r = 900; g = 600; b = 100; }
            r = r * 6 / 10;                                /* a colour cast for the white balance */
            u32 v = (y & 1) ? ((x & 1) ? g : r) : ((x & 1) ? b : g);   /* GBRG */
            v = (u32)((u64)v * k / (1406u * 64u) / 5);
            if (fake_dark) v = ir_now > 0 ? v * 3 / 4 : v / 20;
            row[x] = (u16)(v > 1023 ? 1023 : v);
        }
    }
    n_fs++; n_fe++;
    delay_us(20000);
    return f;
}

static u16 *next_frame(u32 ms)
{
    if (fake) return fake_frame();
    u64 end = timer_us() + (u64)ms * 1000;
    /* 0.110: the buffer being written is found from the write pointer at frame start, and the OTHER one is
     * queued for the next frame - so a frame we return is never written into while it is drawn (before, a
     * late poll that saw start + end together handed out a buffer the next frame was already filling = the
     * picture flickered / tore when drawing was slower than the camera). Start + end seen together = we
     * missed the boundary: wait for the end of the frame that is coming in now. */
    int started = 0;
    u64 wbuf = BUF_A;
    while (timer_us() < end) {
        u32 is = ur(0x104), st = ur(0x004);
        if (st) sta_or |= st;
        if (st & 0x1B6FCu) uw(0x004, st & 0x1B6FCu);    /* 0.99: clear only error/event bits - writing SYN/CS back nonstop (polling) reset the lane sync */
        if (is) uw(0x104, is);
        int fe = (is & 2) || (st & (1u << 15));          /* frame end (FEI, or PI0 = the FE packet captured, like Linux) */
        if (is & 1) {                                    /* frame start */
            n_fs++; started = 1;
            u32 wp = ur(0x11C) & 0x3FFFFFFFu;
            wbuf = (wp >= (u32)BUF_B && wp < (u32)BUF_B + 0x800000u) ? BUF_B : BUF_A;
            set_buf(wbuf == BUF_A ? BUF_B : BUF_A);      /* the next frame goes to the other one */
            if (fe) { delay_us(20); continue; }          /* that end belonged to an older frame */
        }
        if (fe && started) {
            n_fe++;
            cur_buf = wbuf == BUF_B;
            dcache_clean_inval((void *)wbuf, FRAME_BYTES);
            return (u16 *)wbuf;
        }
        delay_us(20);                                    /* do not hammer the receiver's registers */
    }
    return 0;
}

/* ---- 0.103: picture processing + settings (/CAM.TXT) + JPEG snapshots ----
 * Raw RAW10 Bayer -> one RGB pixel per 2x2 square (W/2 x H/2) at RGB_BUF: white balance (grey world /
 * locked / manual), gamma 2 (isqrt LUT), then brightness / contrast / saturation / hue in YCbCr, flip.
 * Exposure + gain: our own auto exposure on the mean level (the sensor's AEC is off, 0x3503 = 3), or fixed. */
#ifndef RGB_BUF
#define RGB_BUF 0x16000000UL                              /* 0.109: up to 1920x1080 x 4 B */
#endif
                             /* up to 648 x 488 x 4 = 1.3 MB */
#define JPG_BUF 0x13C00000UL
#define JPG_MAX (4 << 20)
#define LINE_US10 (vga_mode == 3 ? 276 : vga_mode == 1 ? 318 : 232)   /* one sensor line in 0.1 us: HTS / pixel rate (Linux modes) */
static int s_exp10, s_gain10, s_ev, s_wb, s_wbr = 100, s_wbb = 100;   /* exposure ms x10 / gain x10 (0 = auto); wb 0 auto 1 lock 2 manual */
static int s_bri, s_con, s_sat = 100, s_hue, s_gamma = 1, s_flip, s_q = 85;
static int s_sharp = 30, s_flicker = 50, s_cores = 3;   /* 0.111: s_cores = cores that draw (1-3) */                   /* 0.110: sharpness 0-100 %, mains Hz for the exposure (0 = off) */
static int cfg_loaded;
static u32 gr_cur = 256, gb_cur = 256;                    /* white balance gains x256 */
static int ae_exp = 1406, ae_gain = 64, ae_maxl = 1534, ae_skip, ae_ok;   /* lines, gain x16 */
static int ow, oh;                                        /* processed picture size */
static int zm10 = 10, pan_x, pan_y;                       /* 0.108 microscope: zoom x10 (10 = none), crop centre in sensor pixels */
static u8 glut[1024];
static int m_cf, m_bo, m_11, m_12, m_21, m_22, plain;     /* colour matrix x256 */

static u32 usqrt(u32 v) { u32 r = 0, b = 1u << 30; while (b > v) b >>= 2; while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; } return r; }

/* sin / cos x256 of whole degrees by repeated 1-degree rotations (x2^30) */
static void sincos256(int deg, int *c, int *s)
{
    long long x = 1LL << 30, y = 0; int n = deg < 0 ? -deg : deg;
    for (int i = 0; i < n; i++) { long long nx = (x * 1073578288LL - y * 18739379LL) >> 30, ny = (x * 18739379LL + y * 1073578288LL) >> 30; x = nx; y = ny; }
    *c = (int)(x >> 22); *s = (int)((deg < 0 ? -y : y) >> 22);
}

static void prep(void)
{
    for (int i = 0; i < 1024; i++) glut[i] = (u8)(s_gamma ? usqrt((u32)i * 65025u / 1023u) : (u32)i >> 2);
    int hc, hs; sincos256(s_hue, &hc, &hs);
    int sat = s_sat * 256 / 100;
    m_11 = hc * sat >> 8; m_12 = -hs * sat >> 8; m_21 = hs * sat >> 8; m_22 = hc * sat >> 8;
    m_cf = (100 + s_con) * 256 / 100; m_bo = s_bri * 128 / 100;
    plain = !s_bri && !s_con && s_sat == 100 && !s_hue;
    ir_now = -1;                                           /* the pin may have changed: set it again */
    ir_apply();
}

static void sensor_exposure(void)
{
    if (fake) return;
    u32 v = (u32)ae_exp << 4;
    wr16(saddr, 0x3500, (u8)((v >> 16) & 0x0F)); wr16(saddr, 0x3501, (u8)(v >> 8)); wr16(saddr, 0x3502, (u8)(v & 0xF0));
    wr16(saddr, 0x350A, (u8)((ae_gain >> 8) & 3)); wr16(saddr, 0x350B, (u8)ae_gain);
}

/* after the sensor started: our exposure control */
static void ae_begin(void)
{
    u8 a = 0, b = 0;
    if (fake) { ae_maxl = 1534; ae_skip = 2; ae_ok = 0; if (s_exp10) ae_exp = s_exp10 * 1000 / LINE_US10; if (s_gain10) ae_gain = s_gain10 * 16 / 10; return; }
    if (!rd16(saddr, 0x380E, &a) && !rd16(saddr, 0x380F, &b) && ((a << 8) | b) > 100) ae_maxl = ((a << 8) | b) - 4;
    wr16(saddr, 0x3503, 0x03);                            /* sensor AEC + AGC off */
    if (s_exp10) ae_exp = s_exp10 * 1000 / LINE_US10;
    if (s_gain10) ae_gain = s_gain10 * 16 / 10;
    if (ae_exp > ae_maxl) ae_exp = ae_maxl;
    if (ae_exp < 2) ae_exp = 2;
    sensor_exposure();
    ae_skip = 2; ae_ok = 0;
}

/* mean level (10 bit) of the last frame -> new exposure / gain; ae_ok counts calm frames */
static void ae_step(u32 mean)
{
    if (s_exp10 && s_gain10) { ae_ok = 9; return; }
    if (ae_skip) { ae_skip--; return; }
    u32 target = s_gamma ? 184 : 400;
    for (int i = 0; i < s_ev; i++) target *= 2;
    for (int i = 0; i > s_ev; i--) target /= 2;
    if (target > 900) target = 900;
    if (!mean) mean = 1;
    u32 f = target * 256 / mean;
    /* 0.110: calmer - once settled, only a change over 20 % moves it (8 % while settling), and each step goes
     * half the way (geometric) so the picture does not pump */
    u32 lo = ae_ok >= 3 ? 213 : 236, hi = ae_ok >= 3 ? 320 : 278;
    if (f > lo && f < hi) { ae_ok++; return; }
    ae_ok = 0;
    if (f > 768) f = 768;
    if (f < 96) f = 96;
    f = usqrt(f * 256);
    u64 tot = (u64)ae_exp * (u64)ae_gain * f >> 8;
    int e = ae_exp, g = ae_gain;
    if (s_exp10) g = (int)(tot / (u64)e);
    else if (s_gain10) e = (int)(tot / (u64)g);
    else { e = (int)(tot / 16); if (e > ae_maxl) e = ae_maxl; if (e < 2) e = 2; g = (int)(tot / (u64)e); }
    if (e > ae_maxl) e = ae_maxl;
    if (e < 2) e = 2;
    if (s_flicker && !s_exp10 && !s_gain10) {                           /* lamps on 50/60 Hz mains flicker at 100/120 Hz: exposure */
        int step = (s_flicker == 60 ? 83333 : 100000) / LINE_US10;   /* in whole 10 / 8.3 ms steps = no beat */
        if (step > 0 && step <= ae_maxl) {
            int e2 = e < step ? step : e / step * step;     /* 0.113: never shorter than one period (owner: fixed 10 ms */
            g = (int)((u64)g * e / e2); e = e2;              /* stopped the blinking) - a bright scene lowers the gain */
        }
    }
    if (g < 16) g = 16;
    if (g > 255) g = 255;
    if (ir_mode == 3) {                                    /* IR auto: is it dark? */
        u64 now = timer_us();
        int dark = ir_dark;
        if (!ir_dark && e >= ae_maxl && g >= 128) dark = 1;
        else if (ir_dark && g <= 16 && e < ae_maxl / 4) dark = 0;
        if (dark != ir_dark && (!ir_changed || now - ir_changed > 20000000ULL)) {
            ir_dark = dark; ir_changed = now; ir_apply();
            ae_ok = 0;
        }
    }
    if (e == ae_exp && g == ae_gain) { ae_ok++; return; }   /* at a limit */
    ae_exp = e; ae_gain = g;
    sensor_exposure();
    ae_skip = 2;
}

/* 10-bit R G B (white balance applied) -> 0x00RRGGBB with gamma + the colour settings */
static inline u32 px_out(u32 R, u32 G, u32 B)
{
    if (R > 1023) R = 1023; if (G > 1023) G = 1023; if (B > 1023) B = 1023;
    int r = glut[R], g = glut[G], b = glut[B];
    if (!plain) {
        int Y = (77 * r + 150 * g + 29 * b) >> 8, cb = (-43 * r - 85 * g + 128 * b) >> 8, cr = (128 * r - 107 * g - 21 * b) >> 8;
        Y = (((Y - 128) * m_cf) >> 8) + 128 + m_bo;
        int cb2 = (cb * m_11 + cr * m_12) >> 8, cr2 = (cb * m_21 + cr * m_22) >> 8;
        r = Y + ((359 * cr2) >> 8); g = Y - ((88 * cb2 + 183 * cr2) >> 8); b = Y + ((454 * cb2) >> 8);
        if (r < 0) r = 0; if (r > 255) r = 255; if (g < 0) g = 0; if (g > 255) g = 255; if (b < 0) b = 0; if (b > 255) b = 255;
    }
    return ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

/* 0.110: one output pixel from the 2x2 window at q (offsets o: B, G1, G2, R) + sharpening: the green of the
 * window minus the mean green of the 4 windows 2 sensor pixels away (same colour layout) = detail, added to
 * all three colours (k = strength x256; 0 = off). q must have 2 pixels of room on every side when k > 0. */
static inline u32 px_win(const u16 *q, const int *o, int sh, u32 grc, u32 gbc, int k)
{
    int B = q[o[0]] >> sh, G = ((int)q[o[1]] + q[o[2]]) >> (sh + 1), R = q[o[3]] >> sh;
    if (k) {
        int c = q[o[1]] + q[o[2]], a1 = o[1] - 2, a2 = o[2] - 2, b1 = o[1] + 2, b2 = o[2] + 2;
        int n = q[a1] + q[a2] + q[b1] + q[b2] + q[o[1] - 2 * W] + q[o[2] - 2 * W] + q[o[1] + 2 * W] + q[o[2] + 2 * W];
        int d = ((4 * c - n) >> (sh + 1)) * k >> 10;       /* /4 neighbours, /2 two greens -> 10-bit detail */
        R += d; G += d; B += d;
        if (R < 0) R = 0; if (G < 0) G = 0; if (B < 0) B = 0;
    }
    return px_out((u32)R * grc >> 8, (u32)G, (u32)B * gbc >> 8);
}

/* raw frame -> RGB_BUF (ow x oh), white balance + exposure updated from it */
static int g_sh, g_ob, g_og1, g_og2, g_orr;             /* from frame_stats: bit shift, Bayer offsets */
/* white balance + exposure from a raw frame; sets the g_ values the renderers use */
static void frame_stats(const u16 *f)
{

    u32 orv = 0; u64 sr = 0, sg = 0, sb = 0; u32 cnt = 0;
    int ord = bayer >= 0 ? bayer : ((vga_mode == 0 || vga_mode == 3) ? 1 : 0);
    int ob, og1, og2, orr;
    switch (ord) {
    case 1:  ob = 1;     og1 = 0; og2 = W + 1; orr = W;     break;   /* GBRG */
    case 2:  ob = W;     og1 = 0; og2 = W + 1; orr = 1;     break;   /* GRBG */
    case 3:  ob = W + 1; og1 = 1; og2 = W;     orr = 0;     break;   /* RGGB */
    default: ob = 0;     og1 = 1; og2 = W;     orr = W + 1; break;   /* BGGR */
    }
    for (int y = 0; y < H; y += 8) for (int x = 0; x < W; x += 8) {
        const u16 *p = f + y * W + x;
        orv |= p[0] | p[1] | p[W] | p[W + 1];
        sb += p[ob]; sg += p[og1] + p[og2]; sr += p[orr]; cnt++;
    }
    int sh = 0; while (sh < 8 && (orv >> (sh + 10))) sh++;          /* top bit -> 10 bit */
    sg /= 2; if (!sr) sr = 1; if (!sb) sb = 1; if (!cnt) cnt = 1;
    if (s_wb == 0) {
        u32 gr = (u32)(sg * 256 / sr), gb = (u32)(sg * 256 / sb);
        if (gr > 1024) gr = 1024; if (gb > 1024) gb = 1024;
        gr_cur = (gr_cur * 7 + gr) / 8; gb_cur = (gb_cur * 7 + gb) / 8;   /* 0.110: slower = no colour flicker */
    } else if (s_wb == 2) { gr_cur = (u32)s_wbr * 256 / 100; gb_cur = (u32)s_wbb * 256 / 100; }
    u32 mean = (u32)(((sr * gr_cur >> 8) + 2 * sg + (sb * gb_cur >> 8)) / 4 / cnt) >> sh;
    ae_step(mean);
    g_sh = sh; g_ob = ob; g_og1 = og1; g_og2 = og2; g_orr = orr;
}

static void process_frame(const u16 *f)
{
    frame_stats(f);
    int sh = g_sh, ob = g_ob, og1 = g_og1, og2 = g_og2, orr = g_orr;
    ow = W / 2; oh = H / 2;
    u32 *out = (u32 *)RGB_BUF;
    u32 grc = gr_cur, gbc = gb_cur;
    if (zm10 > 10) {                                       /* zoom: a sliding 2x2 window over the sensor pixels = full detail */
        int cw = W * 10 / zm10, ch = H * 10 / zm10;
        int x0 = pan_x - cw / 2, y0 = pan_y - ch / 2;
        if (x0 > W - cw - 2) x0 = W - cw - 2;
        if (y0 > H - ch - 2) y0 = H - ch - 2;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        pan_x = x0 + cw / 2; pan_y = y0 + ch / 2;
        int off[4][4];                                     /* [parity y*2+x][B, G1, G2, R] offsets inside the window */
        int typ[4];                                        /* colour of the 4 cells of an even window: 0 B, 1 G, 2 R */
        int e[4] = { ob, og1, og2, orr };
        for (int k = 0; k < 4; k++) { int pos = e[k] == 0 ? 0 : e[k] == 1 ? 1 : e[k] == W ? 2 : 3; typ[pos] = k == 0 ? 0 : k == 3 ? 2 : 1; }
        for (int py = 0; py < 2; py++) for (int px = 0; px < 2; px++) {
            int g = 0, *o = off[py * 2 + px];
            for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
                int t = typ[((py + dy) & 1) * 2 + ((px + dx) & 1)], a = dy * W + dx;
                if (t == 0) o[0] = a; else if (t == 2) o[3] = a; else o[1 + g++] = a;
            }
        }
        static int xm[1024];
        for (int x = 0; x < ow && x < 1024; x++) xm[x] = x0 + x * cw / ow;
        for (int y = 0; y < oh; y++) {
            int ry = y0 + y * ch / oh;
            const u16 *a = f + ry * W;
            int dy = (s_flip & 2) ? oh - 1 - y : y;
            u32 *row = out + dy * ow;
            for (int x = 0; x < ow; x++) {
                int rx = xm[x];
                const u16 *q = a + rx;
                const int *o = off[(ry & 1) * 2 + (rx & 1)];
                u32 B = (u32)q[o[0]] >> sh, G = ((u32)q[o[1]] + q[o[2]]) >> (sh + 1), R = (u32)q[o[3]] >> sh;
                row[(s_flip & 1) ? ow - 1 - x : x] = px_out(R * grc >> 8, G, B * gbc >> 8);
            }
        }
        return;
    }
    int ko[4] = { ob, og1, og2, orr }, k = s_sharp * 256 / 100;
    for (int y = 0; y < oh; y++) {
        const u16 *a = f + (2 * y) * W;
        int dy = (s_flip & 2) ? oh - 1 - y : y;
        u32 *row = out + dy * ow;
        int ky = (y >= 1 && y < oh - 2) ? k : 0;
        for (int x = 0; x < ow; x++) {
            const u16 *q = a + 2 * x;
            row[(s_flip & 1) ? ow - 1 - x : x] = px_win(q, ko, sh, grc, gbc, (x >= 1 && x < ow - 2) ? ky : 0);
        }
    }
}


/* 0.109: any part of the sensor picture (x0, y0, cw x ch sensor pixels) into dst (dw x dh, stride in pixels),
 * one sliding 2x2 Bayer window per output pixel = full sensor detail; flip applied. Rows y_from..y_to-1 only,
 * so cores 2 and 3 can draw parts of the same picture (render_par). Needs frame_stats() for this frame. */
struct rjob { const u16 *f; u32 *dst; int stride, dw, dh, x0, y0, cw, ch, yf, yt; };
static int r_off[4][4];                                    /* [sensor parity y*2+x][B, G1, G2, R] window offsets */
static int r_xm[4096];

static void r_prepare(int dw, int x0, int cw)
{
    int typ[4], e[4] = { g_ob, g_og1, g_og2, g_orr };
    for (int k = 0; k < 4; k++) { int pos = e[k] == 0 ? 0 : e[k] == 1 ? 1 : e[k] == W ? 2 : 3; typ[pos] = k == 0 ? 0 : k == 3 ? 2 : 1; }
    for (int py = 0; py < 2; py++) for (int px = 0; px < 2; px++) {
        int g = 0, *o = r_off[py * 2 + px];
        for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
            int t = typ[((py + dy) & 1) * 2 + ((px + dx) & 1)], a = dy * W + dx;
            if (t == 0) o[0] = a; else if (t == 2) o[3] = a; else o[1 + g++] = a;
        }
    }
    if (dw > 4096) dw = 4096;
    for (int x = 0; x < dw; x++) {
        int xx = (s_flip & 1) ? dw - 1 - x : x;
        r_xm[x] = x0 + (int)((long)xx * cw / dw);
    }
}

static void r_rows(void *arg)
{
    struct rjob *j = arg;
    u32 grc = gr_cur, gbc = gb_cur;
    int sh = g_sh;
    int k = s_sharp * 256 / 100;
    for (int y = j->yf; y < j->yt; y++) {
        int yy = (s_flip & 2) ? j->dh - 1 - y : y;
        int ry = j->y0 + (int)((long)yy * j->ch / j->dh);
        const u16 *a = j->f + ry * W;
        u32 *row = j->dst + (long)y * j->stride;
        const int (*oo)[4] = r_off + (ry & 1) * 2;
        int ky = (ry >= 2 && ry < H - 4) ? k : 0;
        for (int x = 0; x < j->dw; x++) {
            int rx = r_xm[x];
            row[x] = px_win(a + rx, oo[rx & 1], sh, grc, gbc, (rx >= 2 && rx < W - 4) ? ky : 0);
        }
    }
}

/* the whole job on core 0 + the worker cores 2 and 3 (a third each) */
static void render_par(const u16 *f, u32 *dst, int stride, int dw, int dh, int x0, int y0, int cw, int ch)
{
    if (dw > 4096) dw = 4096;
    if (cw > W - 1) cw = W - 1;
    if (ch > H - 1) ch = H - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 + cw > W - 1) x0 = W - 1 - cw;
    if (y0 + ch > H - 1) y0 = H - 1 - ch;
    r_prepare(dw, x0, cw);
    static struct rjob jb[3];
    int used[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; k++) {
        jb[k].f = f; jb[k].dst = dst; jb[k].stride = stride; jb[k].dw = dw; jb[k].dh = dh;
        jb[k].x0 = x0; jb[k].y0 = y0; jb[k].cw = cw; jb[k].ch = ch;
        jb[k].yf = dh * k / 3; jb[k].yt = dh * (k + 1) / 3;
    }
    int nc = s_cores < 1 ? 1 : s_cores > 3 ? 3 : s_cores;
    for (int k = 0; k < 3; k++) { jb[k].yf = dh * k / nc; jb[k].yt = k < nc ? dh * (k + 1) / nc : jb[k].yf; }
    for (int k = 1; k < nc; k++) if (smp_worker_ok(k + 1) && smp_run(k + 1, r_rows, &jb[k]) == 0) used[k] = 1;
    r_rows(&jb[0]);
    for (int k = 1; k < nc; k++) { if (used[k]) { if (smp_wait(k + 1, 500)) r_rows(&jb[k]); } else r_rows(&jb[k]); }
}

static int ae_exp_f(void) { return ae_exp * ae_gain; }

static void blit(int x0, int y0, int scale)
{
    const u32 *in = (const u32 *)RGB_BUF;
    for (int y = 0; y < oh; y++)
        for (int s = 0; s < scale; s++) {
            int yy = y0 + y * scale + s;
            if (yy >= (int)fb_height()) return;
            u32 *row = fb_row(yy) + x0;
            const u32 *src = in + y * ow;
            if (scale == 1) { for (int x = 0; x < ow; x++) row[x] = src[x]; continue; }
            for (int x = 0; x < ow; x++) for (int k = 0; k < scale; k++) row[x * scale + k] = src[x];
        }
}

/* ---- settings file ---- */
static void put_d10(char *b, int *n, int v)                /* 12 -> "1.2" */
{
    char t[12]; int k = 0, w = v / 10;
    do t[k++] = (char)('0' + w % 10); while ((w /= 10));
    while (k) b[(*n)++] = t[--k];
    if (v % 10) { b[(*n)++] = '.'; b[(*n)++] = (char)('0' + v % 10); }
}
static void put_int(char *b, int *n, int v)
{
    char t[12]; int k = 0;
    if (v < 0) { b[(*n)++] = '-'; v = -v; }
    do t[k++] = (char)('0' + v % 10); while ((v /= 10));
    while (k) b[(*n)++] = t[--k];
}
static void put_s(char *b, int *n, const char *s) { while (*s) b[(*n)++] = *s++; }

static int cfg_text(char *b)
{
    int n = 0;
    put_s(b, &n, "exposure "); if (s_exp10) put_d10(b, &n, s_exp10); else put_s(b, &n, "auto");
    put_s(b, &n, "\ngain "); if (s_gain10) put_d10(b, &n, s_gain10); else put_s(b, &n, "auto");
    put_s(b, &n, "\nev "); put_int(b, &n, s_ev);
    put_s(b, &n, "\nwb ");
    if (s_wb == 0) put_s(b, &n, "auto");
    else { put_int(b, &n, s_wb == 1 ? (int)(gr_cur * 100 / 256) : s_wbr); put_s(b, &n, " "); put_int(b, &n, s_wb == 1 ? (int)(gb_cur * 100 / 256) : s_wbb); }
    put_s(b, &n, "\nbrightness "); put_int(b, &n, s_bri);
    put_s(b, &n, "\ncontrast "); put_int(b, &n, s_con);
    put_s(b, &n, "\nsaturation "); put_int(b, &n, s_sat);
    put_s(b, &n, "\nhue "); put_int(b, &n, s_hue);
    put_s(b, &n, "\ngamma "); put_s(b, &n, s_gamma ? "on" : "off");
    put_s(b, &n, "\nflip "); put_s(b, &n, s_flip == 0 ? "off" : s_flip == 1 ? "h" : s_flip == 2 ? "v" : "hv");
    put_s(b, &n, "\nquality "); put_int(b, &n, s_q);
    put_s(b, &n, "\nsharpness "); put_int(b, &n, s_sharp);
    put_s(b, &n, "\ncores "); put_int(b, &n, s_cores);
    put_s(b, &n, "\nflicker "); if (s_flicker) put_int(b, &n, s_flicker); else put_s(b, &n, "off");
    put_s(b, &n, "\nir "); if (ir_pin) put_int(b, &n, ir_pin); else put_s(b, &n, "off");
    put_s(b, &n, "\nir_mode "); put_s(b, &n, ir_mode == 0 ? "off" : ir_mode == 1 ? "on" : ir_mode == 2 ? "photo" : "auto");
    put_s(b, &n, "\nir_active "); put_s(b, &n, ir_low ? "low" : "high");
    put_s(b, &n, "\n"); b[n] = 0;
    return n;
}

static void cfg_save(void)
{
    char b[400]; int n = cfg_text(b);
    if (!fat_mounted() || fat_write_file("/CAM.TXT", b, (u32)n)) kprintf("(not saved: no SD card or write error)\n");
}

/* "12" / "-3" / "2.5" -> x10 in *v (dec = 1) or whole (dec = 0); 0 = ok */
static int num_arg(const char *s, int dec, int *v)
{
    int neg = 0, w = 0, f = 0, dig = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { w = w * 10 + (*s++ - '0'); dig = 1; if (w > 100000) return -1; }
    if (*s == '.' && dec) { s++; if (*s >= '0' && *s <= '9') { f = *s++ - '0'; dig = 1; } while (*s >= '0' && *s <= '9') s++; }
    if (*s || !dig) return -1;
    w = dec ? w * 10 + f : w;
    *v = neg ? -w : w;
    return 0;
}

/* one setting "name value [value2]"; 0 ok, -1 bad value, -2 unknown name */
static int set_one(const char *k, const char *v, const char *v2)
{
    int x, y;
    if (!strcmp(k, "exposure")) { if (!strcmp(v, "auto")) { s_exp10 = 0; return 0; } if (num_arg(v, 1, &x) || x < 1 || x > 356) return -1; s_exp10 = x; return 0; }
    if (!strcmp(k, "gain")) { if (!strcmp(v, "auto")) { s_gain10 = 0; return 0; } if (num_arg(v, 1, &x) || x < 10 || x > 159) return -1; s_gain10 = x; return 0; }
    if (!strcmp(k, "ev")) { if (num_arg(v, 0, &x) || x < -3 || x > 3) return -1; s_ev = x; return 0; }
    if (!strcmp(k, "wb")) {
        if (!strcmp(v, "auto")) { s_wb = 0; return 0; }
        if (!strcmp(v, "lock")) { s_wb = 1; return 0; }
        if (!v2 || num_arg(v, 0, &x) || num_arg(v2, 0, &y) || x < 10 || x > 400 || y < 10 || y > 400) return -1;
        s_wb = 2; s_wbr = x; s_wbb = y; return 0;
    }
    if (!strcmp(k, "brightness")) { if (num_arg(v, 0, &x) || x < -100 || x > 100) return -1; s_bri = x; return 0; }
    if (!strcmp(k, "contrast")) { if (num_arg(v, 0, &x) || x < -100 || x > 100) return -1; s_con = x; return 0; }
    if (!strcmp(k, "saturation")) { if (num_arg(v, 0, &x) || x < 0 || x > 300) return -1; s_sat = x; return 0; }
    if (!strcmp(k, "hue")) { if (num_arg(v, 0, &x) || x < -180 || x > 180) return -1; s_hue = x; return 0; }
    if (!strcmp(k, "gamma")) { if (strcmp(v, "on") && strcmp(v, "off")) return -1; s_gamma = !strcmp(v, "on"); return 0; }
    if (!strcmp(k, "flip")) {
        if (!strcmp(v, "off")) s_flip = 0; else if (!strcmp(v, "h")) s_flip = 1; else if (!strcmp(v, "v")) s_flip = 2;
        else if (!strcmp(v, "hv") || !strcmp(v, "vh")) s_flip = 3; else return -1;
        return 0;
    }
    if (!strcmp(k, "ir")) { if (!strcmp(v, "off")) { if (ir_pin && ir_now > 0) gpio_set(ir_pin, ir_low); ir_pin = 0; return 0; }
                            if (num_arg(v, 0, &x) || x < 2 || x > 27) return -1; if (ir_pin && ir_pin != x && ir_now > 0) gpio_set(ir_pin, ir_low); ir_pin = x; ir_now = -1; return 0; }
    if (!strcmp(k, "ir_mode")) { if (!strcmp(v, "off")) ir_mode = 0; else if (!strcmp(v, "on")) ir_mode = 1; else if (!strcmp(v, "photo")) ir_mode = 2;
                                 else if (!strcmp(v, "auto")) ir_mode = 3; else return -1; ir_dark = 0; ir_changed = 0; return 0; }
    if (!strcmp(k, "ir_active")) { if (strcmp(v, "high") && strcmp(v, "low")) return -1; ir_low = !strcmp(v, "low"); ir_now = -1; return 0; }
    if (!strcmp(k, "cores")) { if (num_arg(v, 0, &x) || x < 1 || x > 3) return -1; s_cores = x; return 0; }
    if (!strcmp(k, "sharpness")) { if (num_arg(v, 0, &x) || x < 0 || x > 200) return -1; s_sharp = x; return 0; }
    if (!strcmp(k, "flicker")) { if (!strcmp(v, "off")) { s_flicker = 0; return 0; } if (num_arg(v, 0, &x) || (x != 50 && x != 60)) return -1; s_flicker = x; return 0; }
    if (!strcmp(k, "quality")) { if (num_arg(v, 0, &x) || x < 10 || x > 100) return -1; s_q = x; return 0; }
    return -2;
}

static void cfg_load(void)
{
    if (cfg_loaded) return;
    cfg_loaded = 1;
    struct fat_file fl; char b[400];
    if (fat_mounted() && !fat_open("/CAM.TXT", &fl) && !fl.is_dir) {
        int n = fat_read(&fl, b, sizeof(b) - 1);
        if (n > 0) {
            b[n] = 0;
            for (char *l = b; *l; ) {
                char *e = l; while (*e && *e != '\n' && *e != '\r') e++;
                char sv = *e; *e = 0;
                char *w[3] = { 0, 0, 0 }; int nw = 0;
                for (char *p = l; *p && nw < 3; ) {
                    while (*p == ' ' || *p == '\t' || *p == '=') *p++ = 0;
                    if (!*p) break;
                    w[nw++] = p;
                    while (*p && *p != ' ' && *p != '\t' && *p != '=') p++;
                }
                if (nw >= 2) {
                    set_one(w[0], w[1], w[2]);
                    if (!strcmp(w[0], "wb") && s_wb == 2) { gr_cur = (u32)s_wbr * 256 / 100; gb_cur = (u32)s_wbb * 256 / 100; }
                }
                *e = sv; l = *e ? e + 1 : e; while (*l == '\n' || *l == '\r') l++;
            }
        }
    }
    prep();
}

/* ---- JPEG to the card ---- */
static int exists(const char *p) { struct fat_file f; return !fat_open(p, &f); }

/* auto name: /CAM/YYYYMMDD/HHMMSS.JPG with the clock, else /CAM/IMG00001.JPG */
static int auto_name_x(char *p, const char *ext, const char *pre)
{
    fat_mkdir("/CAM");
    u64 t = clock_now();
    if (t) {
        char d[32]; clock_format(d, t, 0);              /* "YYYY-MM-DD HH:MM:SS" */
        char dir[20] = "/CAM/"; int n = 5;
        for (int i = 0; i < 10; i++) if (d[i] != '-') dir[n++] = d[i];
        dir[n] = 0;
        fat_mkdir(dir);
        for (int k = 0; k < 10; k++) {
            int m = 0; for (const char *s = dir; *s; ) p[m++] = *s++;
            p[m++] = '/';
            for (int i = 11; i < 19; i++) if (d[i] != ':') p[m++] = d[i];
            if (k) { p[m++] = '_'; p[m++] = (char)('0' + k); }
            const char *x = ext; while (*x) p[m++] = *x++;
            p[m] = 0;
            if (!exists(p)) return 0;
        }
        return -1;
    }
    for (int k = 1; k < 100000; k++) {
        const char *b = "/CAM/"; int m = 0; while (*b) p[m++] = *b++;
        b = pre; while (*b) p[m++] = *b++;
        int v = k; for (int i = 4; i >= 0; i--) { p[m + i] = (char)('0' + v % 10); v /= 10; }
        m += 5; const char *x = ext; while (*x) p[m++] = *x++;
        p[m] = 0;
        if (!exists(p)) return 0;
    }
    return -1;
}

static int auto_name(char *p) { return auto_name_x(p, ".JPG", "IMG"); }

/* the processed picture as JPEG to `path` (0 = auto name); prints the result, returns the size or < 0 */
static int save_jpeg(const char *path, char *name_out)
{
    char p[96];
    if (!fat_mounted()) { kprintf("no SD card\n"); return -1; }
    if (path) { if (path_abs(path, p, sizeof(p))) { kprintf("bad file name\n"); return -1; } }
    else if (auto_name(p)) { kprintf("no free file name in /CAM\n"); return -1; }
    u64 t0 = timer_us();
    int n = jpeg_encode((const u32 *)RGB_BUF, ow, oh, ow, s_q, (u8 *)JPG_BUF, JPG_MAX);
    if (n <= 0) { kprintf("JPEG encoding failed (%d)\n", n); return -1; }
    u32 enc_ms = (u32)((timer_us() - t0) / 1000);
    if (fat_write_file(p, (const void *)JPG_BUF, (u32)n)) { kprintf("could not write %s (8.3 names only, folder must exist)\n", p); return -1; }
    kprintf("saved %s: %dx%d, %u KB (JPEG %u ms, total %u ms)\n", p, ow, oh, (u32)(n + 1023) / 1024, enc_ms, (u32)((timer_us() - t0) / 1000));
    if (name_out) { int i = 0; while (p[i] && i < 63) { name_out[i] = p[i]; i++; } name_out[i] = 0; }
    return n;
}

/* 0.92: all receiver registers + the clock regs, to compare with Linux (tools/camdump.py prints the same) */
static void dump_regs(void)
{
    static const u16 offs[] = { 0x000,0x004,0x008,0x00C,0x010,0x014,0x018,0x01C,0x028,0x02C,0x030,0x034,0x038,
                                0x100,0x104,0x108,0x10C,0x110,0x114,0x118,0x11C,0x120,0x124,0x128,0x12C,0x130,0x134,
                                0x200,0x300,0x400 };
    for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
        kprintf("%03x=%08x%s", offs[i], ur(offs[i]), (i % 6) == 5 ? "\n" : "  ");
    kprintf("\nCM_CAM1CTL=%08x CM_CAM1DIV=%08x gate=%08x\n", mmio_read(CM_CAM1CTL), mmio_read(CM_CAM1DIV), mmio_read(UNI_GATE));
}

/* 0.98: the sensor registers we set, read back while streaming - same list + format as tools/ov5647regs.sh */
static const u16 sreg_list[] = { 0x0100,0x3000,0x3001,0x3002,0x300d,0x300e,0x3016,0x3017,0x3018,0x301c,0x301d,0x3034,0x3035,0x3036,0x303c,0x3106,0x3212,0x3500,0x3501,0x3502,0x3503,0x350a,0x350b,0x3600,0x3612,0x3618,0x3620,0x3621,0x3630,0x3632,0x3633,0x3634,0x3636,0x3703,0x3704,0x3705,0x3708,0x3709,0x370b,0x370c,0x3715,0x3717,0x3731,0x3800,0x3801,0x3802,0x3803,0x3804,0x3805,0x3806,0x3807,0x3808,0x3809,0x380a,0x380b,0x380c,0x380d,0x380e,0x380f,0x3811,0x3813,0x3814,0x3815,0x3820,0x3821,0x3827,0x3a08,0x3a09,0x3a0a,0x3a0b,0x3a0d,0x3a0e,0x3a0f,0x3a10,0x3a11,0x3a18,0x3a19,0x3a1b,0x3a1e,0x3a1f,0x3b07,0x3c01,0x3f01,0x3f05,0x3f06,0x4000,0x4001,0x4004,0x4202,0x4800,0x4801,0x4802,0x4805,0x4810,0x4814,0x4818,0x4819,0x481f,0x4823,0x4826,0x4827,0x4836,0x4837,0x5000,0x5001,0x5002,0x5003,0x503d,0x5a00 };
static void dump_sensor(void)
{
    int n = 0;
    for (unsigned i = 0; i < sizeof(sreg_list) / sizeof(sreg_list[0]); i++) {
        u8 v = 0; int r = rd16(saddr, sreg_list[i], &v);
        if (r) kprintf("%04x=?? ", sreg_list[i]); else kprintf("%04x=%02x ", sreg_list[i], v);
        if (++n == 8) { kprintf("\n"); n = 0; }
    }
    if (n) kprintf("\n");
}

/* ---- 0.104: the camera on the web page (/camera, GET|POST /api/camera, GET /api/camera.jpg) ----
 * Core 1 (lwIP) asks, core 0 (camera_poll from con_trygetc, or the cam live/snap loop) takes the pictures:
 * a request gets a ticket (web_req), core 0 encodes the next frame into the free one of two JPEG buffers and
 * publishes it (web_idx, web_done = ticket served); core 1 marks the buffer busy while it sends it.
 * Settings from the page are set by core 1 directly (plain ints), core 0 re-makes the tables and saves
 * /CAM.TXT 2 s after the last change. The camera stops 10 s after the last web picture. */
#define WEB_JPG0 0x14000000UL
#define WEB_JPG1 0x14400000UL
static volatile u32 web_req, web_done;
static volatile int web_idx = -1, web_len[2], web_busy[2], web_err;
static int web_lk;
static volatile int web_prep, web_snap, web_defaults;
static volatile u64 web_save_at;
static u64 web_last;                                      /* core 0: time of the last served web picture */
static int in_show, in_poll;
static u64 nocam_until;                                  /* no camera: web requests fail at once until then */
static char last_photo[64];
static volatile u32 fps_x10;

static void wlock(void)   { while (__atomic_test_and_set(&web_lk, __ATOMIC_ACQUIRE)) ; }
static void wunlock(void) { __atomic_clear(&web_lk, __ATOMIC_RELEASE); }

/* core 0: a processed picture is in RGB_BUF - answer a waiting web request with it */
static void web_publish(void)
{
    u32 want = __atomic_load_n(&web_req, __ATOMIC_ACQUIRE);
    if (want == web_done) return;
    wlock();
    int b = web_idx == 0 ? 1 : 0;
    if (web_busy[b]) { wunlock(); return; }               /* both buffers being sent: next frame */
    web_busy[b] = 1000;                                   /* ours while encoding */
    wunlock();
    int n = jpeg_encode((const u32 *)RGB_BUF, ow, oh, ow, s_q, (u8 *)(b ? WEB_JPG1 : WEB_JPG0), JPG_MAX);
    wlock();
    web_busy[b] = 0;
    if (n > 0) { web_len[b] = n; web_idx = b; web_err = 0; }
    else web_err = 2;
    __atomic_store_n(&web_done, want, __ATOMIC_RELEASE);
    wunlock();
    web_last = timer_us();
}

static void web_fail(int code)
{
    web_err = code;
    __atomic_store_n(&web_done, __atomic_load_n(&web_req, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

static void web_defaults_now(void)
{
    s_exp10 = s_gain10 = s_ev = s_wb = s_bri = s_con = s_hue = s_flip = 0;
    s_wbr = s_wbb = s_sat = 100; s_gamma = 1; s_q = 85; gr_cur = gb_cur = 256; s_sharp = 30; s_flicker = 50; s_cores = 3;
}

/* core 0: start the camera for the web / MQTT / doorbell (quiet), let the exposure settle; 0 = ok */
static int quiet_start(void)
{
    if (nocam_until && timer_us() < nocam_until) return -1;
    int t = trace; trace = 0;
    test_pattern = 0;
    int r = cam_start();
    trace = t;
    if (r) { nocam_until = timer_us() + 30000000ULL; return -1; }   /* try again in 30 s */
    nocam_until = 0;
    ae_begin();
    web_stream = 1;
    for (int i = 0; i < 20; i++) {                         /* let the exposure find its level (IR may switch on) */
        u16 *f = next_frame(i ? 200 : 1500);
        if (!f) break;
        int ir0 = ir_now;
        process_frame(f);
        if (ae_ok >= 1 && i >= 3 && ir_now == ir0) break;
    }
    web_last = timer_us();
    static int announced;
    if ((sensor || fake) && !announced) { announced = 1; mqtt_rediscover(); }   /* Home Assistant: the camera exists now */
    return 0;
}

/* 0.105: one photo now - to the card (save) and / or to MQTT <base>/camera (send). 0 ok, -1 no camera,
 * -3 no frame, -4 the camera is busy (cam live), -5 MQTT not connected, -6 the last photo still going out */
#define MQTT_JPG 0x14800000UL
static volatile int mqtt_snap;
void camera_mqtt_request(void) { mqtt_snap = 1; }

int camera_take(int save, int send, char *name_out)
{
    if (in_show || in_poll) return -4;
    in_poll = 1;
    if (!cfg_loaded) cfg_load();
    int r = 0;
    if (!streaming && quiet_start()) { r = -1; goto out; }
    u16 *f = next_frame(500);
    if (!f) { r = -3; goto out; }
    process_frame(f);
    web_last = timer_us();
    if (save) {
        if (save_jpeg(0, last_photo) <= 0) { const char *x = "(not saved)"; int i = 0; while (x[i]) { last_photo[i] = x[i]; i++; } last_photo[i] = 0; }
        if (name_out) { int i = 0; while (last_photo[i] && i < 63) { name_out[i] = last_photo[i]; i++; } name_out[i] = 0; }
    }
    if (send) {
        if (!mqtt_connected()) { r = -5; goto out; }
        if (mqtt_photo_busy()) { r = -6; goto out; }
        int n = jpeg_encode((const u32 *)RGB_BUF, ow, oh, ow, s_q, (u8 *)MQTT_JPG, JPG_MAX);
        if (n <= 0) { r = -3; goto out; }
        int m = mqtt_send_photo((const u8 *)MQTT_JPG, (u32)n);
        if (m == -1) r = -5; else if (m == -2) r = -6;
        else kprintf("photo sent to MQTT (%u KB)\n", (u32)(n + 1023) / 1024);
    }
out:
    in_poll = 0;
    return r;
}

const char *camera_take_error(int r)
{
    return r == -1 ? "no camera" : r == -3 ? "no picture from the camera" : r == -4 ? "the camera is busy (cam live)" :
           r == -5 ? "MQTT is not connected ('mqtt' shows why)" : r == -6 ? "the last photo is still being sent" : "?";
}

/* core 0, from con_trygetc */
void camera_poll(void)
{
    if (mqtt_snap && !in_show && !in_poll && !io_held()) {   /* Home Assistant pressed "Take photo" */
        mqtt_snap = 0;
        int r = camera_take(0, 1, 0);
        if (r) kprintf("\n[CAM] photo for MQTT: %s\n", camera_take_error(r));
    }
    if (in_show || in_poll || io_held()) return;
    in_poll = 1;
    if (!cfg_loaded) cfg_load();
    if (web_defaults) { web_defaults = 0; web_defaults_now(); web_prep = 1; web_save_at = timer_us() + 100000; }
    if (web_prep) { web_prep = 0; prep(); }
    if (web_save_at && timer_us() >= web_save_at) { web_save_at = 0; cfg_save(); }
    int want = __atomic_load_n(&web_req, __ATOMIC_ACQUIRE) != web_done || web_snap;
    if (want) {
        if (!streaming && quiet_start()) { web_snap = 0; web_fail(1); in_poll = 0; return; }
        u16 *f = next_frame(300);
        if (!f) { web_snap = 0; web_fail(3); }
        else {
            process_frame(f);
            u64 now = timer_us();
            static u64 prev;
            if (prev && now > prev) fps_x10 = (u32)(10000000ULL / (now - prev));
            prev = now;
            if (web_snap) {
                web_snap = 0;
                if (save_jpeg(0, last_photo) <= 0) { const char *x = "(not saved)"; int i = 0; while (x[i]) { last_photo[i] = x[i]; i++; } last_photo[i] = 0; }
            }
            web_publish();
        }
    } else if (web_stream && streaming && timer_us() - web_last > 10000000ULL) {
        cam_stop();                                        /* nobody watches any more */
    }
    in_poll = 0;
}

/* core 1: a ticket for the next picture */
u32 camera_jpeg_want(void) { return __atomic_add_fetch(&web_req, 1, __ATOMIC_ACQ_REL); }

/* core 1: 1 = the picture (buffer marked busy), 0 = not yet, < 0 = failed (-1 no camera, -2, -3 no frame) */
int camera_jpeg_take(u32 ticket, const u8 **p, u32 *len)
{
    if ((int)(__atomic_load_n(&web_done, __ATOMIC_ACQUIRE) - ticket) < 0) return 0;
    wlock();
    int r = 0;
    if (web_err || web_idx < 0) r = web_err ? -web_err : -2;
    else { int b = web_idx; web_busy[b]++; *p = (const u8 *)(b ? WEB_JPG1 : WEB_JPG0); *len = (u32)web_len[b]; r = 1; }
    wunlock();
    return r;
}

void camera_jpeg_release(const u8 *p)
{
    wlock();
    int b = (unsigned long)p == WEB_JPG1 ? 1 : 0;
    if (web_busy[b] > 0) web_busy[b]--;
    wunlock();
}

/* core 1: one setting from the page ("exposure", "12.5"); "defaults" / "snap" too. 0 ok, -1 bad value, -2 unknown */
int camera_web_set(const char *k, const char *v)
{
    if (!cfg_loaded) return -3;
    if (!strcmp(k, "snap")) { web_snap = 1; return 0; }
    if (!strcmp(k, "defaults")) { web_defaults = 1; return 0; }
    char a[24], b2[24]; int i = 0, j = 0;
    while (v[i] && v[i] != ' ' && i < 23) { a[i] = v[i]; i++; }
    a[i] = 0;
    while (v[i] == ' ') i++;
    while (v[i] && j < 23) { b2[j] = v[i]; i++; j++; }
    b2[j] = 0;
    int r = set_one(k, a, j ? b2 : 0);
    if (r) return r;
    if (!strcmp(k, "wb") && s_wb == 2) { gr_cur = (u32)s_wbr * 256 / 100; gb_cur = (u32)s_wbb * 256 / 100; }
    web_prep = 1;
    web_save_at = timer_us() + 2000000;
    return 0;
}

/* core 1: settings + what the camera does now, as JSON members (no braces) */
int camera_web_json(char *b, int max)
{
    char t[300]; int n = 0;
    if (max < 800) return 0;
    #define S(x) put_s(b, &n, x)
    S("\"sensor\":\""); S(fake ? "test picture" : sensor ? sname() : "camera"); S("\"");
    S(",\"streaming\":"); S(streaming ? "true" : "false");
    S(",\"exposure\":\""); if (s_exp10) put_d10(b, &n, s_exp10); else S("auto"); S("\"");
    S(",\"gain\":\""); if (s_gain10) put_d10(b, &n, s_gain10); else S("auto"); S("\"");
    S(",\"ev\":"); put_int(b, &n, s_ev);
    S(",\"wb\":\""); S(s_wb == 0 ? "auto" : s_wb == 1 ? "lock" : "manual"); S("\"");
    S(",\"wb_r\":"); put_int(b, &n, s_wb == 2 ? s_wbr : (int)(gr_cur * 100 / 256));
    S(",\"wb_b\":"); put_int(b, &n, s_wb == 2 ? s_wbb : (int)(gb_cur * 100 / 256));
    S(",\"brightness\":"); put_int(b, &n, s_bri);
    S(",\"contrast\":"); put_int(b, &n, s_con);
    S(",\"saturation\":"); put_int(b, &n, s_sat);
    S(",\"hue\":"); put_int(b, &n, s_hue);
    S(",\"gamma\":"); S(s_gamma ? "true" : "false");
    S(",\"flip\":\""); S(s_flip == 0 ? "off" : s_flip == 1 ? "h" : s_flip == 2 ? "v" : "hv"); S("\"");
    S(",\"quality\":"); put_int(b, &n, s_q);
    S(",\"sharpness\":"); put_int(b, &n, s_sharp);
    S(",\"cores\":"); put_int(b, &n, s_cores);
    S(",\"flicker\":"); put_int(b, &n, s_flicker);
    S(",\"ir_pin\":"); put_int(b, &n, ir_pin);
    S(",\"ir_mode\":\""); S(ir_mode == 0 ? "off" : ir_mode == 1 ? "on" : ir_mode == 2 ? "photo" : "auto"); S("\"");
    S(",\"ir_on\":"); S(ir_now > 0 ? "true" : "false");
    S(",\"now_exposure_ms\":"); put_d10(b, &n, ae_exp * LINE_US10 / 1000);
    S(",\"now_gain\":"); put_d10(b, &n, ae_gain * 10 / 16);
    S(",\"fps\":"); put_d10(b, &n, (int)fps_x10);
    S(",\"width\":"); put_int(b, &n, W / 2);
    S(",\"height\":"); put_int(b, &n, H / 2);
    S(",\"last_photo\":\""); S(last_photo); S("\"");
    #undef S
    (void)t;
    b[n] = 0;
    return n;
}


/* ---- 0.108: microscope - full-screen live picture with zoom, photo and MJPEG AVI video ----
 * Zoom 1-5x (keys 1-5, + / -) crops the SENSOR picture (1296x972) and demosaics it with a sliding 2x2
 * window, so 2x is real detail, not a blown-up picture; arrows move the crop, c centres it. The picture
 * is drawn as big as the screen allows (keeping 4:3). Space / p = photo (JPEG, /CAM/<date>/), v = video
 * start / stop: every shown frame is JPEG-encoded into RAM (REC_BUF, 192 MB) as an MJPEG AVI, written to
 * the card when stopped (fat_write_file has no append). q / Enter = quit. Low delay first: nothing waits
 * for the card while the picture runs. */
#define REC_BUF  0x04000000UL                             /* the picture heap - nothing else uses it meanwhile */
#define REC_MAX  (0x0C000000UL - 0x100000UL)
#define REC_IDX  ((u32 *)0x03800000UL)                     /* 2 words per frame: offset, size (8 MB) */
#define REC_HDR  224                                      /* RIFF + hdrl + LIST movi headers */
static u32 rec_len, rec_frames;
static u64 rec_t0;

static void w32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void w16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void w4c(u8 *p, const char *c) { p[0] = (u8)c[0]; p[1] = (u8)c[1]; p[2] = (u8)c[2]; p[3] = (u8)c[3]; }

static void rec_start(void) { rec_len = REC_HDR; rec_frames = 0; rec_t0 = timer_us(); }

/* the picture in RGB_BUF as one more frame; 0 = ok, -1 = full */
static int rec_frame(void)
{
    u8 *b = (u8 *)REC_BUF;
    if (rec_len + 8 + 2 * 1024 * 1024 + 16 * (rec_frames + 2) > REC_MAX || rec_frames >= 500000) return -1;
    int n = jpeg_encode((const u32 *)RGB_BUF, ow, oh, ow, s_q, b + rec_len + 8, 2 * 1024 * 1024);
    if (n <= 0) return 0;
    w4c(b + rec_len, "00dc"); w32(b + rec_len + 4, (u32)n);
    REC_IDX[rec_frames * 2] = rec_len - (REC_HDR - 4);   /* from the 'movi' fourcc */
    REC_IDX[rec_frames * 2 + 1] = (u32)n;
    rec_len += 8 + (u32)n;
    if (n & 1) b[rec_len++] = 0;
    rec_frames++;
    return 0;
}

static int prog_page = -1;
static u64 prog_t0;
static void rec_progress(u32 done, u32 len)                  /* 0.114: "saving video 45 %" while it is written */
{
    if (prog_page < 0) return;
    char l[96]; int n = 0;
    u32 ms = (u32)((timer_us() - prog_t0) / 1000);
    put_s(l, &n, "saving video to the card: "); put_int(l, &n, (int)((u64)done * 100 / len)); put_s(l, &n, " %  (");
    put_int(l, &n, (int)(done >> 20)); put_s(l, &n, " / "); put_int(l, &n, (int)(len >> 20)); put_s(l, &n, " MB");
    if (done && ms > 2000) { u32 left = (u32)((u64)(len - done) * ms / done / 1000); put_s(l, &n, ", "); put_int(l, &n, (int)left); put_s(l, &n, " s left"); }
    put_s(l, &n, ")          "); l[n] = 0;
    fb_text_page(prog_page, 8, (int)fb_height() - 18, l, 0xFFFF00, 0);
}

/* finish the AVI (headers + idx1) and write it; prints the result */
static void rec_save(char *name_out)
{
    u8 *b = (u8 *)REC_BUF;
    u32 ms = (u32)((timer_us() - rec_t0) / 1000);
    if (!rec_frames) { kprintf("video: no frames\n"); return; }
    if (!ms) ms = 1;
    u32 movi_len = rec_len - (REC_HDR - 12) - 8;          /* LIST size counts from 'movi' */
    u32 idx = rec_len;
    w4c(b + idx, "idx1"); w32(b + idx + 4, rec_frames * 16);
    for (u32 i = 0; i < rec_frames; i++) {
        u8 *e = b + idx + 8 + i * 16;
        w4c(e, "00dc"); w32(e + 4, 0x10); w32(e + 8, REC_IDX[i * 2]); w32(e + 12, REC_IDX[i * 2 + 1]);
    }
    u32 total = idx + 8 + rec_frames * 16;
    u32 rate = (u32)((u64)rec_frames * 1000000ULL / ms);  /* frames per 1000 s */
    if (!rate) rate = 1;
    u32 maxf = 0; for (u32 i = 0; i < rec_frames; i++) if (REC_IDX[i * 2 + 1] > maxf) maxf = REC_IDX[i * 2 + 1];
    for (int i = 0; i < REC_HDR; i++) b[i] = 0;
    w4c(b, "RIFF"); w32(b + 4, total - 8); w4c(b + 8, "AVI ");
    w4c(b + 12, "LIST"); w32(b + 16, 192); w4c(b + 20, "hdrl");
    w4c(b + 24, "avih"); w32(b + 28, 56);
    w32(b + 32, (u32)((u64)ms * 1000 / rec_frames));     /* us per frame */
    w32(b + 36, (u32)((u64)(total / ms) * 1000)); w32(b + 44, 0x10); w32(b + 48, rec_frames);
    w32(b + 56, 1); w32(b + 60, maxf + 8); w32(b + 64, (u32)ow); w32(b + 68, (u32)oh);
    w4c(b + 88, "LIST"); w32(b + 92, 116); w4c(b + 96, "strl");
    w4c(b + 100, "strh"); w32(b + 104, 56);
    w4c(b + 108, "vids"); w4c(b + 112, "MJPG");
    w32(b + 128, 1000); w32(b + 132, rate); w32(b + 140, rec_frames); w32(b + 144, maxf + 8); w32(b + 148, 0xFFFFFFFF);
    w16(b + 160, (u32)ow); w16(b + 162, (u32)oh);
    w4c(b + 164, "strf"); w32(b + 168, 40);
    w32(b + 172, 40); w32(b + 176, (u32)ow); w32(b + 180, (u32)oh); w16(b + 184, 1); w16(b + 186, 24);
    w4c(b + 188, "MJPG"); w32(b + 192, (u32)(ow * oh * 3));
    w4c(b + 212, "LIST"); w32(b + 216, movi_len); w4c(b + 220, "movi");
    char p[64];
    if (!fat_mounted() || auto_name_x(p, ".AVI", "VID")) { kprintf("video: no SD card / no free name\n"); return; }
    u64 t0 = timer_us();
    prog_t0 = t0;
    fat_write_progress = rec_progress;
    int wr = fat_write_file(p, b, total);
    fat_write_progress = 0;
    if (wr) { kprintf("video: could not write %s\n", p); return; }
    kprintf("video %s: %u frames, %u.%u s, %u.%u fps, %u KB (written in %u ms)\n", p, rec_frames, ms / 1000, ms / 100 % 10,
            rate / 1000, rate / 100 % 10, (total + 1023) / 1024, (u32)((timer_us() - t0) / 1000));
    if (name_out) { int i = 0; while (p[i] && i < 63) { name_out[i] = p[i]; i++; } name_out[i] = 0; }
}

static int micro_key(void)
{
    int c = con_trygetc();
    if (c != 27) return c;
    c = con_getc();
    if (c != '[' && c != 'O') return -1;
    c = con_getc();
    return c == 'A' ? K_UP : c == 'B' ? K_DOWN : c == 'C' ? K_RIGHT : c == 'D' ? K_LEFT : -1;
}

void camera_microscope(void)
{
    if (!fb_ready() || fb_width() < 320) { kprintf("the microscope needs the HDMI screen\n"); return; }
    cfg_load();
    if (streaming) cam_stop();                             /* the web view may run in another mode */
    int old_mode = vga_mode, oldW = W, oldH = H;
    vga_mode = 3; W = 1920; H = 1080;                      /* 1920x1080: every sensor pixel, 16:9 like the screen */
    in_show = 1;
    test_pattern = 0;
    int t = trace; trace = 0;
    int r = cam_start();
    trace = t;
    u16 *f = 0;
    if (!r) { ae_begin(); f = next_frame(3000); }
    if (!r && !f && vga_mode == 3) {                       /* the 1080p mode gave nothing: back to the mode that works */
        cam_stop();
        kprintf("no picture in 1920x1080 - using 1296x972\n");
        vga_mode = 0; W = 1296; H = 972;
        r = cam_start();
        if (!r) { ae_begin(); f = next_frame(3000); }
    }
    if (r || !f) {
        if (!r) cam_stop();
        if (!r) kprintf("no picture from the camera\n");
        vga_mode = old_mode; W = oldW; H = oldH; in_show = 0;
        return;
    }
    web_stream = 0;
    zm10 = 10; pan_x = W / 2; pan_y = H / 2;
    fb_hold(1);
    int sw = (int)fb_width(), shh = (int)fb_height() - 20;
    /* 0.111: two pages when the screen has them - draw the hidden one, then show it whole (no tearing:
     * drawing into the shown page in 3 bands at once left smears in motion) */
    int npg = fb_pages() > 1 ? 2 : 1, bk = npg > 1 ? 1 : 0;
    int pitch = (int)(fb_row(1) - fb_row(0));
    for (int pg = 0; pg < npg; pg++)
        for (u32 y = 0; y < fb_height(); y++) { u32 *rw = fb_page_row(pg, (int)y); for (u32 x = 0; x < fb_width(); x++) rw[x] = 0; }
    int rec = 0, fails = 0, fill = 1, vw = W / 2, vh = H / 2;
    char nm[64] = "", msg[64] = "";
    u64 last = timer_us(), msg_until = 0;
    u32 fe0 = n_fe, fps = 0, ms_frame = 0;
    for (;;) {
        if (!f) f = next_frame(500);
        if (!f) { if (++fails >= 6) { kprintf("the camera stopped sending pictures\n"); break; } continue; }
        fails = 0;
        u64 tf = timer_us();
        frame_stats(f);
        /* the visible part of the sensor: zoom + pan, then fill (crop to the screen shape) or fit */
        int cw = W * 10 / zm10, ch = H * 10 / zm10;
        int dw = sw, dh = shh, vcw = cw, vch = ch;
        if (fill) { if ((long)sw * ch > (long)shh * cw) vch = (int)((long)cw * shh / sw); else vcw = (int)((long)ch * sw / shh); }
        else { if ((long)sw * ch > (long)shh * cw) dw = (int)((long)shh * cw / ch); else dh = (int)((long)sw * ch / cw); }
        int x0 = pan_x - vcw / 2, y0 = pan_y - vch / 2;
        if (x0 > W - vcw - 2) x0 = W - vcw - 2;
        if (y0 > H - vch - 2) y0 = H - vch - 2;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        pan_x = x0 + vcw / 2; pan_y = y0 + vch / 2;
        int ox = (sw - dw) / 2, oy = (shh - dh) / 2;
        render_par(f, fb_page_row(bk, oy) + ox, pitch, dw, dh, x0, y0, vcw, vch);
        ms_frame = (u32)((timer_us() - tf) / 1000);
        if (rec || __atomic_load_n(&web_req, __ATOMIC_ACQUIRE) != web_done) {   /* video / web: a fixed-size copy */
            render_par(f, (u32 *)RGB_BUF, vw, vw, vh, x0, y0, vcw, vch);
            ow = vw; oh = vh;
            web_publish();
            if (rec && rec_frame()) { rec = 0; fb_text_page(bk ^ (npg > 1), 8, (int)fb_height() - 18, "memory full - saving video...                    ", 0xFFFF00, 0); prog_page = bk ^ (npg > 1); rec_save(nm); prog_page = -1; msg_until = timer_us() + 5000000; }
        }
        u16 *fphoto = f;
        f = 0;
        u64 now = timer_us();
        if (now - last >= 1000000) { fps = (n_fe - fe0) * 10000000 / (u32)(now - last); fe0 = n_fe; last = now; }
        {   /* status line */
            char l[220]; int n = 0;
            put_int(l, &n, W); put_s(l, &n, "x"); put_int(l, &n, H);
            put_s(l, &n, "  zoom "); put_d10(l, &n, zm10); put_s(l, &n, fill ? "x fill  " : "x fit  ");
            put_d10(l, &n, (int)fps); put_s(l, &n, " fps "); put_int(l, &n, (int)ms_frame); put_s(l, &n, " ms ");
            put_int(l, &n, s_cores); put_s(l, &n, s_cores > 1 ? " cores  " : " core  ");
            if (rec) { u32 s2 = (u32)((now - rec_t0) / 1000000); put_s(l, &n, "REC "); put_int(l, &n, (int)(s2 / 60)); put_s(l, &n, s2 % 60 < 10 ? ":0" : ":"); put_int(l, &n, (int)(s2 % 60));
                       put_s(l, &n, " "); put_int(l, &n, (int)(rec_len >> 20)); put_s(l, &n, " MB  "); }
            if (now < msg_until && nm[0]) { put_s(l, &n, nm); put_s(l, &n, "  "); }
            else if (now < msg_until && msg[0]) { put_s(l, &n, msg); put_s(l, &n, "  "); }
            put_s(l, &n, "+- zoom (1-7), arrows, c centre, f fill/fit, k cores, space photo, v video start/stop, q quit");
            int maxc = sw / 8 - 2;
            while (n < maxc && n < 210) l[n++] = ' ';
            if (n > maxc) n = maxc;
            l[n] = 0;
            fb_text_page(bk, 8, (int)fb_height() - 18, l, rec ? 0xFF6060 : 0xFFFFFF, 0);
        }
        if (npg > 1) { fb_show_page(bk); bk ^= 1; }      /* the finished picture at once */
        int k = micro_key();
        if (k < 0) continue;
        int step = (W * 10 / zm10) / 8;
        /* 0.112: finer zoom, up to 2.5x (owner: more than that only blurs) */
        static const int zl[] = { 10, 12, 15, 18, 20, 22, 25 };
        int zi = 0; while (zi < 6 && zl[zi] < zm10) zi++;
        if (k >= '1' && k <= '7') zm10 = zl[k - '1'];
        else if (k == '+' || k == '=') { if (zi < 6) zm10 = zl[zi + 1]; }
        else if (k == '-') { if (zi > 0) zm10 = zl[zi - 1]; }
        else if (k == K_LEFT) pan_x += (s_flip & 1) ? step : -step;
        else if (k == K_RIGHT) pan_x += (s_flip & 1) ? -step : step;
        else if (k == K_UP) pan_y += (s_flip & 2) ? step : -step;
        else if (k == K_DOWN) pan_y += (s_flip & 2) ? -step : step;
        else if (k == 'c' || k == 'C') { pan_x = W / 2; pan_y = H / 2; }
        else if (k == 'f' || k == 'F') { fill = !fill; for (int pg = 0; pg < npg; pg++) for (u32 y = 0; y < (u32)shh; y++) { u32 *rw = fb_page_row(pg, (int)y); for (int x = 0; x < sw; x++) rw[x] = 0; } }
        else if (k == 'k' || k == 'K') { s_cores = s_cores >= 3 ? 1 : s_cores + 1; web_save_at = timer_us() + 2000000; }
        else if (k == ' ' || k == 'p' || k == 'P' || k == 's' || k == 'S') {
            /* the photo: what is on the screen, at the sensor's own resolution (up to 1920x1080) */
            int pw = vcw, ph = vch;
            if (pw > 1920) { ph = ph * 1920 / pw; pw = 1920; }
            render_par(fphoto, (u32 *)RGB_BUF, pw, pw, ph, x0, y0, vcw, vch);
            ow = pw; oh = ph;
            if (save_jpeg(0, nm) <= 0) { const char *x = "photo NOT saved"; int i = 0; while (x[i]) { msg[i] = x[i]; i++; } msg[i] = 0; nm[0] = 0; }
            msg_until = timer_us() + 3000000;
        }
        else if ((k == '\r' || k == '\n') && rec) k = 'v';     /* 0.114: Enter while recording = stop the video, not quit */
        if (k == 'v' || k == 'V' || k == 'r' || k == 'R') {
            if (!rec) { rec_start(); rec = 1; nm[0] = 0; }
            else {
                rec = 0;
                ow = vw; oh = vh;
                fb_text_page(bk ^ (npg > 1), 8, (int)fb_height() - 18, "saving video to the card...                       ", 0xFFFF00, 0);
                prog_page = bk ^ (npg > 1);
                rec_save(nm);
                prog_page = -1;
                msg_until = timer_us() + 5000000;
            }
        }
        else if (k == 'q' || k == 'Q' || k == 3) break;     /* 0.114: only q / Ctrl+C quit (Enter quit by mistake) */
        if (pan_x < 0) pan_x = 0;
        if (pan_x > W) pan_x = W;
        if (pan_y < 0) pan_y = 0;
        if (pan_y > H) pan_y = H;
    }
    if (rec) { ow = vw; oh = vh; fb_text_page(bk ^ (npg > 1), 8, (int)fb_height() - 18, "saving video to the card...                       ", 0xFFFF00, 0); prog_page = bk ^ (npg > 1); rec_save(0); prog_page = -1; }
    if (npg > 1) fb_show_page(0);
    zm10 = 10;
    cam_stop();
    vga_mode = old_mode; W = oldW; H = oldH;
    fb_hold(0); fb_redraw();
    in_show = 0;
}

/* live: picture until a key (s / space = save a JPEG); else one picture (after the exposure settled),
 * saved as JPEG when save (file 0 = auto name), shown until a key */
static void show_(int live, int save, const char *file);
static void show(int live, int save, const char *file)
{
    in_show = 1;
    show_(live, save, file);
    in_show = 0;
}

static void show_(int live, int save, const char *file)
{
    cfg_load();
    if (cam_start()) return;
    web_stream = 0;
    ae_begin();
    int fb = fb_ready();
    int scale = 1;                                       /* one RGB pixel per 2x2 Bayer square: W/2 x H/2 */
    while (fb && (u32)(W / 2) * (scale + 1) <= fb_width() && 24 + (u32)(H / 2) * (scale + 1) + 24 <= fb_height() && scale < 3) scale++;
    if (fb && (fb_width() < (u32)(W / 2) || fb_height() < 24 + (u32)(H / 2) + 24)) fb = 0;   /* no room for picture + text */
    STEP("cam 8: first frame...\n");
    u16 *f0 = 0;
    for (int i = 0; i < 6 && !f0; i++) {                 /* 0.91: 6 x 0.5 s, what the lanes and the sensor do meanwhile */
        f0 = next_frame(500);
        if (!f0 && trace) {
            u8 e1 = 0xEE, e2 = 0xEE, g = 0xEE;
            rd16(saddr, 0x3501, &e1); rd16(saddr, 0x3502, &e2); rd16(saddr, 0x350B, &g);
            kprintf("  %u ms: STA 0x%x CLK %x DAT0 %x DAT1 %x wp 0x%x | exposure %02x%02x gain %02x\n",
                    (i + 1) * 500, sta_or, ur(0x010), ur(0x018), ur(0x01C), ur(0x11C), e1, e2, g);
        }
    }
    STEP("cam 9: %s (FS %u FE %u, STA 0x%x, write pointer 0x%x)\n", f0 ? "frame OK" : "NO frame", n_fs, n_fe, sta_or, ur(0x11C));
    u64 t0 = timer_us(), last = t0;
    u32 shown = 0, fe0 = 0;
    int got = f0 != 0;
    if (got && !live) {                                  /* let exposure + white balance settle (max ~2 s) */
        for (int i = 0; i < 60; i++) {
            u16 *f = next_frame(500);
            if (!f) break;
            process_frame(f);
            if (ae_ok >= 2 && i >= 4) break;
            if (con_trygetc() >= 0) break;
        }
    }
    if (fb) { fb_hold(1); for (u32 y = 0; y < fb_height(); y++) { u32 *r = fb_row((int)y); for (u32 x = 0; x < fb_width(); x++) r[x] = 0; } }
    int x0 = fb ? ((int)fb_width() - (W / 2) * scale) / 2 : 0, y0 = 24;
    if (fb) fb_text_px(8, 4, live ? "camera live - s or space saves a JPEG, other keys stop" : "camera picture - any key", 0xFFFFFF, 0);
    int saved = 0;
    char nm[64]; nm[0] = 0;
    for (;;) {
        u16 *f = got ? next_frame(got ? 500 : 3000) : 0;
        if (f) {
            shown++;
            process_frame(f);
            web_publish();                                 /* a web page watching meanwhile */
            if (fb) blit(x0, y0, scale);
            if (!live) {
                if (save) { int r = save_jpeg(file, nm); saved = r > 0; if (fb) fb_text_px(x0, y0 + (H / 2) * scale + 8, r > 0 ? nm : "NOT saved", 0xFFFFFF, 0); }
                break;
            }
        } else break;
        u64 now = timer_us();
        if (now - last >= 1000000) {
            char l[96]; int n = 0;
            u32 fps = (n_fe - fe0) * 1000000 / (u32)(now - last); fe0 = n_fe; last = now;
            put_s(l, &n, "fps "); put_int(l, &n, (int)fps);
            put_s(l, &n, "  exposure "); put_d10(l, &n, ae_exp * LINE_US10 / 1000); put_s(l, &n, " ms  gain ");
            put_d10(l, &n, ae_gain * 10 / 16); put_s(l, &n, "x   ");
            if (nm[0]) { put_s(l, &n, nm); put_s(l, &n, "   "); }
            l[n] = 0;
            if (fb) fb_text_px(x0, y0 + (H / 2) * scale + 8, l, 0xFFFFFF, 0);
        }
        int k = con_trygetc();
        if (k == 's' || k == 'S' || k == ' ') {
            if (save_jpeg(0, nm) > 0) saved++;
            else { nm[0] = 0; }
            last = 0;
        } else if (k >= 0) break;
    }
    u32 ibwp = ur(0x11C), lclk = ur(0x010), ld0 = ur(0x018), ld1 = ur(0x01C), sta_now = ur(0x004), ista = ur(0x104);
    if (!live && shown && fb) { while (con_trygetc() < 0) delay_us(20000); }
    cam_stop();
    if (fb) { fb_hold(0); fb_redraw(); }
    if (got) {
        kprintf("frames: %u shown in %u ms, exposure %u.%u ms, gain %u.%ux, white balance R %u%% B %u%%",
                shown, (u32)((timer_us() - t0) / 1000), (u32)(ae_exp * LINE_US10 / 10000), (u32)(ae_exp * LINE_US10 / 1000 % 10),
                (u32)(ae_gain / 16), (u32)(ae_gain * 10 / 16 % 10), gr_cur * 100 / 256, gb_cur * 100 / 256);
        if (saved && live) kprintf(", %d saved", saved);
        kprintf("\n");
    }
    if (got && !trace) return;
    kprintf("frames: start %u, end %u\nreceiver status:", n_fs, n_fe); print_sta(sta_or); if (!sta_or) kprintf(" (nothing)");
    kprintf("\nwrite pointer 0x%x (buffers 0x%x / 0x%x), lanes CLK %x DAT0 %x DAT1 %x, STA 0x%x ISTA 0x%x\n",
            ibwp, (u32)BUF_A | 0xC0000000u, (u32)BUF_B | 0xC0000000u, lclk, ld0, ld1, sta_now, ista);
    if (!got) kprintf("NO PICTURE: no complete frame came in ('cam trace on' and send a photo)\n");
}


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
            kprintf("'cam live' = picture on HDMI, 'cam snap' = JPEG to the card, 'cam set' = settings, 'cam bars' = test bars\n"
                    "web page with sliders + preview: http://<board>/camera ('http on' + 'http token <secret>')\n");
        } else {
            kprintf("no sensor: check the ribbon (contacts towards the HDMI port on the Pi 3,\n"
                    "the blue side towards the Ethernet port), and that it is the CAMERA socket\n");
        }
        return;
    }
    if (!strcmp(a, "off")) { cam_stop(); power(0); sensor = 0; kprintf("camera power off\n"); return; }
    if (!strcmp(a, "test")) {
        if (streaming) cam_stop();
        fake = argc > 2 && (!strcmp(argv[2], "on") || !strcmp(argv[2], "dark"));
        fake_dark = argc > 2 && !strcmp(argv[2], "dark");
        nocam_until = 0;
        kprintf("test picture %s\n", fake ? "ON: made-up colour bars instead of the camera (until reboot or 'cam test off')" : "off");
        return;
    }
    if (!strcmp(a, "trace")) { trace = argc > 2 && !strcmp(argv[2], "off") ? 0 : 1; kprintf("step trace %s\n", trace ? "on" : "off"); return; }
    if (!strcmp(a, "mode") && argc > 2) {
        if (streaming) { kprintf("stop the camera first\n"); return; }
        vga_mode = !strcmp(argv[2], "640") ? 1 : !strcmp(argv[2], "1296") ? 0 : !strcmp(argv[2], "1080") ? 3 : 2;
        W = vga_mode == 1 ? 640 : vga_mode == 3 ? 1920 : 1296; H = vga_mode == 1 ? 480 : vga_mode == 0 ? 972 : vga_mode == 3 ? 1080 : 976;
        kprintf("sensor mode %dx%d (until reboot)\n", W, H);
        return;
    }
    if (!strcmp(a, "gate") && argc > 2) {
        u64 v; if (!parse_num(argv[2], &v) || v > 0xFFF) { kprintf("cam gate <hex value, e.g. 0x155 or 0x15>\n"); return; }
        gate_val = (int)v; kprintf("lane clock gate 0x%x (until reboot)\n", gate_val);
        return;
    }
    if (!strcmp(a, "bayer") && argc > 2) {
        static const char *nm[] = { "bggr", "gbrg", "grbg", "rggb" };
        bayer = -1;
        for (int i = 0; i < 4; i++) if (!strcmp(argv[2], nm[i])) bayer = i;
        kprintf("colour order: %s\n", bayer < 0 ? "auto" : nm[bayer]);
        return;
    }
    if (!strcmp(a, "order") && argc > 2) {
        sensor_first = strcmp(argv[2], "rx") != 0;
        kprintf("start order: %s (until reboot)\n", sensor_first ? "sensor first, then the receiver" : "receiver first, then the sensor");
        return;
    }
    if (!strcmp(a, "clock") && argc > 2) {
        cont_clk = !strcmp(argv[2], "cont");
        kprintf("MIPI clock: %s (test setting until reboot)\n", cont_clk ? "continuous" : "non-continuous (default)");
        return;
    }
    if (!strcmp(a, "live")) { test_pattern = 0; show(1, 0, 0); return; }
    if (!strcmp(a, "snap")) { test_pattern = 0; show(0, 1, argc > 2 ? argv[2] : 0); return; }
    if (!strcmp(a, "micro") || !strcmp(a, "microscope")) { camera_microscope(); return; }
    if (!strcmp(a, "send")) {
        int r = camera_take(argc > 2 && !strcmp(argv[2], "save"), 1, 0);
        if (r) kprintf("not sent: %s\n", camera_take_error(r));
        return;
    }
    if (!strcmp(a, "view")) { test_pattern = 0; show(0, 0, 0); return; }
    if (!strcmp(a, "set") || !strcmp(a, "settings")) {
        cfg_load();
        char b[400]; cfg_text(b);
        kprintf("camera settings (/CAM.TXT):\n");
        for (char *l = b; *l; ) { char *e = l; while (*e && *e != '\n') e++; char sv = *e; *e = 0; kprintf("  %s\n", l); *e = sv; l = *e ? e + 1 : e; }
        kprintf("change: cam exposure auto|<ms>, gain auto|<x>, ev <-3..3>, wb auto|lock|<r%%> <b%%>,\n"
                "        brightness|contrast <-100..100>, saturation <0..300>, hue <-180..180>,\n"
                "        gamma on|off, flip off|h|v|hv, quality <10..100>, sharpness <0..200>, flicker 50|60|off, cores 1-3; cam defaults\n"
                "IR light: cam ir <gpio>|off, cam ir_mode off|on|photo|auto, cam ir_active high|low\n");
        return;
    }
    if (!strcmp(a, "defaults")) {
        cfg_load();
        s_exp10 = s_gain10 = s_ev = s_wb = s_bri = s_con = s_hue = s_flip = 0;
        s_wbr = s_wbb = s_sat = 100; s_gamma = 1; s_q = 85; gr_cur = gb_cur = 256; s_sharp = 30; s_flicker = 50;
        prep(); cfg_save();
        kprintf("camera settings back to the defaults\n");
        return;
    }
    if (argc > 2) {
        cfg_load();
        int r = set_one(a, argv[2], argc > 3 ? argv[3] : 0);
        if (r == 0) {
            if (!strcmp(a, "wb") && s_wb == 2) { gr_cur = (u32)s_wbr * 256 / 100; gb_cur = (u32)s_wbb * 256 / 100; }
            prep(); cfg_save();
            char b[400]; cfg_text(b);
            for (char *l = b; *l; ) { char *e = l; while (*e && *e != '\n') e++; char sv = *e; *e = 0;
                if (!strncmp(l, a, strlen(a)) && l[strlen(a)] == ' ') kprintf("%s (saved in /CAM.TXT)\n", l);
                *e = sv; l = *e ? e + 1 : e; }
            if (!strcmp(a, "wb") && s_wb == 1) kprintf("white balance locked at the current values\n");
            return;
        }
        if (r == -1) { kprintf("bad value for '%s' ('cam set' shows the ranges)\n", a); return; }
    }
    if (!strcmp(a, "dump")) {                         /* start, wait 1 s, print every register, stop */
        test_pattern = 1;
        int t = trace; trace = 0;
        if (!cam_start()) { delay_us(1000000); next_frame(500); dump_regs(); kprintf("FS %u FE %u STA seen 0x%x\n", n_fs, n_fe, sta_or); cam_stop(); }
        trace = t; test_pattern = 0;
        return;
    }
    if (!strcmp(a, "sregs")) {                        /* start (no bars), 1 s, every sensor register we set, stop */
        int t = trace; trace = 0; test_pattern = 0;
        if (!cam_start()) { delay_us(1000000); dump_sensor(); cam_stop(); }
        trace = t;
        return;
    }
    if (!strcmp(a, "bars")) { test_pattern = 1; show(1, 0, 0); test_pattern = 0; return; }
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
            "       cam live            live picture on HDMI, s / space saves a JPEG, other keys stop\n"
            "       cam snap [file]     one picture saved as JPEG (default /CAM/<date>/<time>.JPG)\n"
            "       cam view            one picture on HDMI only, any key\n"
            "       microscope          full screen, zoom 1-2.5x, photo + video (also 'cam micro')\n"
            "       cam send [save]     one photo to MQTT <base>/camera (Home Assistant camera), save = also to the card\n"
            "       cam set             picture settings: exposure, gain, ev, wb, brightness, contrast,\n"
            "                           saturation, hue, gamma, flip, quality (saved in /CAM.TXT)\n"
            "       cam <setting> <value>  e.g. cam saturation 130, cam exposure 10, cam wb auto\n"
            "       cam defaults        settings back to the defaults\n"
            "       cam ir <gpio>|off   IR light on a pin; cam ir_mode off|on|photo|auto (auto = only in the dark)\n"
            "       cam bars            the sensor's test colour bars (checks the link without the lens)\n"
            "       cam dump            receiver registers while streaming (compare with Linux: tools/camdump.py)\n"
            "       cam mode 1296|1080|fw|640 sensor mode (default 1296 = 1296x972 whole picture; 1080 = 1920x1080 centre)\n"
            "       cam gate 0x15|0x155 lane clock gate (default 0x15 like Linux)\n"
            "       cam bayer auto|bggr|gbrg|grbg|rggb   colour order of the sensor pixels (if colours look wrong)\n"
            "       cam order sensor|rx start order (default rx = receiver first, like Linux)\n"
            "       cam sregs           sensor registers while streaming (compare with Linux: tools/ov5647regs.sh)\n"
            "       cam clock cont|ncont  MIPI clock mode (test; default ncont)\n"
            "       cam reg <a> [v]     read / write a sensor register (16-bit address)\n"
            "       cam test on|off     a made-up test picture instead of the camera (tests the web page)\n"
            "       cam off             camera power off\n");
}
