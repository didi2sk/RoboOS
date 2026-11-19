/*
 * WiFi: Broadcom / Cypress CYW43438 (Pi 3B, chip id 43430), CYW43436 (Zero 2 W,
 * 43430 rev 2) and CYW43455 (Pi 3B+, 0x4345) over SDIO on the Arasan (EMMC)
 * controller, GPIO34-39 ALT3. The SD card is on SDHOST (sdhost.c).
 *
 * Ported from the Plan 9 / 9front driver ether4330.c by Richard Miller
 * (MIT license, /lib/legal/mit in 9front): SDIO setup, Silicon Backplane core
 * scan, firmware + NVRAM upload, SDPCM packets, firmware commands, escan.
 * Polled, core 0 for now (the firmware files are read from the SD card).
 *
 * Firmware files (from Raspberry Pi OS /lib/firmware/brcm, or linux-firmware)
 * go to /FIRMWARE on the card:
 *   Pi 3B:     brcmfmac43430-sdio.bin  brcmfmac43430-sdio.txt
 *   Zero 2 W:  brcmfmac43436-sdio.bin  brcmfmac43436-sdio.txt  brcmfmac43436-sdio.clm_blob
 *   Pi 3B+:    brcmfmac43455-sdio.bin  brcmfmac43455-sdio.txt  brcmfmac43455-sdio.clm_blob
 */
#include "kernel.h"
#include "wpa.h"
#include "sd.h"

/* ---------------- Arasan as an SDIO host ---------------- */

#define EMMC        (MMIO_BASE + 0x300000)
#define BLKSIZECNT  (EMMC + 0x04)
#define ARG1        (EMMC + 0x08)
#define CMDTM       (EMMC + 0x0C)
#define RESP0       (EMMC + 0x10)
#define DATA        (EMMC + 0x20)
#define STATUS      (EMMC + 0x24)
#define CONTROL0    (EMMC + 0x28)
#define CONTROL1    (EMMC + 0x2C)
#define INTERRUPT   (EMMC + 0x30)
#define INT_MASK    (EMMC + 0x34)
#define INT_EN      (EMMC + 0x38)
#define SLOTISR_VER (EMMC + 0xFC)
#define GPPUDCLK1   (MMIO_BASE + 0x20009C)

#define SR_CMD_INHIBIT 0x1
#define SR_DAT_INHIBIT 0x2
#define I_CMD_DONE     0x1
#define I_DATA_DONE    0x2
#define I_WRITE_RDY    0x10
#define I_READ_RDY     0x20
#define I_ERR          0x017E8000
#define I_CMD_TIMEOUT  0x10000
#define C1_SRST_HC     0x01000000
#define C1_SRST_CMD    0x02000000
#define C1_SRST_DATA   0x04000000
#define C1_CLK_EN      0x4
#define C1_CLK_STABLE  0x2
#define C1_CLK_INTLEN  0x1
#define C1_TOUNIT_MAX  0x000e0000

/* CMDTM: index << 24 | response | flags */
#define T_NONE   0x00000000
#define T_R48    0x00020000
#define T_R48B   0x00030000
#define T_CRC    0x00080000
#define T_IDX    0x00100000
#define T_DATA   0x00200000
#define T_READ   0x00000010
#define T_MULTI  0x00000022              /* multi block + block count enable */

static u32 base_clk, hc_ver;
static const char *werr = "";
static u32 dbg_int, dbg_resp;

static int reset_lines(void)
{
    mmio_write(CONTROL1, mmio_read(CONTROL1) | C1_SRST_CMD | C1_SRST_DATA);
    u64 end = timer_us() + 100000;
    while ((mmio_read(CONTROL1) & (C1_SRST_CMD | C1_SRST_DATA)) && timer_us() < end) ;
    return -1;
}

static int wait_int(u32 mask, u32 ms)
{
    u64 end = timer_us() + (u64)ms * 1000;
    u32 r;
    while (!((r = mmio_read(INTERRUPT)) & (mask | I_ERR)))
        if (timer_us() > end) { dbg_int = r; reset_lines(); return -1; }
    dbg_int = r;
    if (r & I_ERR) { mmio_write(INTERRUPT, r); reset_lines(); return -2; }
    mmio_write(INTERRUPT, mask);
    return 0;
}

/* a command; 0 = ok, the response in *resp */
static int scmd(u32 tm, u32 arg, u32 *resp)
{
    u64 end = timer_us() + 100000;
    u32 inh = SR_CMD_INHIBIT | ((tm & T_DATA) ? SR_DAT_INHIBIT : 0);
    while (mmio_read(STATUS) & inh)
        if (timer_us() > end) { reset_lines(); break; }
    mmio_write(INTERRUPT, mmio_read(INTERRUPT));
    mmio_write(ARG1, arg);
    mmio_write(CMDTM, tm);
    int e = wait_int(I_CMD_DONE, 200);
    dbg_resp = mmio_read(RESP0);
    if (resp) *resp = dbg_resp;
    return e;
}

static void set_clock(u32 hz)
{
    u64 end = timer_us() + 100000;
    while ((mmio_read(STATUS) & (SR_CMD_INHIBIT | SR_DAT_INHIBIT)) && timer_us() < end) ;
    mmio_write(CONTROL1, mmio_read(CONTROL1) & ~C1_CLK_EN);
    delay_us(2000);
    u32 n = (base_clk + 2 * hz - 1) / (2 * hz), d;
    if (hc_ver >= 2) { if (n > 0x3FF) n = 0x3FF; d = ((n & 0xFF) << 8) | ((n & 0x300) >> 2); }
    else { u32 p = 1; while (p < n && p < 0x80) p <<= 1; d = (p & 0xFF) << 8; }
    mmio_write(CONTROL1, (mmio_read(CONTROL1) & 0xFFFF003F) | d);
    delay_us(2000);
    mmio_write(CONTROL1, mmio_read(CONTROL1) | C1_CLK_EN);
    end = timer_us() + 100000;
    while (!(mmio_read(CONTROL1) & C1_CLK_STABLE) && timer_us() < end) ;
}

/* ---------------- SDIO commands ---------------- */

enum { Fn0, Fn1, Fn2 };
#define Ioenable  0x02
#define Ioready   0x03
#define Intenable 0x04
#define Intpend   0x05
#define Ioabort   0x06
#define Busifc    0x07
#define Fbr1      0x100
#define Fbr2      0x200
#define Blksize   0x10

/* CMD52: one byte; returns the byte or < 0 */
static int sdiord(int fn, u32 addr)
{
    u32 r;
    if (scmd((52u << 24) | T_R48 | T_CRC | T_IDX, ((u32)(fn & 7) << 28) | ((addr & 0x1FFFF) << 9), &r)) return -1;
    if (r & 0xCF00) return -2;
    return (int)(r & 0xFF);
}

static int sdiowr(int fn, u32 addr, int data)
{
    u32 r = 0;
    for (int retry = 0; retry < 10; retry++) {
        if (!scmd((52u << 24) | T_R48 | T_CRC | T_IDX,
                  (1u << 31) | ((u32)(fn & 7) << 28) | ((addr & 0x1FFFF) << 9) | (u32)(data & 0xFF), &r) && !(r & 0xCF00))
            return 0;
    }
    return -1;
}

static int sdioset(int fn, u32 addr, int bits)
{
    int v = sdiord(fn, addr);
    return v < 0 ? v : sdiowr(fn, addr, v | bits);
}

/* CMD53: len bytes; Fn1 blocks of 64, Fn2 blocks of 512; incr = address grows */
static u8 xbuf[2048] __attribute__((aligned(4)));
static int sdiorwext(int fn, int write, u8 *a, int len, u32 addr, int incr)
{
    int bsize = fn == Fn2 ? 512 : 64;
    while (len > 0) {
        int blk, bcount, m;
        if (len > bsize) { blk = 1; bcount = len / bsize; if (bcount > 511) bcount = 511; m = bcount * bsize; }
        else { blk = 0; bcount = len; m = len; }
        if (m > (int)sizeof(xbuf)) { blk = 1; bcount = (int)sizeof(xbuf) / bsize; m = bcount * bsize; }
        int words = (m + 3) / 4;
        if (write) memcpy(xbuf, a, (size_t)m);
        mmio_write(BLKSIZECNT, blk ? ((u32)bcount << 16) | (u32)bsize : (1u << 16) | (u32)bcount);
        u32 tm = (53u << 24) | T_R48 | T_CRC | T_IDX | T_DATA | (write ? 0 : T_READ) | (blk ? T_MULTI : 0);
        u32 arg = ((u32)write << 31) | ((u32)(fn & 7) << 28) | ((u32)blk << 27) | ((u32)incr << 26) |
                  ((addr & 0x1FFFF) << 9) | ((u32)bcount & 0x1FF);
        if (scmd(tm, arg, 0)) { werr = "CMD53"; return -1; }
        /* the data, block by block (the FIFO raises READ_RDY / WRITE_RDY per block) */
        int per = blk ? bsize / 4 : words, blocks = blk ? bcount : 1, w = 0;
        u32 *x = (u32 *)xbuf;
        for (int b = 0; b < blocks; b++) {
            if (wait_int(write ? I_WRITE_RDY : I_READ_RDY, 500)) { werr = "CMD53 data"; return -1; }
            for (int i = 0; i < per && w < words; i++, w++) {
                if (write) mmio_write(DATA, x[w]);
                else x[w] = mmio_read(DATA);
            }
        }
        if (wait_int(I_DATA_DONE, 500)) { werr = "CMD53 done"; return -1; }
        if (!write) memcpy(a, xbuf, (size_t)m);
        len -= m;
        a += m;
        if (incr) addr += (u32)m;
    }
    return 0;
}

