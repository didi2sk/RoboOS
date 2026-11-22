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
#define W 640
#define H 480
#define FRAME_BYTES (W * H * 2)
#define BUF_A 0x13000000UL
#define BUF_B 0x13100000UL
static int streaming, cur_buf;
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
static const struct rv vga[] = {
    {0x3036,0x46},{0x3821,0x01},{0x3820,0x41},{0x3612,0x59},{0x3618,0x00},{0x3814,0x35},{0x3815,0x35},
    {0x3708,0x64},{0x3709,0x52},{0x3800,0x00},{0x3801,0x10},{0x3802,0x00},{0x3803,0x00},{0x3804,0x0a},
    {0x3805,0x2f},{0x3806,0x07},{0x3807,0x9f},{0x3808,0x02},{0x3809,0x80},{0x380a,0x01},{0x380b,0xe0},
    {0x3a09,0x2e},{0x3a0a,0x00},{0x3a0b,0xfb},{0x3a0d,0x02},{0x3a0e,0x01},{0x4004,0x02},{0x4800,0x34},
    {0x380c,0x07},{0x380d,0x3c},{0x380e,0x01},{0x380f,0xf8},          /* HTS 1852, VTS 0x1f8 (mode values) */
};
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
    if (on) {
        if (wr_list(common, sizeof(common) / sizeof(common[0]))) return -1;
        if (wr_list(vga, sizeof(vga) / sizeof(vga[0]))) return -1;
        u8 v = 0;
        if (!rd16(saddr, 0x4814, &v)) wr16(saddr, 0x4814, v & 0x3F);   /* virtual channel 0 */
        wr16(saddr, 0x3503, 0x00);                         /* auto exposure + auto gain */
        wr16(saddr, 0x5001, 0x01);                         /* auto white balance */
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
    mmio_write(UNI_GATE, 0x5a000000 | 0x15);             /* clock + 2 data lane gates */
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
    uw(0x004, 0xFFFFFFFF);                               /* clear STA */
    uw(0x104, 7);                                        /* clear ISTA */
    uwf(0x014, 0xFF, 2); uwf(0x014, 0xFF00, 6);          /* CLT: tclk_term_en, tclk_settle */
    uwf(0x028, 0xFF, 2); uwf(0x028, 0xFF00, 6); uwf(0x028, 0xFF0000, 0);   /* DLT */
    uwf(0x000, 16, 0);                                   /* SOE = 0 */
    uw(0x02C, (1u << 31) | (1u << 9) | (1u << 8) | 1);   /* CMP0: PCE GI CPH, VC 0, DT 1 (frame end) */
    u32 lane = cont_clk ? (1 | 4 | 8 | 16) : (1 | 4);    /* enable + LP enable (+ HS + termination when continuous) */
    uw(0x010, lane);                                     /* CLK */
    uw(0x018, lane);                                     /* DAT0 */
    uw(0x01C, lane);                                     /* DAT1 */
    uw(0x118, W * 2);                                    /* IBLS: bytes per line */
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
    uwf(0x000, 1, 0);
    uw(0x200, 0);
    mmio_write(UNI_GATE, 0x5a000000);
}

static const char *sta_names[] = { "SYN","CS","SBE","PBE","HOE","PLE","SSC","CRCE","OES","IFO","OFO","BFO","DL","PS","IS","PI0","PI1","FSI","FEI","LCI","B0R","B0N","B1R","B1N","DI" };
static void print_sta(u32 s)
{
    for (int i = 0; i < 25; i++) if (s & (1u << i)) kprintf(" %s", sta_names[i]);
}

static int trace = 1;                                    /* 0.90: print each step (a freeze shows where) */
#define STEP(...) do { if (trace) { kprintf(__VA_ARGS__); delay_us(30000); } } while (0)

