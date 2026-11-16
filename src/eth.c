/*
 * Ethernet over USB, two chip families (both are high-speed USB devices behind
 * the onboard hub; registers via vendor control requests 0xA0/0xA1, frames over
 * two bulk endpoints):
 *
 *  - SMSC LAN9514 / LAN9512 (0424:ec00): Raspberry Pi 3B, Pi 2
 *      RX: [status word][frame + FCS]            status bits 16-29 = length incl. FCS
 *      TX: [command A][command B][frame]         the MAC adds padding and the FCS
 *      Register layout as in Linux smsc95xx / Circle.
 *  - Microchip LAN7515 = LAN7800 (0424:7800): Raspberry Pi 3B+
 *      RX: [cmd A][cmd B][cmd C, 2 B][frame + FCS][pad to 4 B (counted from +2)]
 *      TX: [cmd A][cmd B][frame]                 cmd A = length | insert FCS
 *      Register layout as in Linux lan78xx. The MAC speed and duplex have to be set
 *      by hand after the link comes up, and only while TX and RX are disabled.
 *
 * An empty bulk IN is answered with a zero-length packet (BIR = 0), which suits
 * our polling driver.
 */
#include "usb.h"

#define ID_REV        0x00
#define INT_STS       0x08
#define TX_CFG        0x10
#define HW_CFG        0x14
#define PM_CTRL       0x20
#define LED_GPIO_CFG  0x24
#define MAC_CR        0x100
#define ADDRH         0x104
#define ADDRL         0x108
#define MII_ADDR      0x114
#define MII_DATA      0x118

#define HW_CFG_BIR    0x1000      /* 1 = NAK when empty, 0 = zero-length packet */
#define HW_CFG_RXDOFF 0x0600
#define HW_CFG_MEF    0x0020      /* several frames per USB transfer */
#define HW_CFG_LRST   0x0008
#define HW_CFG_BCE    0x0002
#define PM_PHY_RST    0x0010
#define TX_CFG_ON     0x0004
#define MAC_RCVOWN    0x00800000
#define MAC_FDPX      0x00100000
#define MAC_TXEN      0x00000008
#define MAC_RXEN      0x00000004
#define MII_WRITE     0x02
#define MII_BUSY      0x01
#define PHY_ADDR      1

/* ---- LAN7800 / LAN7515 ---- */
#define L_ID_REV       0x00
#define L_INT_STS      0x0C
#define L_HW_CFG       0x10
#define L_PMT_CTL      0x14
#define L_USB_CFG0     0x80
#define L_RFE_CTL      0xB0
#define L_FCT_RX_CTL   0xC0
#define L_FCT_TX_CTL   0xC4
#define L_FCT_RX_END   0xC8
#define L_FCT_TX_END   0xCC
#define L_FCT_FLOW     0xD0
#define L_MAC_CR       0x100
#define L_MAC_RX       0x104
#define L_MAC_TX       0x108
#define L_FLOW         0x10C
#define L_RX_ADDRH     0x118
#define L_RX_ADDRL     0x11C
#define L_MII_ACC      0x120
#define L_MII_DATA     0x124
#define L_MAF_HI0      0x400
#define L_MAF_LO0      0x404

#define L_HW_LRST      0x00000002
#define L_HW_MEF       0x00000010
#define L_HW_CLK125    0x02000000
#define L_HW_REFCLK25  0x01000000
#define L_PMT_PHY_RST  0x00000010
#define L_PMT_READY    0x00000080
#define L_USB_BIR      0x00000040
#define L_USB_BCE      0x00000020
#define L_RFE_BCAST    0x00000400
#define L_RFE_MCAST    0x00000200
#define L_RFE_PERFECT  0x00000002
#define L_FCT_EN       0x80000000
#define L_FCT_RST      0x40000000
#define L_MAC_FDPX     0x00000008
#define L_MAC_SPD_MASK 0x00000006
#define L_MAC_SPD_100  0x00000002
#define L_MAC_AUTO     0x00001800      /* auto duplex + auto speed: off, the driver sets them */
#define L_MAC_EEE      0x00020000
#define L_RX_MAXSZ     0x3FFF0000
#define L_RXEN         0x00000001
#define L_TXEN         0x00000001
#define L_TX_FCS       0x00400000
#define L_RX_RED       0x00400000
#define L_RX_HARD_ERR  0x003F0000