/* ---------------- the chip ---------------- */

#define Sbwsize   0x8000
#define Sb32bit   0x8000
#define Sbaddr    0x1000a
#define Enumbase  0x18000000
#define Framectl  0x1000d
#define Clkcsr    0x1000e
#define Pullups   0x1000f
#define Rfrmcnt   0x1001b
#define ForceALP  0x01
#define ForceHT   0x02
#define ReqALP    0x08
#define ReqHT     0x10
#define Nohwreq   0x20
#define ALPavail  0x40
#define HTavail   0x80
#define Ioctrl    0x408
#define Resetctrl 0x800
#define Coreinfo  0x00
#define Bankidx   0x10
#define Bankinfo  0x40
#define Bankpda   0x44
#define Cr4Cap    0x04
#define Cr4Bankidx 0x40
#define Cr4Bankinfo 0x44
#define Cr4Cpuhalt 0x20
#define Gpiopullup 0x58
#define Gpiopulldown 0x5c
#define Intstatus 0x20
#define Intmask   0x24
#define Sbmbox    0x40
#define Sbmboxdata 0x48
#define Hostmboxdata 0x4c
#define FrameInt  (1 << 6)
#define MailboxInt (1 << 7)
#define Fcchange  (1 << 5)
#define ARMcm3    0x82A
#define ARM7tdmi  0x825
#define ARMcr4    0x83E

static struct {
    int chipid, chiprev, armcore, up;
    u32 chipcommon, armctl, armregs, d11ctl, socramregs, socramctl, sdregs;
    int socramrev;
    u32 socramsize, rambase, resetvec;
    u16 reqid;
    u8 txseq, txwindow, fcmask;
    u8 mac[6];
    char ver[96];
} w;

static u32 cur_window = 0xFFFFFFFF;
static int sbwindow(u32 addr)
{
    addr &= ~(u32)(Sbwsize - 1);
    if (addr == cur_window) return 0;
    if (sdiowr(Fn1, Sbaddr, (int)(addr >> 8)) || sdiowr(Fn1, Sbaddr + 1, (int)(addr >> 16)) ||
        sdiowr(Fn1, Sbaddr + 2, (int)(addr >> 24))) { cur_window = 0xFFFFFFFF; return -1; }
    cur_window = addr;
    return 0;
}

