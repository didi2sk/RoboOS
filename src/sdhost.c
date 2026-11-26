/*
 * SD card on the BCM2835 "SDHOST" controller (0x3F202000), GPIO48-53 ALT0.
 *
 * Linux and Circle use this controller for the SD card and keep the Arasan
 * (EMMC) controller for the WiFi chip (GPIO34-39 ALT3). RoboOS moves the card
 * here so that the Arasan is free for WiFi; sd.c falls back to the Arasan if
 * this does not work. PIO, single blocks, like the Arasan driver.
 * Register names as in the Linux bcm2835 driver (the controller has no public
 * datasheet).
 */
#include "kernel.h"
#include "sd.h"

#define SDH          (MMIO_BASE + 0x202000)
#define SDCMD        (SDH + 0x00)
#define SDARG        (SDH + 0x04)
#define SDTOUT       (SDH + 0x08)
#define SDCDIV       (SDH + 0x0C)
#define SDRSP0       (SDH + 0x10)
#define SDRSP1       (SDH + 0x14)
#define SDRSP2       (SDH + 0x18)
#define SDRSP3       (SDH + 0x1C)
#define SDHSTS       (SDH + 0x20)
#define SDVDD        (SDH + 0x30)
#define SDEDM        (SDH + 0x34)
#define SDHCFG       (SDH + 0x38)
#define SDHBCT       (SDH + 0x3C)
#define SDDATA       (SDH + 0x40)
#define SDHBLC       (SDH + 0x50)

#define C_NEW        0x8000
#define C_FAIL       0x4000
#define C_BUSY       0x0800                  /* wait while the card is busy (R1b) */
#define C_NORESP     0x0400
#define C_LONG       0x0200
#define C_WRITE      0x0080
#define C_READ       0x0040

#define S_CMD_TIMEOUT 0x40
#define S_CRC7       0x10
#define S_ERRORS     0xF8                    /* timeouts, CRC, FIFO */
#define S_CLEAR      0x7F8

#define H_SLOW_CARD  (1 << 3)
#define H_WIDE_EXT   (1 << 2)
#define H_WIDE_INT   (1 << 1)

#define EDM_FIFO(e)  (((e) >> 4) & 0x1F)
#define EDM_FSM(e)   ((e) & 0xF)
#define FSM_IDENT    0
#define FSM_DATA     1

#define GPPUDCLK1    (MMIO_BASE + 0x20009C)

static u32 core_clk, rca, ccs, bus4, hcfg;
static u64 sectors;
static int ready;
static const char *stage = "";
static u32 dbg_hsts, dbg_cmd, dbg_edm;

static int failed(int e)
{
    dbg_hsts = mmio_read(SDHSTS);
    dbg_cmd = mmio_read(SDCMD);
    dbg_edm = mmio_read(SDEDM);
    return e;
}

static int wait_cmd_free(u32 ms)
{
    u64 end = timer_us() + (u64)ms * 1000;
    while (mmio_read(SDCMD) & C_NEW)
        if (timer_us() > end) return SD_TIMEOUT;
    return SD_OK;
}

/* one command; flags: C_* ; *err = SD_OK / error; returns RSP0 */
static u32 cmd(u32 idx, u32 arg, u32 flags, int *err)
{
    *err = SD_OK;
    if (wait_cmd_free(500)) { *err = SD_TIMEOUT; return 0; }
    mmio_write(SDHSTS, S_CLEAR);
    mmio_write(SDARG, arg);
    mmio_write(SDCMD, (idx & 0x3F) | flags | C_NEW);
    if (wait_cmd_free(flags & C_BUSY ? 1000 : 500)) {
        mmio_write(SDCMD, 0);                 /* give up on it */
        *err = SD_TIMEOUT;
        return 0;
    }
    if (mmio_read(SDCMD) & C_FAIL) {
        u32 h = mmio_read(SDHSTS);
        mmio_write(SDHSTS, S_CLEAR);
        /* ACMD41 (R3) has no valid CRC: a CRC7 error there is normal */
        if (!(idx == 41 && (h & S_ERRORS) == S_CRC7)) {
            dbg_hsts = h;
            *err = (h & S_CMD_TIMEOUT) ? SD_TIMEOUT : SD_ERROR;
            return 0;
        }
    }
    return mmio_read(SDRSP0);
}

static u32 acmd(u32 idx, u32 arg, u32 flags, int *err)
{
    cmd(55, rca, 0, err);                      /* CMD55 first (R1, wait for it) */
    if (*err) return 0;
    return cmd(idx, arg, flags, err);
}