static int cam_start(void)
{
    if (streaming) return 0;
    if (!sensor && !detect(0)) { kprintf("no camera sensor found ('cam' shows why)\n"); return -1; }
    if (sensor != 1) { kprintf("only the OV5647 can give a picture so far\n"); return -1; }
    u32 g[2] = { 13, 0 };
    int ok = mbox_prop(0x00030030, g, 2);
    STEP("cam 1: power domain UNICAM1 was %s (%u)\n", ok ? (g[1] & 1 ? "on" : "off") : "?? (no answer)", g[1]);
    u32 pd[2] = { 13, 1 };
    ok = mbox_prop(0x00038030, pd, 2);
    g[0] = 13; g[1] = 0;
    int ok2 = mbox_prop(0x00030030, g, 2);
    STEP("cam 2: switched on: answer %s, now %s (%u)\n", ok ? "ok" : "NONE", ok2 ? (g[1] & 1 ? "on" : "OFF") : "?", g[1]);
    cam1_clock(1);
    STEP("cam 3: CAM1 clock ctl 0x%x div 0x%x\n", mmio_read(CM_CAM1CTL), mmio_read(CM_CAM1DIV));
    dcache_clean_inval((void *)BUF_A, 0x200000);
    if (wr_list(oe_on, 3) || sensor_stream(0)) { kprintf("sensor does not answer\n"); return -1; }
    STEP("cam 4: sensor lanes in LP-11, reading the receiver...\n");
    STEP("cam 5: receiver CTRL 0x%x STA 0x%x ANA 0x%x\n", ur(0x000), ur(0x004), ur(0x008));
    delay_us(2000);
    unicam_start();
    STEP("cam 6: receiver on: CTRL 0x%x STA 0x%x CLK 0x%x DAT0 0x%x\n", ur(0x000), ur(0x004), ur(0x010), ur(0x018));
    if (sensor_stream(1)) { kprintf("sensor setup failed\n"); unicam_stop(); return -1; }
    STEP("cam 7: sensor streaming, waiting for frames: STA 0x%x ISTA 0x%x\n", ur(0x004), ur(0x104));
    n_fs = n_fe = sta_or = 0;
    streaming = 1;
    return 0;
}

static void cam_stop(void)
{
    if (!streaming) return;
    sensor_stream(0);
    unicam_stop();
    cam1_clock(0);
    streaming = 0;
}

/* wait for the next complete frame; returns its buffer or 0 (timeout ms) */
static u16 *next_frame(u32 ms)
{
    u64 end = timer_us() + (u64)ms * 1000;
    int started = 0;
    while (timer_us() < end) {
        u32 is = ur(0x104), st = ur(0x004);
        if (st) { sta_or |= st; uw(0x004, st); }
        if (is) uw(0x104, is);
        if (is & 1) {                                    /* frame start: this frame goes to cur_buf, queue the other */
            n_fs++; started = 1;
            set_buf(cur_buf ? BUF_A : BUF_B);
        }
        if ((is & 2) && started) {                       /* frame end */
            n_fe++;
            u64 done = cur_buf ? BUF_B : BUF_A;
            cur_buf ^= 1;
            dcache_clean_inval((void *)done, FRAME_BYTES);
            return (u16 *)done;
        }
    }
    return 0;
}

/* 2x2 Bayer (BGGR) -> 320x240 RGB, grey-world white balance + auto brightness, drawn 2x at (x0, y0) */
static void draw_frame(const u16 *f, int x0, int y0, int scale)
{
    u32 orv = 0; u64 sr = 0, sg = 0, sb = 0;
    for (int y = 0; y < H; y += 8) for (int x = 0; x < W; x += 8) {
        const u16 *p = f + y * W + x;
        orv |= p[0] | p[1] | p[W] | p[W + 1];
        sb += p[0]; sg += p[1] + p[W]; sr += p[W + 1];
    }
    int sh = 0; while (sh < 8 && (orv >> (sh + 8))) sh++;         /* top bit -> 8 bit */
    sg /= 2; if (!sr) sr = 1; if (!sb) sb = 1;
    u32 gr = (u32)(sg * 256 / sr), gb = (u32)(sg * 256 / sb);       /* x256 */
    if (gr > 1024) gr = 1024; if (gb > 1024) gb = 1024;
    for (int y = 0; y < H / 2; y++) {
        const u16 *a = f + (2 * y) * W, *b = a + W;
        for (int s = 0; s < scale; s++) {
            int yy = y0 + y * scale + s;
            if (yy >= (int)fb_height()) break;
            u32 *row = fb_row(yy) + x0;
            for (int x = 0; x < W / 2; x++) {
                u32 B = a[2 * x] >> sh, G = ((u32)a[2 * x + 1] + b[2 * x]) >> (sh + 1), R = b[2 * x + 1] >> sh;
                R = R * gr >> 8; B = B * gb >> 8;
                if (R > 255) R = 255; if (G > 255) G = 255; if (B > 255) B = 255;
                u32 px = (R << 16) | (G << 8) | B;
                for (int k = 0; k < scale; k++) row[x * scale + k] = px;
            }
        }
    }
}