static u32 cfgreadl(u32 off)
{
    u8 b[4] = { 0, 0, 0, 0 };
    sbwindow(off);
    sdiorwext(Fn1, 0, b, 4, (off & (Sbwsize - 1)) | Sb32bit, 1);
    return (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
}

static void cfgwritel(u32 off, u32 v)
{
    u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
    sbwindow(off);
    for (int retry = 0; retry < 3; retry++) {
        if (!sdiorwext(Fn1, 1, b, 4, (off & (Sbwsize - 1)) | Sb32bit, 1)) return;
        sdiowr(Fn0, Ioabort, Fn1);
    }
}

/* chip memory through the 32 KB backplane window */
static int sbmem(int write, u8 *buf, int len, u32 off)
{
    u32 n = ((off + Sbwsize) & ~(u32)(Sbwsize - 1)) - off;
    while (len > 0) {
        if ((int)n > len) n = (u32)len;
        if (sbwindow(off)) return -1;
        u32 o = off & (Sbwsize - 1);
        int k = (int)n & ~3;
        if (k && sdiorwext(Fn1, write, buf, k, o | Sb32bit, 1)) return -1;
        for (int i = k; i < (int)n; i++) {                  /* the odd bytes one by one */
            if (write) { if (sdiowr(Fn1, (o + (u32)i) | Sb32bit, buf[i])) return -1; }
            else { int v = sdiord(Fn1, (o + (u32)i) | Sb32bit); if (v < 0) return -1; buf[i] = (u8)v; }
        }
        off += n; buf += n; len -= (int)n;
        n = Sbwsize;
    }
    return 0;
}

static void sbdisable(u32 regs, u32 pre, u32 ioctl)
{
    if (cfgreadl(regs + Resetctrl) & 1) {
        cfgwritel(regs + Ioctrl, 3 | ioctl);
        cfgreadl(regs + Ioctrl);
        return;
    }
    cfgwritel(regs + Ioctrl, 3 | pre);
    cfgreadl(regs + Ioctrl);
    cfgwritel(regs + Resetctrl, 1);
    delay_us(10);
    u64 end = timer_us() + 100000;
    while (!(cfgreadl(regs + Resetctrl) & 1) && timer_us() < end) ;
    cfgwritel(regs + Ioctrl, 3 | ioctl);
    cfgreadl(regs + Ioctrl);
}

static void sbreset(u32 regs, u32 pre, u32 ioctl)
{
    sbdisable(regs, pre, ioctl);
    u64 end = timer_us() + 100000;
    while ((cfgreadl(regs + Resetctrl) & 1) && timer_us() < end) {
        cfgwritel(regs + Resetctrl, 0);
        delay_us(40);
    }
    cfgwritel(regs + Ioctrl, 1 | ioctl);
    cfgreadl(regs + Ioctrl);
}

static void corescan(u32 r)
{
    static u8 buf[512];
    int coreid = 0, corerev = 0;
    sbmem(0, buf, sizeof(buf), r);
    for (int i = 0; i < (int)sizeof(buf); i += 4) {
        switch (buf[i] & 0xF) {
        case 0xF: return;
        case 0x1:
            if ((buf[i + 4] & 0xF) != 0x1) break;
            coreid = (buf[i + 1] | buf[i + 2] << 8) & 0xFFF;
            i += 4;
            corerev = buf[i + 3];
            break;
        case 0x5: {
            u32 addr = ((u32)buf[i + 1] << 8 | (u32)buf[i + 2] << 16 | (u32)buf[i + 3] << 24) & ~0xFFFu;
            int ctl = buf[i] & 0xC0;
            switch (coreid) {
            case 0x800: if (!ctl) w.chipcommon = addr; break;
            case ARMcm3: case ARM7tdmi: case ARMcr4:
                w.armcore = coreid;
                if (ctl) { if (!w.armctl) w.armctl = addr; } else if (!w.armregs) w.armregs = addr;
                break;
            case 0x80E:
                if (ctl) w.socramctl = addr; else if (!w.socramregs) w.socramregs = addr;
                w.socramrev = corerev;
                break;
            case 0x829: if (!ctl) w.sdregs = addr; break;
            case 0x812: if (ctl) w.d11ctl = addr; break;
            }
            break;
        }
        }
    }
}

static int ramscan(void)
{
    u32 n, size = 0;
    int banks;
    if (w.armcore == ARMcr4) {
        n = cfgreadl(w.armregs + Cr4Cap);
        banks = (int)(((n >> 4) & 0xF) + (n & 0xF));
        for (int i = 0; i < banks; i++) {
            cfgwritel(w.armregs + Cr4Bankidx, (u32)i);
            size += 8192 * ((cfgreadl(w.armregs + Cr4Bankinfo) & 0x3F) + 1);
        }
        w.socramsize = size;
        w.rambase = 0x198000;
        return 0;
    }
    if (w.socramrev <= 7 || w.socramrev == 12) { werr = "SOCRAM revision not supported"; return -1; }
    sbreset(w.socramctl, 0, 0);
    n = cfgreadl(w.socramregs + Coreinfo);
    banks = (int)((n >> 4) & 0xF);
    for (int i = 0; i < banks; i++) {
        cfgwritel(w.socramregs + Bankidx, (u32)i);
        size += 8192 * ((cfgreadl(w.socramregs + Bankinfo) & 0x3F) + 1);
    }
    w.socramsize = size;
    w.rambase = 0;
    if (w.chipid == 43430) {
        cfgwritel(w.socramregs + Bankidx, 3);
        cfgwritel(w.socramregs + Bankpda, 0);
    }
    return 0;
}

/* ---------------- power, SDIO bus, backplane ---------------- */

static void chip_power(void)
{
    if (board_type() == 0x12) {                /* Zero 2 W: WL_ON = GPIO41 */
        gpio_func(41, 1);
        gpio_set(41, 0); delay_us(20000);
        gpio_set(41, 1);
    } else {                                   /* 3B / 3B+: WL_ON = firmware GPIO expander pin 1 */
        u32 v[2] = { 129, 0 };
        mbox_prop(0x00038041, v, 2);
        delay_us(20000);
        v[0] = 129; v[1] = 1;
        mbox_prop(0x00038041, v, 2);
    }
    delay_us(150000);
}

static void pins_wifi(void)
{
    for (int p = 34; p <= 39; p++) gpio_func(p, 7);       /* ALT3 = Arasan */
    /* 34 = CLK: no pull, 35-39 = CMD + DAT: pull-up */
    mmio_write(GPPUD, 0); delay_cycles(150);
    mmio_write(GPPUDCLK1, 1u << (34 - 32)); delay_cycles(150);
    mmio_write(GPPUD, 0); mmio_write(GPPUDCLK1, 0);
    mmio_write(GPPUD, 2); delay_cycles(150);
    mmio_write(GPPUDCLK1, 0x3Eu << (34 - 32)); delay_cycles(150);
    mmio_write(GPPUD, 0); mmio_write(GPPUDCLK1, 0);
}

static int stage(const char *s) { werr = s; return -1; }

static int sdio_init(void)
{
    u32 r;
    if (!strcmp(sd_controller(), "Arasan (EMMC)")) return stage("the SD card uses the Arasan (SDHOST failed) - no WiFi");
    chip_power();
    pins_wifi();
    hc_ver = (mmio_read(SLOTISR_VER) >> 16) & 0xFF;
    u32 clk[2] = { 1, 0 };
    base_clk = mbox_prop(0x00030002, clk, 2) && clk[1] ? clk[1] : 41666666;
    mmio_write(CONTROL0, 0);
    mmio_write(CONTROL1, mmio_read(CONTROL1) | C1_SRST_HC);
    u64 end = timer_us() + 1000000;
    while (mmio_read(CONTROL1) & C1_SRST_HC) if (timer_us() > end) return stage("Arasan reset");
    mmio_write(CONTROL1, mmio_read(CONTROL1) | C1_CLK_INTLEN | C1_TOUNIT_MAX);
    set_clock(400000);
    mmio_write(INT_EN, 0xFFFFFFFF);
    mmio_write(INT_MASK, 0xFFFFFFFF);
    delay_us(2000);

    scmd(T_NONE, 0, 0);                                   /* CMD0 */
    r = 0;
    scmd((5u << 24) | T_R48, 0, &r);                      /* CMD5: OCR */
    for (int i = 0; !(r & 0x80000000u); i++) {
        if (i > 10) { werr = "CMD5: the WiFi chip does not answer"; return -1; }
        scmd((5u << 24) | T_R48, 3u << 20, &r);           /* 3.2-3.4 V */
        delay_us(100000);
    }
    if (scmd((3u << 24) | T_R48 | T_CRC | T_IDX, 0, &r)) return stage("CMD3 address");
    u32 rca = r & 0xFFFF0000;
    if (scmd((7u << 24) | T_R48B | T_CRC | T_IDX, rca, &r)) return stage("CMD7 select");
    if (sdioset(Fn0, Busifc, 2)) return stage("4-bit bus");
    mmio_write(CONTROL0, mmio_read(CONTROL0) | 0x2);      /* host 4-bit */
    set_clock(25000000);
    if (sdiowr(Fn0, Fbr1 + Blksize, 64) || sdiowr(Fn0, Fbr1 + Blksize + 1, 0) ||
        sdiowr(Fn0, Fbr2 + Blksize, 512 & 0xFF) || sdiowr(Fn0, Fbr2 + Blksize + 1, 512 >> 8)) return stage("block sizes");
    if (sdioset(Fn0, Ioenable, 1 << Fn1)) return stage("enable function 1");
    sdiowr(Fn0, Intenable, 0);
    for (int i = 0; !(sdiord(Fn0, Ioready) & (1 << Fn1)); i++) {
        if (i == 10) return stage("function 1 not ready");
        delay_us(100000);
    }
    return 0;
}

static int sb_init(void)
{
    cur_window = 0xFFFFFFFF;
    sbwindow(Enumbase);
    u32 r = cfgreadl(Enumbase);
    w.chipid = (int)(r & 0xFFFF);
    w.chiprev = (int)((r >> 16) & 0xF);
    if (w.chipid != 43430 && w.chipid != 0x4345 && w.chipid != 0x4330 && w.chipid != 43362) return stage("unknown chip id");
    corescan(cfgreadl(Enumbase + 63 * 4));
    if (!w.armctl || !w.d11ctl || (w.armcore == ARMcm3 && (!w.socramctl || !w.socramregs))) return stage("core scan");
    if (w.armcore == ARMcr4) sbreset(w.armctl, Cr4Cpuhalt, Cr4Cpuhalt);
    else sbdisable(w.armctl, 0, 0);
    sbreset(w.d11ctl, 8 | 4, 4);
    if (ramscan()) return -1;
    sdiowr(Fn1, Clkcsr, 0);
    delay_us(10);
    sdiowr(Fn1, Clkcsr, Nohwreq | ReqALP);
    u64 end = timer_us() + 100000;
    while (!(sdiord(Fn1, Clkcsr) & (HTavail | ALPavail))) if (timer_us() > end) return stage("ALP clock");
    sdiowr(Fn1, Clkcsr, Nohwreq | ForceALP);
    delay_us(65);
    sdiowr(Fn1, Pullups, 0);
    cfgwritel(w.chipcommon + Gpiopullup, 0);
    cfgwritel(w.chipcommon + Gpiopulldown, 0);
    return 0;
}

/* ---------------- firmware ---------------- */

static const struct { int chipid, rev; const char *fw, *nv, *clm; } fwtab[] = {
    { 43430, 1, "brcmfmac43430-sdio.bin", "brcmfmac43430-sdio.txt", "brcmfmac43430-sdio.clm_blob" },
    { 43430, 2, "brcmfmac43436-sdio.bin", "brcmfmac43436-sdio.txt", "brcmfmac43436-sdio.clm_blob" },
    { 0x4345, 6, "brcmfmac43455-sdio.bin", "brcmfmac43455-sdio.txt", "brcmfmac43455-sdio.clm_blob" },
    { 0x4345, 9, "brcmfmac43456-sdio.bin", "brcmfmac43456-sdio.txt", "brcmfmac43456-sdio.clm_blob" },
};
static int fwi = -1;
static char missing[64];

static int fw_open(const char *name, struct fat_file *f)
{
    char p[80] = "/FIRMWARE/";
    int n = (int)strlen(p);
    for (int i = 0; name[i] && n < 79; i++) p[n++] = name[i];
    p[n] = 0;
    if (fat_open(p, f) || f->is_dir) {
        int k = 0;
        for (const char *s = p; *s && k < 63; s++) missing[k++] = *s;
        missing[k] = 0;
        return -1;
    }
    return 0;
}

/* the NVRAM text -> "var=value\0...\0\0", padded to 4 bytes */
static int condense(u8 *buf, int n)
{
    u8 *op = buf, *lp = buf;
    int skip = 0;
    for (int i = 0; i < n; i++) {
        u8 c = buf[i];
        if (c == '#') skip = 1;
        else if (c == '\n' || c == 0) { skip = 0; if (op != lp) { *op++ = 0; lp = op; } }
        else if (c != '\r' && !skip) *op++ = c;
    }
    if (!skip && op != lp) *op++ = 0;
    *op++ = 0;
    while ((op - buf) & 3) *op++ = 0;
    return (int)(op - buf);
}

static int fw_load(void)
{
    struct fat_file f;
    static u8 buf[2048] __attribute__((aligned(4))), cmp[2048] __attribute__((aligned(4)));
    fwi = -1;
    for (int i = 0; i < (int)(sizeof(fwtab) / sizeof(fwtab[0])); i++)
        if (fwtab[i].chipid == w.chipid && fwtab[i].rev == w.chiprev) fwi = i;
    if (fwi < 0) return stage("no firmware known for this chip revision");
    if (fw_open(fwtab[fwi].fw, &f)) return stage("firmware file missing");
    sdiowr(Fn1, Clkcsr, ReqALP);
    u64 end = timer_us() + 100000;
    while (!(sdiord(Fn1, Clkcsr) & ALPavail)) if (timer_us() > end) return stage("ALP clock");
    u8 z[4] = { 0, 0, 0, 0 };
    sbmem(1, z, 4, w.rambase + w.socramsize - 4);
    /* the firmware, then read it back */
    u32 off = 0;
    int n;
    while ((n = fat_read(&f, buf, sizeof(buf))) > 0) {
        if (off == 0) w.resetvec = (u32)buf[0] | (u32)buf[1] << 8 | (u32)buf[2] << 16 | (u32)buf[3] << 24;
        while (n & 3) buf[n++] = 0;
        if (sbmem(1, buf, n, w.rambase + off)) return stage("firmware upload");
        off += (u32)n;
    }
    u32 total = off;
    f.pos = 0; f.cur_cluster = 0; f.cur_index = 0;
    for (off = 0; off < total; off += (u32)n) {
        n = fat_read(&f, buf, sizeof(buf));
        if (n <= 0) break;
        while (n & 3) buf[n++] = 0;
        if (sbmem(0, cmp, n, w.rambase + off) || memcmp_n(buf, cmp, (size_t)n)) return stage("firmware check (read back differs)");
    }
    /* NVRAM at the end of RAM, then its length token */
    if (fw_open(fwtab[fwi].nv, &f)) return stage("NVRAM file missing");
    static u8 nv[4096] __attribute__((aligned(4)));
    n = fat_read(&f, nv, sizeof(nv) - 8);
    if (n <= 0) return stage("NVRAM file empty");
    n = condense(nv, n);
    if (sbmem(1, nv, n, w.rambase + w.socramsize - (u32)n - 4)) return stage("NVRAM upload");
    u32 tok = (u32)n / 4;
    tok = (tok & 0xFFFF) | (~tok << 16);
    u8 t[4] = { (u8)tok, (u8)(tok >> 8), (u8)(tok >> 16), (u8)(tok >> 24) };
    sbmem(1, t, 4, w.rambase + w.socramsize - 4);
    kprintf("  firmware %u bytes + NVRAM %d bytes loaded\n", total, n);
    if (w.armcore == ARMcr4) {
        cfgwritel(w.sdregs + Intstatus, 0xFFFFFFFF);
        if (w.resetvec) { u8 rv[4] = { (u8)w.resetvec, (u8)(w.resetvec >> 8), (u8)(w.resetvec >> 16), (u8)(w.resetvec >> 24) }; sbmem(1, rv, 4, 0); }
        sbreset(w.armctl, Cr4Cpuhalt, 0);
    } else sbreset(w.armctl, 0, 0);
    return 0;
}

static int sb_enable(void)
{
    sdiowr(Fn1, Clkcsr, 0);
    delay_us(1000);
    sdiowr(Fn1, Clkcsr, ReqHT);
    for (int i = 0; !(sdiord(Fn1, Clkcsr) & HTavail); i++) {
        if (i == 50) return stage("HT clock (the firmware did not start)");
        delay_us(100000);
    }
    sdiowr(Fn1, Clkcsr, sdiord(Fn1, Clkcsr) | ForceHT);
    delay_us(10000);
    cfgwritel(w.sdregs + Sbmboxdata, 4 << 16);            /* protocol version */
    cfgwritel(w.sdregs + Intmask, FrameInt | MailboxInt | Fcchange);
    sdioset(Fn0, Ioenable, 1 << Fn2);
    for (int i = 0; !(sdiord(Fn0, Ioready) & (1 << Fn2)); i++) {
        if (i == 30) return stage("function 2 not ready");
        delay_us(100000);
    }
    sdiowr(Fn0, Intenable, (1 << Fn1) | (1 << Fn2) | 1);
    return 0;
}

/* ---------------- SDPCM packets ---------------- */

#define Sdpcmsz 12
#define Cmdsz   16
static u8 rx[2048] __attribute__((aligned(4)));

static int packetrw(int write, u8 *buf, int len)
{
    while (len > 0) {
        int n = len > 2048 ? 2048 : (len + 3) & ~3;
        int ok = 0;
        for (int retry = 0; retry < 3 && !ok; retry++) {
            if (!sdiorwext(Fn2, write, buf, n, Enumbase, 0)) ok = 1;
            else sdiowr(Fn0, Ioabort, Fn2);
        }
        if (!ok) return -1;
        buf += n; len -= n;
    }
    return 0;
}

/* one packet into rx[]; its length, 0 = nothing waiting */
static int readpkt(void)
{
    if (packetrw(0, rx, Sdpcmsz)) return -1;
    int len = rx[0] | rx[1] << 8, lenck = rx[2] | rx[3] << 8;
    if (len == 0) return 0;
    if (lenck != (len ^ 0xFFFF) || len < Sdpcmsz || len > 2048) {
        sdiowr(Fn1, Framectl, 1);                         /* read frame halt */
        for (int i = 0; i < 100 && (sdiord(Fn1, Rfrmcnt + 1) > 0 || sdiord(Fn1, Rfrmcnt) > 0); i++) ;
        return -1;
    }
    if (len > Sdpcmsz && packetrw(0, rx + Sdpcmsz, len - Sdpcmsz)) return -1;
    w.txwindow = rx[9];
    w.fcmask = rx[8];
    return len;
}

/* something waiting? (interrupt pending on the chip) */
static int pending(void)
{
    int i = sdiord(Fn0, Intpend);
    if (i <= 0) return 0;
    u32 ints = cfgreadl(w.sdregs + Intstatus);
    cfgwritel(w.sdregs + Intstatus, ints);
    if (ints & MailboxInt) {
        cfgreadl(w.sdregs + Hostmboxdata);
        cfgwritel(w.sdregs + Sbmbox, 2);                  /* ack */
    }
    return (ints & FrameInt) != 0;
}

/* events: escan results end up here */
#define MAXBSS 32
static struct bss { u8 bssid[6]; char ssid[33]; int rssi, chan, sec, grp; } bsslist[MAXBSS];
static int nbss, scan_done;

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static void add_bss(const u8 *p, int len)
{
    if (len < 0x80) return;
    struct bss b;
    memcpy(b.bssid, p + 8, 6);
    int sl = p[18] > 32 ? 32 : p[18];
    memcpy(b.ssid, p + 19, (size_t)sl);
    b.ssid[sl] = 0;
    b.rssi = (short)le16(p + 78);
    b.chan = p[88];                                       /* ctl_ch; 0 = take it from the chanspec */
    if (!b.chan) {
        u16 cs = le16(p + 72);
        int c = cs & 0xFF, sb, bw;
        if (w.chipid == 0x4345) {                         /* d11ac chanspec: center channel + sideband */
            sb = (cs >> 8) & 7; bw = (cs >> 11) & 7;
            if (bw == 3) c = c - 2 + 4 * sb; else if (bw == 4) c = c - 6 + 4 * sb; else if (bw == 5) c = c - 14 + 4 * sb;
        } else {                                          /* d11n: 40 MHz has a lower / upper sideband */
            sb = (cs >> 8) & 3; bw = (cs >> 10) & 3;
            if (bw == 3) c = sb == 1 ? c - 2 : c + 2;
        }
        b.chan = c;
    }
    b.grp = 4;
    b.sec = (le16(p + 16) & 0x10) ? 1 : 0;               /* privacy bit: WEP or better */
    u32 ieoff = le32(p + 0x74), ielen = le32(p + 0x78);
    if (ielen && ieoff + ielen <= (u32)len) {
        const u8 *t = p + ieoff, *e = t + ielen;
        while (t + 2 <= e && t + 2 + t[1] <= e) {
            if (t[0] == 0x30) { b.sec = 3; if (t[1] >= 6) b.grp = t[7]; }   /* RSN = WPA2/WPA3; group cipher 2 TKIP, 4 CCMP */
            else if (t[0] == 0xDD && t[1] >= 4 && t[2] == 0 && t[3] == 0x50 && t[4] == 0xF2 && t[5] == 1 && b.sec < 2) b.sec = 2;
            t += 2 + t[1];
        }
    }
    for (int i = 0; i < nbss; i++)
        if (!memcmp_n(bsslist[i].bssid, b.bssid, 6)) { if (b.rssi > bsslist[i].rssi) bsslist[i].rssi = b.rssi; return; }
    if (nbss < MAXBSS) bsslist[nbss++] = b;
}

/* joining: set by events (on core 1 once WiFi is up) */
static volatile int j_ssid_ok, j_link, j_keyed, j_fail, j_need_key, j_told;
static char j_ssid[33];
static int cfg_band;                                      /* 0 auto, 1 = 5 GHz only, 2 = 2.4 GHz only (WLC_SET_BAND) */

/* our own WPA2 supplicant, for firmware without one (Pi 3B+): see wpa.c */
static struct wpa sup;
static int host_sup, sup_ie_wanted, sup_keys, sup_gtk_only, sup_fails, sup_started;
/* diagnostics for photos: key frames in / handled, frames out, where our RSN element came from, why the link went */
static int d_eapol, d_m1, d_m3, d_grp, d_sent, d_sendfail, d_ie_fw, d_ie_len, d_keys, d_wpaie;
static volatile int d_lost_type, d_lost_reason;
static u64 sup_key_at;
static const char *sup_err;
static u8 sup_pmk[32];
void wpa_random(unsigned char *out, int n)
{
    rng_bytes(out, n);
    u64 t = timer_us();                                   /* mixed in, in case there is no hardware RNG */
    for (int i = 0; i < n; i++) out[i] ^= (u8)(t >> (8 * (i & 7))) ^ (u8)(i * 151);
}

static void event(const u8 *p, int len)
{
    if (len < 14 + 10 + 48) return;
    p += 14 + 10; len -= 14 + 10;
    u32 type = be32(p + 4), status = be32(p + 8), reason = be32(p + 12);
    u32 flags = (u32)p[2] << 8 | p[3];
    switch (type) {
    case 0:                                               /* E_SET_SSID: the join finished */
        if (status == 0) j_ssid_ok = 1; else j_fail = 1 + (int)status;   /* 1 fail, 2 timeout, 3 no network */
        return;
    case 16:                                              /* E_LINK */
        if (flags & 1) { if (!j_link && host_sup && !sup_started) { wpa_start(&sup, sup_pmk, w.mac, 0, 0); sup_ie_wanted = 1; sup_started = 1; } j_link = 1; }
        else { if (j_link) { d_lost_type = 16; d_lost_reason = (int)reason; } j_link = 0; j_keyed = 0; sup_started = 0; }
        return;
    case 46:                                              /* E_PSK_SUP: the firmware's WPA supplicant */
        if (status == 6) j_keyed = 1;                     /* keyed */
        else if (status == 7 || status == 0) { if (j_need_key && !j_keyed) j_fail = 100 + (int)status; }   /* timeout = wrong password? */
        return;
    case 5: case 6: case 11: case 12:                     /* deauth / disassoc (reason = 802.11 reason code) */
        d_lost_type = (int)type; d_lost_reason = (int)reason;
        j_link = 0; j_keyed = 0; sup_started = 0;
        return;
    }
    if (type == 69) {                                     /* E_ESCAN_RESULT */
        const u8 *r = p + 48;
        int rl = len - 48;
        if (rl < 12) return;
        if (status == 8 && le16(r + 10)) add_bss(r + 12, rl - 12);   /* partial: one network */
        else if (status != 8) scan_done = 1;              /* 0 = complete, else aborted */
    }
}

/* handle one received packet; returns its channel (0 = command answer) */
static int dispatch(int len, u16 *rsp_id)
{
    int ch = rx[5] & 0xF, doff = rx[7];
    if (ch == 0 && len >= Sdpcmsz + Cmdsz) { *rsp_id = le16(rx + doff + 10); return 0; }
    if (ch == 1 && len > doff + 4) {
        int bdc = 4 + (rx[doff + 3] << 2);
        if (len > doff + bdc) event(rx + doff + bdc, len - doff - bdc);
    }
    return ch;
}

static void poll_events(u32 ms)
{
    u64 end = timer_us() + (u64)ms * 1000;
    while (timer_us() < end) {
        if (!pending()) { delay_us(2000); continue; }
        int n;
        u16 id;
        while ((n = readpkt()) > 0) dispatch(n, &id);
    }
}

/* a firmware command (ioctl); write = data goes in, else the answer comes back into res */
static int wlcmd(int write, u32 op, const void *data, int dlen, void *res, int rlen)
{
    static u8 b[1600] __attribute__((aligned(4)));
    int tlen = write ? dlen + rlen : (dlen > rlen ? dlen : rlen);
    int len = Sdpcmsz + Cmdsz + tlen;
    if (len > (int)sizeof(b)) return stage("command too long");
    memset(b, 0, (size_t)len);
    b[0] = (u8)len; b[1] = (u8)(len >> 8); b[2] = (u8)~len; b[3] = (u8)(~len >> 8);
    b[4] = w.txseq;
    b[7] = Sdpcmsz;
    u8 *q = b + Sdpcmsz;
    q[0] = (u8)op; q[1] = (u8)(op >> 8); q[2] = (u8)(op >> 16); q[3] = (u8)(op >> 24);
    q[4] = (u8)tlen; q[5] = (u8)(tlen >> 8);
    q[8] = write ? 2 : 0;
    w.reqid++;
    q[10] = (u8)w.reqid; q[11] = (u8)(w.reqid >> 8);
    if (dlen) memcpy(q + Cmdsz, data, (size_t)dlen);
    if (write && rlen) memcpy(q + Cmdsz + dlen, res, (size_t)rlen);
    if (packetrw(1, b, len)) return stage("command send");
    w.txseq++;
    u64 end = timer_us() + 2000000;
    while (timer_us() < end) {
        if (!pending()) { delay_us(500); continue; }
        int n;
        u16 id;
        while ((n = readpkt()) > 0) {
            if (dispatch(n, &id) == 0 && id == w.reqid) {
                int doff = rx[7];
                u32 st = le32(rx + doff + 12);
                if (st) {                                 /* a BCME_ code: -1 error, -2 bad arg, -23 unsupported ... */
                    static char eb[48];
                    int code = (int)st, k = 0;
                    const char *t = "firmware said error ";
                    while (*t) eb[k++] = *t++;
                    if (code < 0) { eb[k++] = '-'; code = -code; }
                    char d[12]; int dn = 0;
                    do { d[dn++] = (char)('0' + code % 10); code /= 10; } while (code);
                    while (dn) eb[k++] = d[--dn];
                    t = " to command ";
                    while (*t) eb[k++] = *t++;
                    u32 o = op; dn = 0;
                    do { d[dn++] = (char)('0' + o % 10); o /= 10; } while (o);
                    while (dn) eb[k++] = d[--dn];
                    eb[k] = 0;
                    werr = eb;
                    return -1;
                }
                if (!write && rlen) memcpy(res, rx + doff + Cmdsz, (size_t)rlen);
                return 0;
            }
        }
    }
    return stage("no answer from the firmware");
}

static int setvar(const char *name, const void *val, int len)
{
    static u8 t[1500];
    int nl = (int)strlen(name) + 1;
    memcpy(t, name, (size_t)nl);
    memcpy(t + nl, val, (size_t)len);
    return wlcmd(1, 263, t, nl + len, 0, 0);
}
static int getvar(const char *name, void *val, int len) { return wlcmd(0, 262, name, (int)strlen(name) + 1, val, len); }
static int setint(const char *name, u32 v) { u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) }; return setvar(name, b, 4); }
static int cmdint(u32 op, u32 v) { u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) }; return wlcmd(1, op, b, 4, 0, 0); }