enum { FAM_NONE, FAM_SMSC, FAM_LAN78 };
static int fam;
static u16 mii_addr_reg = MII_ADDR, mii_data_reg = MII_DATA;
static int cur_speed, cur_full;          /* LAN78xx: what the MAC is set to (0 = link down) */
static u64 next_tick;

#define TX_FIRST_SEG  0x2000
#define TX_LAST_SEG   0x1000
#define RX_ERROR      0x8000

static struct usb_dev *ed;
static u8  ep_in, ep_out, pid_in, pid_out;
static u8  mac[6];
static u32 chip_id;
static const char *stage = "";
static int err_code;
static u32 err_hcint;

/* received USB transfer, may hold more than one frame */
static u8  rxbuf[2048] __attribute__((aligned(16)));
static int rx_len, rx_off;
static u8  txbuf[1600] __attribute__((aligned(16)));

struct eth_stats eth_stat;

static int fail(const char *what, int code)
{
    stage = what; err_code = code; err_hcint = usb_last_hcint();
    return -1;
}

static int reg_write(u16 reg, u32 v)
{
    u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
    int r = usb_control(ed, 0x40, 0xA0, 0, reg, b, 4);
    return r < 0 ? r : 0;
}

static int reg_read(u16 reg, u32 *v)
{
    u8 b[4];
    int r = usb_control(ed, 0xC0, 0xA1, 0, reg, b, 4);
    if (r < 4) return r < 0 ? r : USB_ERR;
    *v = b[0] | (b[1] << 8) | (b[2] << 16) | ((u32)b[3] << 24);
    return 0;
}

/* wait until the bits in mask are clear */
static int reg_wait(u16 reg, u32 mask, u32 ms)
{
    u64 end = timer_us() + (u64)ms * 1000;
    u32 v;
    for (;;) {
        int r = reg_read(reg, &v);
        if (r) return r;
        if (!(v & mask)) return 0;
        if (timer_us() > end) return USB_TIMEOUT;
        delay_us(1000);
    }
}

static int phy_read(u8 reg, u16 *v)
{
    u32 d;
    int r;
    if ((r = reg_wait(mii_addr_reg, MII_BUSY, 100))) return r;
    if ((r = reg_write(mii_addr_reg, (PHY_ADDR << 11) | ((u32)reg << 6) | MII_BUSY))) return r;
    if ((r = reg_wait(mii_addr_reg, MII_BUSY, 100))) return r;
    if ((r = reg_read(mii_data_reg, &d))) return r;
    *v = (u16)d;
    return 0;
}

static int phy_write(u8 reg, u16 v)
{
    int r;
    if ((r = reg_wait(mii_addr_reg, MII_BUSY, 100))) return r;
    if ((r = reg_write(mii_data_reg, v))) return r;
    if ((r = reg_write(mii_addr_reg, (PHY_ADDR << 11) | ((u32)reg << 6) | MII_WRITE | MII_BUSY))) return r;
    return reg_wait(mii_addr_reg, MII_BUSY, 100);
}

void eth_reset(void) { ed = 0; fam = FAM_NONE; cur_speed = 0; rx_len = rx_off = 0; stage = ""; chip_id = 0; }

int eth_present(void) { return ed != 0; }
int eth_is(const struct usb_dev *d) { return ed && ed == d; }
const u8 *eth_mac(void) { return mac; }
u32 eth_chip(void) { return chip_id; }
const char *eth_chip_name(void)
{
    return fam == FAM_SMSC ? "LAN9514 (SMSC)" : fam == FAM_LAN78 ? "LAN7515 (LAN7800)" : "none";
}

