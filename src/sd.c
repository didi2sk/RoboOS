/*
 * SD card driver for the Arasan SDHCI controller ("EMMC") in the BCM2837.
 *
 * The firmware routes the SD slot (GPIO48-53) to the SDHOST controller;
 * we switch the pins to ALT3 so the card is driven by the Arasan controller.
 * PIO mode (no DMA), 512-byte single-block reads and writes.
 * Based on the well-known bare-metal sequence (bztsrc raspi3-tutorial, SD spec).
 */
#include "kernel.h"
#include "sd.h"

#define EMMC        (MMIO_BASE + 0x300000)
#define ARG2        (EMMC + 0x00)
#define BLKSIZECNT  (EMMC + 0x04)
#define ARG1        (EMMC + 0x08)
#define CMDTM       (EMMC + 0x0C)
#define RESP0       (EMMC + 0x10)
#define RESP1       (EMMC + 0x14)
#define RESP2       (EMMC + 0x18)
#define RESP3       (EMMC + 0x1C)
#define DATA        (EMMC + 0x20)
#define STATUS      (EMMC + 0x24)
#define CONTROL0    (EMMC + 0x28)
#define CONTROL1    (EMMC + 0x2C)
#define INTERRUPT   (EMMC + 0x30)
#define INT_MASK    (EMMC + 0x34)
#define INT_EN      (EMMC + 0x38)
#define CONTROL2    (EMMC + 0x3C)
#define SLOTISR_VER (EMMC + 0xFC)

#define GPPUDCLK1   (MMIO_BASE + 0x20009C)

/* command words (index, response type, data flags) */
#define CMD_NEED_APP      0x80000000
#define CMD_RSPNS_48      0x00020000
#define CMD_ERRORS_MASK   0xfff9c004
#define CMD_RCA_MASK      0xffff0000
#define CMD_GO_IDLE       0x00000000
#define CMD_ALL_SEND_CID  0x02010000
#define CMD_SEND_REL_ADDR 0x03020000
#define CMD_CARD_SELECT   0x07030000
#define CMD_SEND_IF_COND  0x08020000
#define CMD_SEND_CSD      0x09010000
#define CMD_READ_SINGLE   0x11220010
#define CMD_WRITE_SINGLE  0x18220000
#define CMD_APP_CMD       0x37020000   /* R1 - always wait for the answer */
#define CMD_SET_BUS_WIDTH (0x06020000 | CMD_NEED_APP)
#define CMD_SEND_OP_COND  (0x29020000 | CMD_NEED_APP)
#define CMD_SEND_SCR      (0x33220010 | CMD_NEED_APP)

#define SR_READ_AVAILABLE 0x00000800
#define SR_DAT_INHIBIT    0x00000002
#define SR_CMD_INHIBIT    0x00000001
#define SR_APP_CMD        0x00000020

#define INT_DATA_TIMEOUT  0x00100000
#define INT_CMD_TIMEOUT   0x00010000
#define INT_READ_RDY      0x00000020
#define INT_WRITE_RDY     0x00000010
#define INT_DATA_DONE     0x00000002
#define INT_CMD_DONE      0x00000001
#define INT_ERROR_MASK    0x017E8000

#define C0_HCTL_DWIDTH    0x00000002
#define C1_SRST_HC        0x01000000
#define C1_SRST_CMD       0x02000000
#define C1_SRST_DATA      0x04000000
#define C1_TOUNIT_MAX     0x000e0000
#define C1_CLK_EN         0x00000004
#define C1_CLK_STABLE     0x00000002
#define C1_CLK_INTLEN     0x00000001

#define ACMD41_VOLTAGE      0x00ff8000
#define ACMD41_CMD_COMPLETE 0x80000000
#define ACMD41_CMD_CCS      0x40000000
#define ACMD41_ARG_HC       0x51ff8000

#define SCR_SD_BUS_WIDTH_4  0x00000400

static u32 rca, hc_ver, base_clk, ccs, bus4;
static u64 sectors;
static int ready;

/* diagnostics: which init step failed and what the controller said */
static const char *stage = "";
static u32 dbg_int, dbg_status, dbg_resp;
#define STAGE(s) (stage = (s))
static int failed(int e)
{
    dbg_status = mmio_read(STATUS);
    dbg_resp = mmio_read(RESP0);
    return e;
}