/* the regulatory data (.clm_blob) for 43436 / 43455 */
static int clm_load(const char *file)
{
    struct fat_file f;
    static u8 b[12 + 1400 + 8] __attribute__((aligned(4)));
    if (fw_open(file, &f)) return stage("CLM file missing");
    int flag = (1 << 12) | (1 << 1), n;
    while (!(flag & (1 << 2))) {
        n = fat_read(&f, b + 12, 1400);
        if (n <= 0) break;
        if (f.pos >= f.size) { while (n & 7) b[12 + n++] = 0; flag |= 1 << 2; }
        b[0] = (u8)flag; b[1] = (u8)(flag >> 8);
        b[2] = 2; b[3] = 0;                               /* type: CLM */
        b[4] = (u8)n; b[5] = (u8)(n >> 8); b[6] = 0; b[7] = 0;
        b[8] = b[9] = b[10] = b[11] = 0;
        if (setvar("clmload", b, 12 + n)) return stage("CLM load");
        flag &= ~(1 << 1);
    }
    return 0;
}

/* a setup step that went wrong is shown, but the next ones still run */
static int chk(const char *what, int r)
{
    if (r) kprintf("  %s: %s\n", what, werr);
    return r;
}

static int wl_init(void)
{
    if (chk("cur_etheraddr", getvar("cur_etheraddr", w.mac, 6))) return -1;
    chk("assoc_listen", setint("assoc_listen", 10));
    chk("power save off", cmdint(0x56, 0));
    chk("bus:txglom", setint("bus:txglom", 0));
    chk("bcn_timeout", setint("bcn_timeout", 10));
    chk("assoc_retry_max", setint("assoc_retry_max", 3));
    u8 ev[16];
    memset(ev, 0xFF, sizeof(ev));
    ev[40 / 8] &= (u8)~(1 << (40 % 8));                   /* E_RADIO */
    ev[44 / 8] &= (u8)~(1 << (44 % 8));                   /* E_PROBREQ_MSG */
    ev[54 / 8] &= (u8)~(1 << (54 % 8));
    ev[71 / 8] &= (u8)~(1 << (71 % 8));
    ev[20 / 8] &= (u8)~(1 << (20 % 8));
    ev[124 / 8] &= (u8)~(1 << (124 % 8));
    chk("event_msgs", setvar("event_msgs", ev, sizeof(ev)));
    chk("scan channel time", cmdint(0xb9, 0x28));
    chk("scan unassoc time", cmdint(0xbb, 0x28));
    chk("scan passive time", cmdint(0x102, 0x82));
    memset(w.ver, 0, sizeof(w.ver));
    getvar("ver", w.ver, sizeof(w.ver) - 1);
    for (int i = 0; w.ver[i]; i++) if (w.ver[i] == '\n' || w.ver[i] == '\r') w.ver[i] = 0;
    chk("roam_off", setint("roam_off", 1));
    chk("infrastructure mode", cmdint(0x14, 1));
    if (chk("UP (radio on)", cmdint(2, 1))) return -1;    /* WLC_UP */
    return 0;
}