/* 0 = initialized fine; otherwise the failed step for the screen */
const char *eth_error(int *code, u32 *hcint)
{
    if (code) *code = err_code;
    if (hcint) *hcint = err_hcint;
    return stage[0] ? stage : 0;
}

static int smsc_init(void)
{
    int r;
    u32 v;
    u16 p;
    mii_addr_reg = MII_ADDR; mii_data_reg = MII_DATA;

    if ((r = reg_read(ID_REV, &chip_id))) return fail("read ID", r);

    /* soft reset of the MAC, then of the PHY */
    if ((r = reg_write(HW_CFG, HW_CFG_LRST))) return fail("lite reset", r);
    if ((r = reg_wait(HW_CFG, HW_CFG_LRST, 1000))) return fail("lite reset wait", r);
    if ((r = reg_write(PM_CTRL, PM_PHY_RST))) return fail("PHY reset", r);
    if ((r = reg_wait(PM_CTRL, PM_PHY_RST, 1000))) return fail("PHY reset wait", r);

    /* MAC address from the firmware (the board has no EEPROM for it) */
    u32 m[2] = { 0, 0 };
    mbox_prop(0x00010003, m, 2);
    memcpy(mac, m, 6);
    if ((r = reg_write(ADDRL, mac[0] | (mac[1] << 8) | (mac[2] << 16) | ((u32)mac[3] << 24))))
        return fail("set MAC", r);
    if ((r = reg_write(ADDRH, mac[4] | (mac[5] << 8)))) return fail("set MAC", r);

    /* one frame per transfer, no offset, zero-length packet when empty */
    if ((r = reg_read(HW_CFG, &v))) return fail("HW_CFG", r);
    v &= ~(HW_CFG_BIR | HW_CFG_MEF | HW_CFG_BCE | HW_CFG_RXDOFF);
    if ((r = reg_write(HW_CFG, v))) return fail("HW_CFG", r);
    reg_write(INT_STS, 0xFFFFFFFF);
    reg_write(LED_GPIO_CFG, 0x01110000);     /* link / speed / duplex LEDs on the jack */

    /* PHY: reset, advertise 10/100 half/full, start auto-negotiation */
    if ((r = phy_write(0, 0x8000))) return fail("PHY BMCR reset", r);
    u64 end = timer_us() + 500000;
    do {
        if ((r = phy_read(0, &p))) return fail("PHY read", r);
    } while ((p & 0x8000) && timer_us() < end);
    if (p & 0x8000) return fail("PHY stuck in reset", 0);
    if ((r = phy_write(4, 0x05E1))) return fail("PHY advertise", r);
    phy_read(29, &p);                        /* clear pending PHY interrupts */
    if ((r = phy_write(0, 0x1200))) return fail("PHY autoneg", r);

    if ((r = reg_write(TX_CFG, TX_CFG_ON))) return fail("TX on", r);
    if ((r = reg_read(MAC_CR, &v))) return fail("MAC_CR", r);
    if ((r = reg_write(MAC_CR, v | MAC_TXEN | MAC_RXEN))) return fail("MAC_CR", r);
    return 1;
}