static void set_clock(u32 hz)
{
    u32 div = core_clk / hz;
    if (div < 2) div = 2;
    if (core_clk / div > hz) div++;
    div -= 2;
    if (div > 0x7FF) div = 0x7FF;
    mmio_write(SDCDIV, div);
    u32 actual = core_clk / (div + 2);
    mmio_write(SDTOUT, actual / 2);            /* data timeout: half a second */
}

static void pins_alt0(void)
{
    if (!wifi_owns_pins)                                  /* 0.126: never while the WiFi uses them (a card re-init killed WiFi) */
        for (int p = 34; p <= 39; p++) gpio_func(p, 0);   /* the WiFi pins: not on our controller */
    if (board_type() != 0x04) gpio_func(47, 0);
    for (int p = 48; p <= 53; p++) gpio_func(p, 4);       /* ALT0 = SDHOST */
    mmio_write(GPPUD, 2);                                 /* pull-up */
    delay_cycles(150);
    mmio_write(GPPUDCLK1, (1 << 15) | (1 << 16) | (1 << 17) | (1 << 18) | (1 << 19) | (1 << 20) | (1 << 21));
    delay_cycles(150);
    mmio_write(GPPUD, 0);
    mmio_write(GPPUDCLK1, 0);
}

/* move n words through the FIFO (read: into buf, write: from buf) */
static int fifo(u32 *buf, int words, int rd)
{
    u64 end = timer_us() + 500000;
    while (words > 0) {
        u32 edm = mmio_read(SDEDM), have = EDM_FIFO(edm);
        int n = rd ? (int)have : 16 - (int)have;
        if (n <= 0) {
            if (mmio_read(SDHSTS) & S_ERRORS) return SD_ERROR;
            if (timer_us() > end) return SD_TIMEOUT;
            continue;
        }
        if (n > words) n = words;
        for (int i = 0; i < n; i++) {
            if (rd) *buf++ = mmio_read(SDDATA);
            else mmio_write(SDDATA, *buf++);
        }
        words -= n;
    }
    return SD_OK;
}

/* the controller is back in data mode (the block and its CRC are done) */
static int wait_data_idle(u32 ms)
{
    u64 end = timer_us() + (u64)ms * 1000;
    for (;;) {
        u32 s = EDM_FSM(mmio_read(SDEDM));
        if (s == FSM_DATA || s == FSM_IDENT) return SD_OK;
        if (timer_us() > end) return SD_TIMEOUT;
    }
}

/* after a write: the card is programming until CMD13 says "ready, transfer state" */
static int wait_card_ready(u32 ms)
{
    int e;
    u64 end = timer_us() + (u64)ms * 1000;
    for (;;) {
        u32 st = cmd(13, rca, 0, &e);
        if (!e && (st & (1 << 8)) && ((st >> 9) & 0xF) == 4) return SD_OK;
        if (timer_us() > end) return SD_TIMEOUT;
    }
}