static int wait_status(u32 mask, u32 ms)
{
    u64 end = timer_us() + (u64)ms * 1000;
    while ((mmio_read(STATUS) & mask) && !(mmio_read(INTERRUPT) & INT_ERROR_MASK))
        if (timer_us() > end) return SD_TIMEOUT;
    return (mmio_read(INTERRUPT) & INT_ERROR_MASK) ? SD_ERROR : SD_OK;
}

/* after a failed command the CMD/DAT state machines stay busy until reset */
static void reset_lines(void)
{
    mmio_write(CONTROL1, mmio_read(CONTROL1) | C1_SRST_CMD | C1_SRST_DATA);
    u64 end = timer_us() + 100000;
    while ((mmio_read(CONTROL1) & (C1_SRST_CMD | C1_SRST_DATA)) && timer_us() < end) ;
}

static int wait_int(u32 mask, u32 ms)
{
    u32 m = mask | INT_ERROR_MASK, r;
    u64 end = timer_us() + (u64)ms * 1000;
    while (!(mmio_read(INTERRUPT) & m))
        if (timer_us() > end) { dbg_int = mmio_read(INTERRUPT); reset_lines(); return SD_TIMEOUT; }
    r = mmio_read(INTERRUPT);
    dbg_int = r;
    if (r & INT_ERROR_MASK) {
        mmio_write(INTERRUPT, r);
        reset_lines();
        return (r & (INT_CMD_TIMEOUT | INT_DATA_TIMEOUT)) ? SD_TIMEOUT : SD_ERROR;
    }
    mmio_write(INTERRUPT, mask);
    return SD_OK;
}

/* returns the response (or 0); *err gets SD_OK / error */
static u32 sd_cmd(u32 code, u32 arg, int *err)
{
    u32 r;
    *err = SD_OK;
    if (code & CMD_NEED_APP) {
        int e;
        /* CMD55 answers with R1 even before the card has an address. Not waiting
         * for that answer made the next command collide with it on real cards
         * (INT=30000: command timeout + CRC = CMD line conflict). */
        sd_cmd(CMD_APP_CMD, rca, &e);
        if (e) { *err = e; return 0; }
        code &= ~CMD_NEED_APP;
    }
    if (wait_status(SR_CMD_INHIBIT, 500)) { *err = SD_TIMEOUT; return 0; }
    mmio_write(INTERRUPT, mmio_read(INTERRUPT));
    mmio_write(ARG1, arg);
    mmio_write(CMDTM, code);
    if ((*err = wait_int(INT_CMD_DONE, 500))) return 0;
    r = mmio_read(RESP0);
    if (code == CMD_SEND_REL_ADDR) return r & CMD_RCA_MASK;
    return r;
}

static void set_clock(u32 hz)
{
    u64 end = timer_us() + 100000;
    while ((mmio_read(STATUS) & (SR_CMD_INHIBIT | SR_DAT_INHIBIT)) && timer_us() < end) ;
    mmio_write(CONTROL1, mmio_read(CONTROL1) & ~C1_CLK_EN);
    delay_us(10000);

    /* SDCLK = base / (2 * N) */
    u32 n = (base_clk + 2 * hz - 1) / (2 * hz);
    u32 d;
    if (hc_ver >= 2) {                       /* SDHCI 3.0: 10-bit divider */
        if (n > 0x3FF) n = 0x3FF;
        d = ((n & 0xFF) << 8) | ((n & 0x300) >> 2);
    } else {                                 /* SDHCI 2.0: power of two */
        u32 p = 1;
        while (p < n && p < 0x80) p <<= 1;
        d = (p & 0xFF) << 8;
    }
    mmio_write(CONTROL1, (mmio_read(CONTROL1) & 0xFFFF003F) | d);
    delay_us(10000);
    mmio_write(CONTROL1, mmio_read(CONTROL1) | C1_CLK_EN);
    end = timer_us() + 100000;
    while (!(mmio_read(CONTROL1) & C1_CLK_STABLE) && timer_us() < end) ;
}