static int lan78_init(void)
{
    int r;
    u32 v;
    u16 p;
    mii_addr_reg = L_MII_ACC; mii_data_reg = L_MII_DATA;
    cur_speed = 0;

    if ((r = reg_read(L_ID_REV, &chip_id))) return fail("read ID", r);
    if ((r = reg_read(L_HW_CFG, &v))) return fail("HW_CFG", r);
    if ((r = reg_write(L_HW_CFG, v | L_HW_LRST))) return fail("lite reset", r);
    if ((r = reg_wait(L_HW_CFG, L_HW_LRST, 1000))) return fail("lite reset wait", r);

    /* MAC address from the firmware: station address + perfect filter 0 */
    u32 m[2] = { 0, 0 };
    mbox_prop(0x00010003, m, 2);
    memcpy(mac, m, 6);
    u32 lo = mac[0] | (mac[1] << 8) | (mac[2] << 16) | ((u32)mac[3] << 24), hi = mac[4] | (mac[5] << 8);
    if ((r = reg_write(L_RX_ADDRL, lo)) || (r = reg_write(L_RX_ADDRH, hi))) return fail("set MAC", r);
    if ((r = reg_write(L_MAF_LO0, lo)) || (r = reg_write(L_MAF_HI0, hi | 0x80000000u))) return fail("set MAC filter", r);

    /* zero-length packet on an empty bulk IN, one frame per transfer */
    if ((r = reg_read(L_USB_CFG0, &v))) return fail("USB_CFG0", r);
    if ((r = reg_write(L_USB_CFG0, v & ~(L_USB_BIR | L_USB_BCE)))) return fail("USB_CFG0", r);
    if ((r = reg_read(L_HW_CFG, &v))) return fail("HW_CFG", r);
    if ((r = reg_write(L_HW_CFG, (v & ~L_HW_MEF) | L_HW_CLK125 | L_HW_REFCLK25))) return fail("HW_CFG", r);

    /* FIFOs (12 KB each), no flow control, accept our address + broadcast + multicast */
    if ((r = reg_write(L_FCT_RX_END, (12 * 1024 - 512) / 512))) return fail("FIFO size", r);
    if ((r = reg_write(L_FCT_TX_END, (12 * 1024 - 512) / 512))) return fail("FIFO size", r);
    reg_write(L_INT_STS, 0xFFFFFFFF);
    reg_write(L_FLOW, 0);
    reg_write(L_FCT_FLOW, 0);
    if ((r = reg_read(L_RFE_CTL, &v))) return fail("RFE_CTL", r);
    if ((r = reg_write(L_RFE_CTL, v | L_RFE_BCAST | L_RFE_MCAST | L_RFE_PERFECT))) return fail("RFE_CTL", r);

    /* PHY reset (done when READY is set again) */
    if ((r = reg_read(L_PMT_CTL, &v))) return fail("PMT_CTL", r);
    if ((r = reg_write(L_PMT_CTL, v | L_PMT_PHY_RST))) return fail("PHY reset", r);
    u64 end = timer_us() + 1000000;
    for (;;) {
        if ((r = reg_read(L_PMT_CTL, &v))) return fail("PHY reset wait", r);
        if (!(v & L_PMT_PHY_RST) && (v & L_PMT_READY)) break;
        if (timer_us() > end) return fail("PHY reset timeout", 0);
        delay_us(1000);
    }

    /* MAC: speed and duplex are set by hand when the link comes up; max frame with VLAN tag */
    if ((r = reg_read(L_MAC_CR, &v))) return fail("MAC_CR", r);
    if ((r = reg_write(L_MAC_CR, v & ~(L_MAC_AUTO | L_MAC_EEE)))) return fail("MAC_CR", r);
    if ((r = reg_read(L_MAC_RX, &v))) return fail("MAC_RX", r);
    if ((r = reg_write(L_MAC_RX, (v & ~(L_RX_MAXSZ | L_RXEN)) | (1522u << 16)))) return fail("MAC_RX", r);

    /* PHY: reset, advertise 10/100 only (the USB 2.0 link cannot use 1000 anyway), autoneg */
    if ((r = phy_write(0, 0x8000))) return fail("PHY BMCR reset", r);
    end = timer_us() + 500000;
    do {
        if ((r = phy_read(0, &p))) return fail("PHY read", r);
    } while ((p & 0x8000) && timer_us() < end);
    if (p & 0x8000) return fail("PHY stuck in reset", 0);
    phy_write(9, 0);
    if ((r = phy_write(4, 0x05E1))) return fail("PHY advertise", r);
    if ((r = phy_write(0, 0x1200))) return fail("PHY autoneg", r);
    return 1;                                /* TX/RX are started by link_update() */
}