/* ---------------- commands ---------------- */

int wifi_up(void) { return w.up; }

static int wifi_start(void)
{
    if (w.up) return 0;
    memset(&w, 0, sizeof(w));
    werr = "";
    missing[0] = 0;
    kprintf("WiFi: SDIO on GPIO34-39...\n");
    if (sdio_init()) return -1;
    if (sb_init()) return -1;
    kprintf("  chip %d rev %d (%s core), RAM %u KB\n", w.chipid == 0x4345 ? 43455 : w.chipid, w.chiprev, w.armcore == ARMcr4 ? "Cortex-R4" : "Cortex-M3", w.socramsize / 1024);
    if (fw_load()) return -1;
    if (sb_enable()) return -1;
    poll_events(100);
    if (fwtab[fwi].clm) {                                 /* newer firmware needs its CLM blob to switch the radio on */
        if (clm_load(fwtab[fwi].clm)) kprintf("  CLM (regulatory data): %s%s%s\n", werr, missing[0] ? " " : "", missing);
        else {
            u8 st[4] = { 0, 0, 0, 0 };
            if (!getvar("clmload_status", st, 4) && (st[0] | st[1] | st[2] | st[3])) kprintf("  CLM: the firmware did not accept it (status %d)\n", st[0]);
            else kprintf("  CLM loaded (%s)\n", fwtab[fwi].clm);
        }
        missing[0] = 0;
    }
    if (wl_init()) return -1;
    w.up = 1;
    kprintf("  firmware: %s\n  MAC %02x:%02x:%02x:%02x:%02x:%02x\n", w.ver,
            w.mac[0], w.mac[1], w.mac[2], w.mac[3], w.mac[4], w.mac[5]);
    return 0;
}