static void show(int live)
{
    if (cam_start()) return;
    int fb = fb_ready() && fb_width() >= 640 && fb_height() >= 480;
    int scale = fb && fb_width() >= 1280 && fb_height() >= 1000 ? 3 : 2;
    if (fb && fb_height() < 24 + 240 * (u32)scale + 24) fb = 0;   /* no room for picture + text */
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
    { STEP("cam 9: %s (FS %u FE %u, STA 0x%x, write pointer 0x%x)\n", f0 ? "frame OK" : "NO frame", n_fs, n_fe, sta_or, ur(0x11C)); if (trace) delay_us(1500000); }
    if (fb) { fb_hold(1); for (u32 y = 0; y < fb_height(); y++) { u32 *r = fb_row((int)y); for (u32 x = 0; x < fb_width(); x++) r[x] = 0; } }
    int x0 = fb ? ((int)fb_width() - 320 * scale) / 2 : 0, y0 = 24;
    if (fb) fb_text_px(8, 4, live ? "camera live - any key stops" : "camera picture - any key", 0xFFFFFF, 0);
    u64 t0 = timer_us(), last = t0;
    u32 shown = 0, fe0 = 0;
    int got = 0;
    for (;;) {
        u16 *f = next_frame(got ? 500 : 3000);
        if (f) {
            got = 1; shown++;
            if (fb) draw_frame(f, x0, y0, scale);
            if (!live) break;
        } else if (!got || !live) break;
        u64 now = timer_us();
        if (now - last >= 1000000) {
            char l[80]; int n = 0;
            u32 fps = (n_fe - fe0) * 1000000 / (u32)(now - last); fe0 = n_fe; last = now;
            const char *t = "fps "; while (*t) l[n++] = *t++;
            if (fps >= 10) l[n++] = (char)('0' + fps / 10);
            l[n++] = (char)('0' + fps % 10); l[n++] = ' '; l[n++] = ' '; l[n] = 0;
            if (fb) fb_text_px(x0, y0 + 240 * scale + 8, l, 0xFFFFFF, 0);
        }
        if (con_trygetc() >= 0) break;
    }
    u32 ibwp = ur(0x11C), lclk = ur(0x010), ld0 = ur(0x018), ld1 = ur(0x01C), sta_now = ur(0x004), ista = ur(0x104);
    u8 s0100 = 0xEE, s4800 = 0xEE, s300d = 0xEE, s3036 = 0xEE, s4814 = 0xEE;
    rd16(saddr, 0x0100, &s0100); rd16(saddr, 0x4800, &s4800); rd16(saddr, 0x300D, &s300d);
    rd16(saddr, 0x3036, &s3036); rd16(saddr, 0x4814, &s4814);
    if (!live && got && fb) { while (con_trygetc() < 0) delay_us(20000); }
    cam_stop();
    if (fb) { fb_hold(0); fb_redraw(); }
    kprintf("frames: start %u, end %u, shown %u in %u ms\n", n_fs, n_fe, shown, (u32)((timer_us() - t0) / 1000));
    kprintf("receiver status:"); print_sta(sta_or); if (!sta_or) kprintf(" (nothing)");
    kprintf("\nwrite pointer 0x%x (buffers 0x%x / 0x%x), lanes CLK %x DAT0 %x DAT1 %x\n",
            ibwp, (u32)BUF_A | 0xC0000000u, (u32)BUF_B | 0xC0000000u, lclk, ld0, ld1);
    kprintf("at the end: STA 0x%x ISTA 0x%x; sensor 0100=%02x 4800=%02x 300d=%02x 3036=%02x 4814=%02x\n",
            sta_now, ista, s0100, s4800, s300d, s3036, s4814);
    if (!got) kprintf("NO PICTURE: no complete frame came in (send a photo of these lines)\n");
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
            kprintf("'cam live' = picture on HDMI, 'cam snap' = one picture, 'cam bars' = test colour bars\n");
        } else {
            kprintf("no sensor: check the ribbon (contacts towards the HDMI port on the Pi 3,\n"
                    "the blue side towards the Ethernet port), and that it is the CAMERA socket\n");
        }
        return;
    }
    if (!strcmp(a, "off")) { cam_stop(); power(0); sensor = 0; kprintf("camera power off\n"); return; }
    if (!strcmp(a, "trace")) { trace = argc > 2 && !strcmp(argv[2], "off") ? 0 : 1; kprintf("step trace %s\n", trace ? "on" : "off"); return; }
    if (!strcmp(a, "clock") && argc > 2) {
        cont_clk = !strcmp(argv[2], "cont");
        kprintf("MIPI clock: %s (test setting until reboot)\n", cont_clk ? "continuous" : "non-continuous (default)");
        return;
    }
    if (!strcmp(a, "live")) { test_pattern = 0; show(1); return; }
    if (!strcmp(a, "snap")) { test_pattern = 0; show(0); return; }
    if (!strcmp(a, "bars")) { test_pattern = 1; show(1); test_pattern = 0; return; }
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
            "       cam live            live picture on HDMI (640x480 sensor mode), any key stops\n"
            "       cam snap            one picture, any key\n"
            "       cam bars            the sensor's test colour bars (checks the link without the lens)\n"
            "       cam clock cont|ncont  MIPI clock mode (test; default ncont)\n"
            "       cam reg <a> [v]     read / write a sensor register (16-bit address)\n"
            "       cam off             camera power off\n");
}