static void pins_alt3(void)
{
    /* On boards with WiFi (3B, 3B+, 3A+, Zero 2 W) the firmware leaves GPIO34-39
     * on ALT3 too: the Arasan controller is wired to the WiFi chip there. Two pin
     * groups on one controller = it never sees the card's answers. Release them
     * (same workaround as in Circle). */
    for (int p = 34; p <= 39; p++) gpio_func(p, 0);
    /* GPIO47 = card detect (input), GPIO48-53 = CLK, CMD, DAT0-3 -> ALT3 */
    if (board_type() != 0x04) gpio_func(47, 0);          /* Pi 2: GPIO47 is the ACT LED */
    for (int p = 48; p <= 53; p++) gpio_func(p, 7);
    mmio_write(GPPUD, 2);                    /* pull-up */
    delay_cycles(150);
    mmio_write(GPPUDCLK1, (1 << 15) | (1 << 16) | (1 << 17) | (1 << 18) |
                          (1 << 19) | (1 << 20) | (1 << 21));
    delay_cycles(150);
    mmio_write(GPPUD, 0);
    mmio_write(GPPUDCLK1, 0);
}

int sd_init(void)
{
    int e;
    u32 r;
    ready = 0; rca = 0; ccs = 0; bus4 = 0; sectors = 0;

    STAGE("controller reset");
    dbg_int = dbg_status = dbg_resp = 0;
    pins_alt3();
    hc_ver = (mmio_read(SLOTISR_VER) >> 16) & 0xFF;

    u32 pwr[2] = { 0 /* SD card */, 3 /* on + wait */ };
    mbox_prop(0x00028001, pwr, 2);
    u32 clk[2] = { 1 /* EMMC */, 0 };
    base_clk = mbox_prop(0x00030002, clk, 2) && clk[1] ? clk[1] : 41666666;

    /* reset the host controller */
    mmio_write(CONTROL0, 0);
    mmio_write(CONTROL1, mmio_read(CONTROL1) | C1_SRST_HC);
    u64 end = timer_us() + 1000000;
    while (mmio_read(CONTROL1) & C1_SRST_HC)
        if (timer_us() > end) return failed(SD_TIMEOUT);
    mmio_write(CONTROL1, mmio_read(CONTROL1) | C1_CLK_INTLEN | C1_TOUNIT_MAX);
    delay_us(10000);

    set_clock(400000);                       /* identification at 400 kHz */
    mmio_write(INT_EN, 0xFFFFFFFF);
    mmio_write(INT_MASK, 0xFFFFFFFF);
    delay_us(2000);                          /* >= 74 clocks before the first command */

    STAGE("CMD0 go idle");
    sd_cmd(CMD_GO_IDLE, 0, &e);
    if (e) return failed(e);
    STAGE("CMD8 interface condition");
    r = sd_cmd(CMD_SEND_IF_COND, 0x1AA, &e);
    if (e || (r & 0xFFF) != 0x1AA) return failed(SD_NOCARD);   /* only SD 2.0+ cards */

    STAGE("ACMD41 power-up");
    end = timer_us() + 2000000;              /* cards may take over 1 s to power up */
    do {
        r = sd_cmd(CMD_SEND_OP_COND, ACMD41_ARG_HC, &e);
        if (!e && (r & ACMD41_CMD_COMPLETE)) break;
        delay_us(10000);
    } while (timer_us() < end);
    if (e || !(r & ACMD41_CMD_COMPLETE)) return failed(SD_TIMEOUT);
    if (!(r & ACMD41_VOLTAGE)) return failed(SD_ERROR);
    ccs = (r & ACMD41_CMD_CCS) != 0;          /* SDHC/SDXC: block addressing */

    STAGE("CMD2 card id");
    sd_cmd(CMD_ALL_SEND_CID, 0, &e);
    if (e) return failed(e);
    STAGE("CMD3 card address");
    rca = sd_cmd(CMD_SEND_REL_ADDR, 0, &e);
    if (e) return failed(e);

    /* card size from CSD (CMD9). The controller strips the CRC byte:
     * RESP3..RESP0 hold CSD bits [127:8] shifted down by 8. */
    sd_cmd(CMD_SEND_CSD, rca, &e);
    if (!e) {
        u32 r1 = mmio_read(RESP1), r2 = mmio_read(RESP2), r3 = mmio_read(RESP3);
        u32 structure = (r3 >> 22) & 3;
        if (structure == 1) {                /* CSD v2: C_SIZE bits [69:48] */
            u32 c_size = ((r1 >> 8) & 0x3FFFFF);
            sectors = ((u64)c_size + 1) * 1024;
        } else {                             /* CSD v1 */
            u32 read_bl_len = (r2 >> 8) & 0xF;
            u32 c_size = ((r2 & 0x3) << 10) | (r1 >> 22);
            u32 mult = (r1 >> 7) & 0x7;
            sectors = ((u64)(c_size + 1) << (mult + 2)) << read_bl_len >> 9;
        }
    }

    set_clock(25000000);                     /* default speed: 25 MHz */
    STAGE("CMD7 select card");
    sd_cmd(CMD_CARD_SELECT, rca, &e);
    if (e) return failed(e);

    /* SCR tells whether the card supports a 4-bit bus */
    STAGE("ACMD51 read SCR");
    if (wait_status(SR_DAT_INHIBIT, 500)) return failed(SD_TIMEOUT);
    mmio_write(BLKSIZECNT, (1 << 16) | 8);
    sd_cmd(CMD_SEND_SCR, 0, &e);
    if (e) return failed(e);
    if (wait_int(INT_READ_RDY, 500)) return failed(SD_TIMEOUT);
    u32 scr[2] = { 0, 0 };
    end = timer_us() + 500000;
    for (int i = 0; i < 2; ) {
        if (mmio_read(STATUS) & SR_READ_AVAILABLE) scr[i++] = mmio_read(DATA);
        else if (timer_us() > end) return failed(SD_TIMEOUT);
    }
    /* SCR is big-endian: bus widths are bits [51:48] -> byte 1 of word 0 */
    if (scr[0] & SCR_SD_BUS_WIDTH_4) {
        sd_cmd(CMD_SET_BUS_WIDTH, rca | 2, &e);
        if (!e) { mmio_write(CONTROL0, mmio_read(CONTROL0) | C0_HCTL_DWIDTH); bus4 = 1; }
    }
    STAGE("");
    ready = 1;
    return SD_OK;
}