int eth_attach(struct usb_dev *d, const u8 *cfg, int len)
{
    int f = 0, r;

    if (ed || d->vid != 0x0424) return 0;
    if (d->pid == 0xEC00) f = FAM_SMSC;
    else if (d->pid == 0x7800 || d->pid == 0x7850) f = FAM_LAN78;
    if (!f) return 0;

    ep_in = ep_out = 0;
    for (int i = 0; i + 1 < len && cfg[i]; i += cfg[i])
        if (cfg[i + 1] == 5 && i + 3 < len && (cfg[i + 3] & 3) == EP_BULK) {
            if (cfg[i + 2] & 0x80) { if (!ep_in) ep_in = cfg[i + 2] & 0xF; }
            else if (!ep_out) ep_out = cfg[i + 2] & 0xF;
        }
    ed = d;
    fam = f;
    pid_in = pid_out = PID_DATA0;
    rx_len = rx_off = 0;
    next_tick = 0;
    cur_speed = 0;
    memset(&eth_stat, 0, sizeof(eth_stat));
    stage = ""; err_code = 0; err_hcint = 0;
    if (!ep_in || !ep_out) return fail("no bulk endpoints", 0);

    r = f == FAM_SMSC ? smsc_init() : lan78_init();
    return r;
}

static int lan78_set(u16 reg, u32 mask, u32 val)
{
    u32 v;
    if (reg_read(reg, &v)) return -1;
    return reg_write(reg, (v & ~mask) | val);
}

/* link went down: stop the MAC so that nothing is sent into the void */
static void lan78_down(void)
{
    lan78_set(L_MAC_TX, L_TXEN, 0);
    lan78_set(L_MAC_RX, L_RXEN, 0);
    cur_speed = 0;
    rx_len = rx_off = 0;
}

/* link is up with this speed / duplex: set the MAC (TX and RX off!), flush, start */
static void lan78_up(int speed, int full)
{
    lan78_set(L_MAC_TX, L_TXEN, 0);
    lan78_set(L_MAC_RX, L_RXEN, 0);
    lan78_set(L_MAC_CR, L_MAC_SPD_MASK | L_MAC_FDPX | L_MAC_AUTO,
              (speed == 100 ? L_MAC_SPD_100 : 0) | (full ? L_MAC_FDPX : 0));
    lan78_set(L_FCT_RX_CTL, L_FCT_RST, L_FCT_RST);
    lan78_set(L_FCT_TX_CTL, L_FCT_RST, L_FCT_RST);
    delay_us(1000);
    lan78_set(L_MAC_TX, L_TXEN, L_TXEN);
    lan78_set(L_FCT_TX_CTL, L_FCT_EN, L_FCT_EN);
    lan78_set(L_FCT_RX_CTL, L_FCT_EN, L_FCT_EN);
    lan78_set(L_MAC_RX, L_RXEN, L_RXEN);
    cur_speed = speed; cur_full = full;
    rx_len = rx_off = 0;
}

/* 0 = no link, else 10 or 100; *full = full duplex. Also tells the MAC about speed/duplex. */
static int link_update(int *full)
{
    u16 s = 0, sp = 0;
    u32 v;
    if (!ed || stage[0]) return 0;
    phy_read(1, &s);                         /* link bit is latched: read twice */
    if (phy_read(1, &s) || !(s & 0x0004)) {
        if (fam == FAM_LAN78 && cur_speed) lan78_down();
        return 0;
    }
    if (fam == FAM_LAN78) {                  /* result of the auto-negotiation */
        u16 adv = 0, lpa = 0;
        if (phy_read(4, &adv) || phy_read(5, &lpa)) return cur_speed;
        u16 c = adv & lpa;
        int speed = (c & 0x0180) ? 100 : 10;
        int fd = (c & 0x0100) || (!(c & 0x0180) && (c & 0x0040));
        if (speed != cur_speed || fd != cur_full) lan78_up(speed, fd);
        if (full) *full = cur_full;
        return cur_speed;
    }
    if (phy_read(31, &sp)) return 0;         /* PHY special control/status */
    int fd = (sp & 0x10) != 0;
    if (full) *full = fd;
    if (!reg_read(MAC_CR, &v)) {
        u32 n = fd ? (v | MAC_FDPX) & ~MAC_RCVOWN : (v & ~MAC_FDPX) | MAC_RCVOWN;
        if (n != v) reg_write(MAC_CR, n);
    }
    return (sp & 0x08) ? 100 : 10;
}