int sdhost_init(void)
{
    int e;
    u32 r;
    ready = 0; rca = 0; ccs = 0; bus4 = 0; sectors = 0;
    dbg_hsts = dbg_cmd = dbg_edm = 0;

    stage = "SDHOST power";
    pins_alt0();
    u32 pwr[2] = { 0, 3 };                     /* SD card power on, wait */
    mbox_prop(0x00028001, pwr, 2);
    u32 clk[2] = { 4 /* CORE */, 0 };
    core_clk = mbox_prop(0x00030002, clk, 2) && clk[1] ? clk[1] : 250000000;

    /* reset (as Linux bcm2835_reset_internal) */
    mmio_write(SDVDD, 0);
    mmio_write(SDCMD, 0);
    mmio_write(SDARG, 0);
    mmio_write(SDTOUT, 0xF00000);
    mmio_write(SDCDIV, 0);
    mmio_write(SDHSTS, S_CLEAR);
    mmio_write(SDHCFG, 0);
    mmio_write(SDHBCT, 0);
    mmio_write(SDHBLC, 0);
    u32 edm = mmio_read(SDEDM);
    edm &= ~((0x1Fu << 14) | (0x1Fu << 9));
    edm |= (4u << 14) | (4u << 9);             /* FIFO read / write thresholds */
    mmio_write(SDEDM, edm);
    delay_us(20000);
    mmio_write(SDVDD, 1);                      /* power on */
    delay_us(20000);
    hcfg = H_WIDE_INT | H_SLOW_CARD;
    mmio_write(SDHCFG, hcfg);
    set_clock(400000);                         /* identification at 400 kHz */
    delay_us(2000);                            /* >= 74 clocks */

    stage = "CMD0 go idle";
    cmd(0, 0, C_NORESP, &e);
    if (e) return failed(e);
    stage = "CMD8 interface condition";
    r = cmd(8, 0x1AA, 0, &e);
    if (e || (r & 0xFFF) != 0x1AA) return failed(e ? e : SD_NOCARD);

    stage = "ACMD41 power-up";
    u64 end = timer_us() + 2000000;
    do {
        r = acmd(41, 0x50FF8000, 0, &e);   /* no S18R (0.124) */
        if (!e && (r & 0x80000000)) break;
        delay_us(10000);
    } while (timer_us() < end);
    if (e || !(r & 0x80000000)) return failed(e ? e : SD_TIMEOUT);
    ccs = (r & 0x40000000) != 0;

    stage = "CMD2 card id";
    cmd(2, 0, C_LONG, &e);
    if (e) return failed(e);
    stage = "CMD3 card address";
    r = cmd(3, 0, 0, &e);
    if (e) return failed(e);
    rca = r & 0xFFFF0000;

    /* CSD (CMD9): the full 128 bits, RSP3 = bits 127..96 */
    stage = "CMD9 card size";
    cmd(9, rca, C_LONG, &e);
    if (e) return failed(e);
    {
        u32 r1 = mmio_read(SDRSP1), r2 = mmio_read(SDRSP2), r3 = mmio_read(SDRSP3);
        if ((r3 >> 30) == 1) {                 /* CSD v2: C_SIZE = bits 69..48 */
            u32 c_size = ((r2 & 0x3F) << 16) | (r1 >> 16);
            sectors = ((u64)c_size + 1) * 1024;
        } else {                               /* v1: READ_BL_LEN 83..80, C_SIZE 73..62, MULT 49..47 */
            u32 bl = (r2 >> 16) & 0xF, c_size = ((r2 & 0x3FF) << 2) | (r1 >> 30), mult = (r1 >> 15) & 7;
            sectors = ((u64)(c_size + 1) << (mult + 2)) << bl >> 9;
        }
    }

    stage = "CMD7 select card";
    cmd(7, rca, C_BUSY, &e);
    if (e) return failed(e);
    set_clock(25000000);                       /* default speed */

    /* SCR: 4-bit bus? */
    stage = "ACMD51 read SCR";
    u32 scr[2] = { 0, 0 };
    mmio_write(SDHBCT, 8);
    mmio_write(SDHBLC, 1);
    acmd(51, 0, C_READ, &e);
    if (e) return failed(e);
    if ((e = fifo(scr, 2, 1))) return failed(e);
    wait_data_idle(100);
    if (scr[0] & 0x400) {                      /* bus widths bits 51..48: 4-bit */
        stage = "ACMD6 4-bit bus";
        acmd(6, 2, 0, &e);
        if (!e) { hcfg |= H_WIDE_EXT; mmio_write(SDHCFG, hcfg); bus4 = 1; }
    }
    mmio_write(SDHBCT, 512);
    stage = "";
    ready = 1;
    return SD_OK;
}

int sdhost_read(u32 lba, void *buf, u32 count)
{
    u32 *p = buf;
    int e;
    if (!ready) return SD_NOCARD;
    for (u32 b = 0; b < count; b++) {
        mmio_write(SDHBCT, 512);
        mmio_write(SDHBLC, 1);
        cmd(17, ccs ? lba + b : (lba + b) * 512, C_READ, &e);
        if (e) return e;
        if ((e = fifo(p, 128, 1))) return e;
        p += 128;
        if ((e = wait_data_idle(500))) return e;
    }
    return SD_OK;
}

int sdhost_write(u32 lba, const void *buf, u32 count)
{
    const u32 *p = buf;
    int e;
    if (!ready) return SD_NOCARD;
    for (u32 b = 0; b < count; b++) {
        mmio_write(SDHBCT, 512);
        mmio_write(SDHBLC, 1);
        cmd(24, ccs ? lba + b : (lba + b) * 512, C_WRITE, &e);
        if (e) return e;
        if ((e = fifo((u32 *)p, 128, 0))) return e;
        p += 128;
        if ((e = wait_data_idle(1000))) return e;
        if ((e = wait_card_ready(1000))) return e;
    }
    return SD_OK;
}

u64  sdhost_sectors(void)   { return sectors; }
int  sdhost_is_sdhc(void)   { return ccs; }
int  sdhost_bus_width(void) { return bus4 ? 4 : 1; }

void sdhost_print_fail(int e)
{
    kprintf("%s at %s (SDHSTS=%x SDCMD=%x SDEDM=%x)\n", sd_error(e), stage, dbg_hsts, dbg_cmd, dbg_edm);
}