static void scan(void)
{
    /* escan, version 1: like Linux brcmfmac for "all channels" - no channel list, the
     * firmware scans every channel it may use (newer firmware rejects old chanspecs) */
    static const u8 params[4 + 2 + 2 + 4 + 32 + 6 + 1 + 1 + 4 * 4 + 4] = {
        1, 0, 0, 0,                                       /* version */
        1, 0,                                             /* action: start */
        0x34, 0x12,                                       /* sync id */
        0, 0, 0, 0,                                       /* ssid length: any network */
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,               /* any BSSID */
        2,                                                /* bss type: any */
        0,                                                /* active scan */
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,   /* nprobes, active, passive, home time: default */
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0, 0, 0, 0,                                       /* 0 channels, 0 SSIDs = all */
    };
    /* the 9front list (old d11n chanspecs), tried if the firmware does not like the first */
    static const u8 params_old[4 + 2 + 2 + 4 + 32 + 6 + 1 + 1 + 4 * 4 + 2 + 2 + 14 * 2 + 32 + 4] = {
        1, 0, 0, 0, 1, 0, 0x34, 0x12, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 2, 0,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        14, 0, 1, 0,
        0x01, 0x2b, 0x02, 0x2b, 0x03, 0x2b, 0x04, 0x2b, 0x05, 0x2e, 0x06, 0x2e, 0x07, 0x2e,
        0x08, 0x2b, 0x09, 0x2b, 0x0a, 0x2b, 0x0b, 0x2b, 0x0c, 0x2b, 0x0d, 0x2b, 0x0e, 0x2b,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    nbss = 0;
    scan_done = 0;
    io_lock();                                            /* core 1 reads the chip too once WiFi is up */
    if (cmdint(49, 0)) kprintf("  (active scan: %s)\n", werr);   /* SET_PASSIVE_SCAN 0 */
    int bad = setvar("escan", params, sizeof(params));
    if (bad) {
        kprintf("  escan: %s - trying the old channel list\n", werr);
        bad = setvar("escan", params_old, sizeof(params_old));
    }
    io_unlock();
    if (bad) { kprintf("scan: %s\n", werr); return; }
    kprintf("scanning...\n");
    u64 end = timer_us() + 8000000;
    while (!scan_done && timer_us() < end) {              /* the I/O core handles the results */
        if (con_trygetc() == 3) break;
        delay_us(20000);
    }
    /* strongest first */
    for (int i = 1; i < nbss; i++)
        for (int j = i; j > 0 && bsslist[j].rssi > bsslist[j - 1].rssi; j--) {
            struct bss t = bsslist[j]; bsslist[j] = bsslist[j - 1]; bsslist[j - 1] = t;
        }
    static const char *const sec[4] = { "open", "WEP", "WPA", "WPA2/3" };
    for (int i = 0; i < nbss; i++) {
        struct bss *b = &bsslist[i];
        kprintf("  %-32s ch %2d  %4d dBm  %-6s  %02x:%02x:%02x:%02x:%02x:%02x\n", b->ssid[0] ? b->ssid : "(hidden)",
                b->chan, b->rssi, sec[b->sec], b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4], b->bssid[5]);
    }
    kprintf("%d network(s)%s\n", nbss, scan_done ? "" : " (scan did not finish)");
}

/* ---------------- joined: frames for the network stack (core 1, io lock) ---------------- */

int wifi_joined(void) { return w.up && j_link && j_ssid_ok && (j_keyed || !j_need_key); }
const u8 *wifi_mac(void) { return w.mac; }
const char *wifi_ssid(void) { return j_ssid; }

static volatile int rx_more;
static u8 sup_out[160];
static int sup_outlen;
static int send_frame(const void *f, int len);

/* our RSN element as the firmware sent it in the association request (it must be in message 2) */
static void sup_fetch_ie(void)
{
    static u8 b[512] __attribute__((aligned(4)));
    u8 il[8];
    sup_ie_wanted = 0;
    if (getvar("assoc_info", il, 8)) return;
    u32 req = le32(il);
    if (!req || req > sizeof(b)) return;
    if (getvar("assoc_req_ies", b, (int)req)) return;
    for (u32 i = 0; i + 2 <= req && i + 2 + b[i + 1] <= req; i += 2 + b[i + 1])
        if (b[i] == 0x30 && b[i + 1] >= 18 && i + 3 < req && b[i + 2] == 1 && b[i + 3] == 0) {   /* RSN version 1 */
            wpa_start(&sup, sup_pmk, w.mac, b + i, 2 + b[i + 1]);
            d_ie_fw = 1; d_ie_len = 2 + b[i + 1];
            return;
        }
}

/* a key into the firmware ("wsec_key", struct brcmf_wsec_key_le, 164 bytes) */
static int sup_key(int idx, const u8 *key, int len, const u8 *ea, const u8 *rsc)
{
    u8 k[164];
    memset(k, 0, sizeof(k));
    k[0] = (u8)idx;
    k[4] = (u8)len;
    memcpy(k + 8, key, (size_t)len);
    if (len == 32) { u8 t[8]; memcpy(t, k + 8 + 24, 8); memcpy(k + 8 + 24, k + 8 + 16, 8); memcpy(k + 8 + 16, t, 8); }   /* TKIP: rx/tx MIC swapped, as brcmfmac */
    k[112] = len == 32 ? 2 : 4;                           /* CRYPTO_ALGO_TKIP / AES_CCM */
    if (ea) memcpy(k + 156, ea, 6);
    else k[116] = 2;                                      /* group key: BRCMF_PRIMARY_KEY */
    if (rsc) {                                            /* receive sequence of the group key */
        k[132] = 1;
        k[144] = rsc[0]; k[145] = rsc[1];
        k[140] = rsc[2]; k[141] = rsc[3]; k[142] = rsc[4]; k[143] = rsc[5];
    }
    return setvar("wsec_key", k, sizeof(k));
}

static void sup_eapol(const u8 *f, int len)
{
    static u8 c[1600];
    if (!host_sup || len > (int)sizeof(c)) return;
    d_eapol++;
    memcpy(c, f, (size_t)len);                            /* rx is reused by the commands below */
    if (!sup_started) { wpa_start(&sup, sup_pmk, w.mac, 0, 0); sup_ie_wanted = 1; sup_started = 1; }   /* came before E_LINK */
    if (sup_ie_wanted) sup_fetch_ie();
    if (!wpa_rx(&sup, c, len)) {
        sup_err = sup.err;
        if (sup.err && sup.err[0] == 'M' && ++sup_fails >= 2 && j_need_key && !j_keyed) j_fail = 100;   /* MIC wrong twice = password */
        return;
    }
    if (sup.last == 1) d_m1++; else if (sup.last == 3) d_m3++; else if (sup.last == 5) d_grp++;
    if (sup.outlen) { memcpy(sup_out, sup.out, (size_t)sup.outlen); sup_outlen = sup.outlen; }
    if (sup.set_ptk || sup.set_gtk) {                     /* after message 4 has left */
        sup_keys = 1;
        sup_gtk_only = !sup.set_ptk;
        sup_key_at = timer_us() + 30000;
        sup.set_ptk = sup.set_gtk = 0;
    }
}