int eth_link(int *full) { next_tick = timer_us() + 1000000; return link_update(full); }

/* once a second: follow the link (the MAC must know speed and duplex) */
static void link_tick(void)
{
    u64 now = timer_us();
    if (now < next_tick) return;
    next_tick = now + 1000000;
    link_update(0);
}

int eth_send(const void *frame, int len)
{
    if (!ed || stage[0] || len < 14 || len > 1514) return -1;
    link_tick();
    if (fam == FAM_LAN78 && !cur_speed) return -1;           /* link down: MAC is stopped */
    u32 a = TX_FIRST_SEG | TX_LAST_SEG | (u32)len, b = (u32)len;
    if (fam == FAM_LAN78) { a = (u32)len | L_TX_FCS; b = 0; }
    memcpy(txbuf, &a, 4);
    memcpy(txbuf + 4, &b, 4);
    memcpy(txbuf + 8, frame, len);
    int total = len + 8;
    int r = usb_bulk(ed, ep_out, 0, &pid_out, txbuf, total, 100);
    if (r >= 0 && total % 512 == 0)          /* a full last packet needs a terminator */
        r = usb_bulk(ed, ep_out, 0, &pid_out, txbuf, 0, 100);
    if (r < 0) { eth_stat.tx_err++; eth_stat.last_err = r; eth_stat.last_hcint = usb_last_hcint(); return r; }
    eth_stat.tx_ok++;
    return len;
}

/* one received frame (without FCS) into buf; 0 = nothing, <0 = USB error */
int eth_recv(void *buf, int max)
{
    if (!ed || stage[0]) return 0;
    link_tick();
    if (fam == FAM_LAN78 && !cur_speed) return 0;
    for (int pass = 0; pass < 2; pass++) {
        while (fam == FAM_LAN78 && rx_off + 10 <= rx_len) {
            u32 a;
            memcpy(&a, rxbuf + rx_off, 4);
            int size = (int)(a & 0x3FFF);                    /* frame + FCS */
            u8 *f = rxbuf + rx_off + 10;
            if (size < 4 || rx_off + 10 + size > rx_len) { eth_stat.rx_bad++; rx_len = 0; break; }
            rx_off += 10 + size + ((4 - ((size + 2) % 4)) % 4);
            if ((a & L_RX_RED) && (a & L_RX_HARD_ERR)) { eth_stat.rx_bad++; continue; }
            size -= 4;                                       /* drop the FCS */
            if (size > max) { eth_stat.rx_bad++; continue; }
            memcpy(buf, f, size);
            eth_stat.rx_ok++;
            return size;
        }
        while (fam == FAM_SMSC && rx_off + 4 <= rx_len) {
            u32 st;
            memcpy(&st, rxbuf + rx_off, 4);
            int fl = (st >> 16) & 0x3FFF;
            u8 *f = rxbuf + rx_off + 4;
            if (fl < 4 || rx_off + 4 + fl > rx_len) { eth_stat.rx_bad++; rx_len = 0; break; }
            rx_off += (4 + fl + 3) & ~3;
            if (st & RX_ERROR) { eth_stat.rx_bad++; continue; }
            fl -= 4;                         /* drop the FCS */
            if (fl > max) { eth_stat.rx_bad++; continue; }
            memcpy(buf, f, fl);
            eth_stat.rx_ok++;
            return fl;
        }
        if (pass) break;
        rx_off = 0;
        rx_len = 0;
        int r = usb_bulk(ed, ep_in, 1, &pid_in, rxbuf, sizeof(rxbuf), 5);
        if (r == USB_NAK || r == USB_TIMEOUT || r == 0) { eth_stat.rx_empty++; return 0; }
        if (r < 0) { eth_stat.rx_err++; eth_stat.last_err = r; eth_stat.last_hcint = usb_last_hcint(); return r; }
        rx_len = r;
    }
    return 0;
}