int sd_read(u32 lba, void *buf, u32 count)
{
    u32 *p = buf;
    int e;
    if (!ready) return SD_NOCARD;
    for (u32 b = 0; b < count; b++) {
        if (wait_status(SR_DAT_INHIBIT, 500)) return SD_TIMEOUT;
        mmio_write(BLKSIZECNT, (1 << 16) | 512);
        sd_cmd(CMD_READ_SINGLE, ccs ? lba + b : (lba + b) * 512, &e);
        if (e) return e;
        if ((e = wait_int(INT_READ_RDY, 500))) return e;
        for (int i = 0; i < 128; i++) *p++ = mmio_read(DATA);
    }
    return SD_OK;
}

int sd_write(u32 lba, const void *buf, u32 count)
{
    const u32 *p = buf;
    int e;
    if (!ready) return SD_NOCARD;
    for (u32 b = 0; b < count; b++) {
        if (wait_status(SR_DAT_INHIBIT, 1000)) return SD_TIMEOUT;   /* card may still be programming */
        mmio_write(BLKSIZECNT, (1 << 16) | 512);
        sd_cmd(CMD_WRITE_SINGLE, ccs ? lba + b : (lba + b) * 512, &e);
        if (e) return e;
        if ((e = wait_int(INT_WRITE_RDY, 500))) return e;
        for (int i = 0; i < 128; i++) mmio_write(DATA, *p++);
        if ((e = wait_int(INT_DATA_DONE, 1000))) return e;
    }
    return SD_OK;
}

int  sd_ready(void)      { return ready; }
u64  sd_sectors(void)    { return sectors; }
int  sd_is_sdhc(void)    { return ccs; }
int  sd_bus_width(void)  { return bus4 ? 4 : 1; }

/* one line for the screen after a failed sd_init() */
void sd_print_fail(int e)
{
    kprintf("%s at %s (INT=%x STATUS=%x RESP=%x)\n", sd_error(e), stage, dbg_int, dbg_status, dbg_resp);
}

const char *sd_error(int e)
{
    switch (e) {
    case SD_OK:      return "ok";
    case SD_TIMEOUT: return "timeout";
    case SD_NOCARD:  return "no card / unsupported card";
    default:         return "error";
    }
}