static void sup_work(void)
{
    if (sup_outlen) {
        int r = send_frame(sup_out, sup_outlen);
        if (r != -2) { if (r > 0) d_sent++; else d_sendfail++; sup_outlen = 0; }
    }
    if (sup_keys && !sup_outlen && timer_us() >= sup_key_at) {
        sup_keys = 0;
        int r = 0;
        if (!sup_gtk_only) r |= sup_key(0, sup.ptk + 32, 16, sup.aa, 0);
        r |= sup_key(sup.gtk_idx, sup.gtk, sup.gtk_len, 0, sup.gtk_rsc);
        if (r) { sup_err = werr; return; }
        sup_err = 0;
        d_keys++;
        j_keyed = 1;
    }
}

/* the next data frame (Ethernet) into buf, 0 = none; events are handled on the way.
 * want_data = 0: only events (WiFi is up but Ethernet carries the traffic) */
int wifi_recv(void *buf, int max, int want_data)
{
    if (!w.up) return 0;
    for (int guard = 0; guard < 8; guard++) {
        if (!rx_more) { if (!pending()) break; rx_more = 1; }
        int n = readpkt();
        if (n <= 0) { rx_more = 0; break; }
        u16 id;
        int ch = dispatch(n, &id);
        if (ch == 2) {
            int doff = rx[7];
            if (n <= doff + 4) continue;
            int off = doff + 4 + (rx[doff + 3] << 2), len = n - off;
            if (len >= 14 && rx[off + 12] == 0x88 && rx[off + 13] == 0x8E) { sup_eapol(rx + off, len); continue; }
            if (!want_data || len < 14 || len > max) continue;
            memcpy(buf, rx + off, (size_t)len);
            return len;
        }
    }
    if (host_sup) sup_work();
    if (wifi_joined() && !j_told) {                       /* tell once: connected */
        j_told = 1;
        con_color(COL_CYAN); kprintf("\n[NET] WiFi connected to %s\n", j_ssid); con_color(COL_DEFAULT);
    } else if (!wifi_joined() && j_told == 1 && j_ssid_ok) {
        j_told = 2;
        con_color(COL_YELLOW); kprintf("\n[NET] WiFi lost %s\n", j_ssid); con_color(COL_DEFAULT);
    }
    return 0;
}

/* an Ethernet frame out: SDPCM header (channel 2) + BDC header + frame */
int wifi_send(const void *f, int len)
{
    if (!wifi_joined()) return -1;
    return send_frame(f, len);
}

static int send_frame(const void *f, int len)
{
    static u8 tb[1600] __attribute__((aligned(4)));
    if (!w.up || len > 1514) return -1;
    if (w.txseq == w.txwindow || (w.fcmask & (1 << 2))) return -2;   /* the firmware has no room now */
    int total = Sdpcmsz + 4 + len;
    memset(tb, 0, Sdpcmsz + 4);
    tb[0] = (u8)total; tb[1] = (u8)(total >> 8); tb[2] = (u8)~total; tb[3] = (u8)(~total >> 8);
    tb[4] = w.txseq;
    tb[5] = 2;                                            /* data channel */
    tb[7] = Sdpcmsz;                                      /* data offset */
    tb[Sdpcmsz] = 0x20;                                   /* BDC version 2 */
    memcpy(tb + Sdpcmsz + 4, f, (size_t)len);
    if (packetrw(1, tb, total)) return -3;
    w.txseq++;
    return len;
}

/* ---------------- join / leave (core 0) ---------------- */

static int join_send(const char *ssid, const char *pass)
{
    int sl = (int)strlen(ssid), pl = pass ? (int)strlen(pass) : 0;
    if (sl < 1 || sl > 32) { werr = "the network name must be 1-32 characters"; return -1; }
    if (pl && (pl < 8 || pl > 63)) { werr = "a WPA password has 8-63 characters"; return -1; }
    j_ssid_ok = j_link = j_keyed = j_fail = j_told = 0;
    j_need_key = pl > 0;
    memcpy(j_ssid, ssid, (size_t)sl);
    j_ssid[sl] = 0;
    /* WPA (TKIP) only networks need the old WPA mode; everything else WPA2 (AES) */
    u32 auth = 0x80;                                      /* WPA2_AUTH_PSK */
    int grp = 4;
    for (int i = 0; i < nbss; i++)
        if (!strcmp(bsslist[i].ssid, ssid)) { if (bsslist[i].sec == 2) auth = 0x04; if (bsslist[i].sec == 3) grp = bsslist[i].grp; }
    int r = 0;
    io_lock();
    host_sup = 0;
    sup_outlen = sup_keys = sup_fails = 0;
    sup_err = 0;
    r |= chk("wsec", setint("wsec", pl ? 6 : 0));        /* TKIP + AES; the same order as before for the Pi 3B firmware */
    if (pl && setint("sup_wpa", 1)) {                     /* no supplicant in this firmware (Pi 3B+): RoboOS does WPA2 */
        if (auth != 0x80) { io_unlock(); werr = "this firmware has no WPA supplicant and RoboOS does WPA2 only (the network is WPA1)"; return -1; }
        io_unlock();
        wpa_pmk(pass, ssid, sl, sup_pmk);                 /* ~0.1 s: 8192 x HMAC-SHA1 */
        io_lock();
        host_sup = 1;
        sup_started = 0;
        d_eapol = d_m1 = d_m3 = d_grp = d_sent = d_sendfail = d_ie_fw = d_ie_len = d_keys = 0;
        d_lost_type = d_lost_reason = 0;
        if (grp != 2) r |= chk("wsec", setint("wsec", 4));   /* AES only, unless the group key is TKIP */
        if (grp != 2) d_wpaie = setvar("wpaie", wpa_rsn_default, 22) == 0;   /* our RSN element in the association, as Linux */
    }
    if (!pl) chk("sup_wpa", setint("sup_wpa", 0));
    if (pl && !host_sup) {
        setint("sup_wpa2_eapver", 0xFFFFFFFF);
        setint("sup_wpa_tmo", 2500);
        u8 pmk[68];
        memset(pmk, 0, sizeof(pmk));
        pmk[0] = (u8)pl;
        pmk[2] = 1;                                       /* WSEC_PASSPHRASE */
        memcpy(pmk + 4, pass, (size_t)pl);
        delay_us(2000);
        r |= chk("password", wlcmd(1, 268, pmk, sizeof(pmk), 0, 0));   /* WLC_SET_WSEC_PMK */
    }
    r |= chk("band", cmdint(142, (u32)cfg_band));         /* WLC_SET_BAND: which band the join may use */
    r |= chk("infrastructure", cmdint(20, 1));
    r |= chk("auth open", cmdint(22, 0));
    r |= chk("wpa_auth", cmdint(165, pl ? auth : 0));
    u8 sb[36];
    memset(sb, 0, sizeof(sb));
    sb[0] = (u8)sl;
    memcpy(sb + 4, ssid, (size_t)sl);
    if (wlcmd(1, 26, sb, sizeof(sb), 0, 0)) {             /* WLC_SET_SSID */
        char e1[48];                                      /* werr's buffer is reused by the next command */
        int k = 0;
        for (const char *t = werr; *t && k < 47; t++) e1[k++] = *t;
        e1[k] = 0;
        u8 jp[68];                                        /* else the "join" iovar (brcmf_ext_join_params_le), as Linux */
        memset(jp, 0, sizeof(jp));
        memcpy(jp, sb, 36);
        memset(jp + 36, 0xFF, 20);                        /* scan type, probes, times: firmware defaults */
        memset(jp + 56, 0xFF, 6);                         /* any BSSID */
        if (setvar("join", jp, sizeof(jp))) { kprintf("  join: %s (and 'join': %s)\n", e1, werr); r = -1; }
    }
    io_unlock();
    if (host_sup && !r) kprintf("  (this firmware has no WPA supplicant: RoboOS does the WPA2 handshake)\n");
    return r ? -1 : 0;
}

/* /WIFI.TXT: "ssid=<name>", "psk=<password>" and "band=2|5|auto" lines */
static char cfg_ssid[33], cfg_psk[64];
static int load_cfg(void)
{
    struct fat_file f;
    char b[200];
    cfg_ssid[0] = cfg_psk[0] = 0;
    cfg_band = 0;
    if (!fat_mounted() || fat_open("/WIFI.TXT", &f) || f.is_dir) return 0;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return 0;
    b[n] = 0;
    for (char *l = b; l && *l; ) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        int k = (int)strlen(l);
        while (k && (l[k - 1] == '\r' || l[k - 1] == ' ')) l[--k] = 0;
        char *dst = 0; int max = 0;
        if (!strncmp(l, "ssid=", 5)) { dst = cfg_ssid; max = 32; l += 5; }
        else if (!strncmp(l, "psk=", 4)) { dst = cfg_psk; max = 63; l += 4; }
        else if (!strncmp(l, "band=", 5)) cfg_band = l[5] == '5' ? 1 : l[5] == '2' ? 2 : 0;
        if (dst) { int i = 0; for (; l[i] && i < max; i++) dst[i] = l[i]; dst[i] = 0; }
        l = e ? e + 1 : 0;
    }
    return cfg_ssid[0] != 0;
}

static void save_cfg(const char *ssid, const char *psk)
{
    char b[160];
    int n = 0;
    const char *t = "ssid=";
    while (*t) b[n++] = *t++;
    while (*ssid && n < 50) b[n++] = *ssid++;
    t = "\r\npsk=";
    while (*t) b[n++] = *t++;
    while (psk && *psk && n < 116) b[n++] = *psk++;
    b[n++] = '\r'; b[n++] = '\n';
    if (cfg_band) { t = cfg_band == 1 ? "band=5\r\n" : "band=2\r\n"; while (*t) b[n++] = *t++; }
    if (!fat_mounted() || fat_write_file("/WIFI.TXT", b, (u32)n)) kprintf("(not saved: no SD card or write error)\n");
}

/* core 0 at boot (before core 1 starts): /WIFI.TXT -> start the chip and ask to join, do not wait */
void wifi_boot(void)
{
    if (!load_cfg()) return;
    if (wifi_start()) { kprintf("WiFi: %s%s%s\n", werr, missing[0] ? " " : "", missing); return; }
    if (join_send(cfg_ssid, cfg_psk[0] ? cfg_psk : 0)) kprintf("WiFi join: %s\n", werr);
    else kprintf("WiFi: joining %s (in the background)\n", cfg_ssid);
}

static void wpa_diag(void)
{
    if (!host_sup) return;
    static const char *const lost[] = { "", "unspecified", "auth no longer valid", "left", "inactivity", "AP busy", "class 2", "class 3",
        "disassociated", "not authenticated", "power", "channels", "", "invalid element", "MIC failure", "4-way handshake timeout",
        "group key timeout", "element in 4-way differs", "group cipher", "pairwise cipher", "AKMP", "RSN version", "RSN caps", "802.1X failed", "cipher rejected" };
    kprintf("  WPA: key frames in %d (msg1 %d, msg3 %d, group %d), out %d (failed %d), keys set %d\n",
            d_eapol, d_m1, d_m3, d_grp, d_sent, d_sendfail, d_keys);
    kprintf("  RSN element %s (%d B), wpaie %s%s%s\n", d_ie_fw ? "from the firmware" : "built in", d_ie_fw ? d_ie_len : 22,
            d_wpaie ? "set" : "refused", sup_err ? ", last problem: " : "", sup_err ? sup_err : "");
    if (d_lost_type)
        kprintf("  link lost: %s, reason %d%s%s\n", d_lost_type == 16 ? "link down" : d_lost_type <= 6 ? "deauth" : "disassoc",
                d_lost_reason, d_lost_reason > 0 && d_lost_reason < 25 ? " = " : "", d_lost_reason > 0 && d_lost_reason < 25 ? lost[d_lost_reason] : "");
}

static void wait_join(void)
{
    u64 end = timer_us() + 20000000;
    int shown = 0;
    while (!wifi_joined() && !j_fail && timer_us() < end) {
        if (j_link && !shown) { kprintf("  associated%s\n", j_need_key ? ", waiting for the keys (WPA)" : ""); shown = 1; }
        if (con_trygetc() == 3) { kprintf("^C\n"); return; }
        delay_us(20000);
    }
    if (wifi_joined()) { j_told = 1; kprintf("connected to %s - the address comes by DHCP ('ip')\n", j_ssid); return; }
    wpa_diag();
    if (j_fail == 4) kprintf("join failed: network %s not found\n", j_ssid);
    else if (j_fail >= 100) kprintf("join failed: the WPA handshake did not finish (wrong password?)%s%s\n", sup_err ? " - " : "", sup_err ? sup_err : "");
    else if (j_fail) kprintf("join failed (status %d)\n", j_fail - 1);
    else kprintf("join: no answer in 20 s%s%s%s\n", j_link ? " (associated, but no WPA keys - wrong password?)" : "",
                 sup_err ? " - " : "", sup_err ? sup_err : "");
}

void wifi_cmd(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "join")) {
        if (!w.up && wifi_start()) { kprintf("WiFi: %s%s%s\n", werr, missing[0] ? " " : "", missing); return; }
        const char *pass = argc > 3 ? argv[3] : 0;
        if (join_send(argv[2], pass)) { kprintf("join: %s\n", werr); return; }
        save_cfg(argv[2], pass);
        kprintf("joining %s... (Ctrl+C stops waiting; saved in /WIFI.TXT)\n", argv[2]);
        wait_join();
        return;
    }
    if (argc > 1 && !strcmp(argv[1], "band")) {
        static const char *const bn[3] = { "auto (2.4 or 5 GHz, the firmware picks)", "5 GHz only", "2.4 GHz only" };
        if (argc > 2) {
            int b = !strcmp(argv[2], "5") ? 1 : (!strcmp(argv[2], "2") || !strcmp(argv[2], "2.4")) ? 2 : !strcmp(argv[2], "auto") ? 0 : -1;
            if (b < 0) { kprintf("usage: wifi band 2|5|auto\n"); return; }
            int saved = load_cfg();                       /* keeps the network + password in /WIFI.TXT */
            cfg_band = b;
            if (saved) save_cfg(cfg_ssid, cfg_psk[0] ? cfg_psk : 0);
            else kprintf("(saved with the next 'wifi join')\n");
            if (w.up) { io_lock(); cmdint(142, (u32)b); io_unlock(); }
            kprintf("WiFi band: %s%s\n", bn[b], j_ssid_ok ? " - 'wifi join' again to switch now" : "");
        } else kprintf("WiFi band: %s\n", bn[cfg_band]);
        return;
    }
    if (argc > 1 && (!strcmp(argv[1], "leave") || !strcmp(argv[1], "forget"))) {
        if (w.up) { io_lock(); wlcmd(1, 52, 0, 0, 0, 0); io_unlock(); }   /* WLC_DISASSOC */
        j_link = j_keyed = j_ssid_ok = 0; j_told = 0;
        if (!strcmp(argv[1], "forget") && fat_mounted()) { fat_remove("/WIFI.TXT"); kprintf("/WIFI.TXT removed\n"); }
        kprintf("WiFi: not connected\n");
        return;
    }
    if (argc > 1 && (!strcmp(argv[1], "init") || !strcmp(argv[1], "scan"))) {
        if (!strcmp(argv[1], "init")) { io_lock(); w.up = 0; io_unlock(); }
        if (wifi_start()) {
            kprintf("WiFi: %s", werr);
            if (missing[0]) kprintf(" (%s)", missing);
            kprintf("  [INT=%x RESP=%x]\n", dbg_int, dbg_resp);
            if (missing[0]) kprintf("  copy the files from Raspberry Pi OS /lib/firmware/brcm to /FIRMWARE on the card\n");
            return;
        }
        if (!strcmp(argv[1], "scan")) scan();
        return;
    }
    if (argc > 1) { kprintf("usage: wifi [init | scan | join <network> [password] | band 2|5|auto | leave | forget]\n"); return; }
    if (!w.up) { kprintf("WiFi off ('wifi init' or 'wifi scan' starts it; firmware in /FIRMWARE)\n"); return; }
    kprintf("WiFi: chip %d rev %d, firmware %s\n  MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            w.chipid == 0x4345 ? 43455 : w.chipid, w.chiprev, w.ver,
            w.mac[0], w.mac[1], w.mac[2], w.mac[3], w.mac[4], w.mac[5]);
    if (wifi_joined()) {
        u8 r[4] = { 0, 0, 0, 0 }, ch[12];
        memset(ch, 0, sizeof(ch));
        io_lock();
        wlcmd(0, 127, 0, 0, r, 4);                        /* WLC_GET_RSSI */
        wlcmd(0, 29, 0, 0, ch, 12);                       /* WLC_GET_CHANNEL: hw_channel first */
        io_unlock();
        int c = (int)le32(ch);
        kprintf("  connected to %s, channel %d (%s GHz), signal %d dBm%s%s\n", j_ssid, c, c > 14 ? "5" : "2.4",
                (int)(r[0] | r[1] << 8 | r[2] << 16 | (u32)r[3] << 24),
                host_sup ? ", WPA2 by RoboOS" : "", link_wifi() ? ", carries the network now" : " (Ethernet carries the network)");
    } else if (j_ssid[0]) { kprintf("  not connected (%s)\n", j_ssid); wpa_diag(); }
    else kprintf("  not connected ('wifi join <network> <password>')\n");
    if (cfg_band) kprintf("  band: %s GHz only ('wifi band auto' allows both)\n", cfg_band == 1 ? "5" : "2.4");
}
