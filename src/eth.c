/*
 * Ethernet: SMSC LAN9514 / LAN9512 (USB 0424:ec00), the chip on the
 * Raspberry Pi 3B and Pi 2. It is a high-speed USB device behind the onboard
 * hub: registers are accessed with vendor control requests, frames travel
 * over two bulk endpoints.
 *
 *   RX: [status word][frame + FCS]      status bits 16-29 = length incl. FCS
 *   TX: [command A][command B][frame]   the MAC adds padding and the FCS
 *
 * An empty bulk IN is answered with a zero-length packet (HW_CFG.BIR = 0),
 * which suits our polling driver. Register layout as in Linux smsc95xx / Circle.
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
    if ((r = reg_wait(MII_ADDR, MII_BUSY, 100))) return r;
    if ((r = reg_write(MII_ADDR, (PHY_ADDR << 11) | ((u32)reg << 6) | MII_BUSY))) return r;
    if ((r = reg_wait(MII_ADDR, MII_BUSY, 100))) return r;
    if ((r = reg_read(MII_DATA, &d))) return r;
    *v = (u16)d;
    return 0;
}

static int phy_write(u8 reg, u16 v)
{
    int r;
    if ((r = reg_wait(MII_ADDR, MII_BUSY, 100))) return r;
    if ((r = reg_write(MII_DATA, v))) return r;
    if ((r = reg_write(MII_ADDR, (PHY_ADDR << 11) | ((u32)reg << 6) | MII_WRITE | MII_BUSY))) return r;
    return reg_wait(MII_ADDR, MII_BUSY, 100);
}

void eth_reset(void) { ed = 0; rx_len = rx_off = 0; stage = ""; chip_id = 0; }

int eth_present(void) { return ed != 0; }
int eth_is(const struct usb_dev *d) { return ed && ed == d; }
const u8 *eth_mac(void) { return mac; }
u32 eth_chip(void) { return chip_id; }

/* 0 = initialized fine; otherwise the failed step for the screen */
const char *eth_error(int *code, u32 *hcint)
{
    if (code) *code = err_code;
    if (hcint) *hcint = err_hcint;
    return stage[0] ? stage : 0;
}

int eth_attach(struct usb_dev *d, const u8 *cfg, int len)
{
    int r;
    u32 v;
    u16 p;

    if (ed || d->vid != 0x0424 || d->pid != 0xEC00) return 0;
    ep_in = ep_out = 0;
    for (int i = 0; i + 1 < len && cfg[i]; i += cfg[i])
        if (cfg[i + 1] == 5 && i + 3 < len && (cfg[i + 3] & 3) == EP_BULK) {
            if (cfg[i + 2] & 0x80) { if (!ep_in) ep_in = cfg[i + 2] & 0xF; }
            else if (!ep_out) ep_out = cfg[i + 2] & 0xF;
        }
    ed = d;
    pid_in = pid_out = PID_DATA0;
    rx_len = rx_off = 0;
    memset(&eth_stat, 0, sizeof(eth_stat));
    stage = ""; err_code = 0; err_hcint = 0;
    if (!ep_in || !ep_out) return fail("no bulk endpoints", 0);

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

/* 0 = no link, else 10 or 100; *full = full duplex. Also tells the MAC the duplex. */
int eth_link(int *full)
{
    u16 s = 0, sp = 0;
    u32 v;
    if (!ed || stage[0]) return 0;
    phy_read(1, &s);                         /* link bit is latched: read twice */
    if (phy_read(1, &s) || !(s & 0x0004)) return 0;
    if (phy_read(31, &sp)) return 0;         /* PHY special control/status */
    int fd = (sp & 0x10) != 0;
    if (full) *full = fd;
    if (!reg_read(MAC_CR, &v)) {
        u32 n = fd ? (v | MAC_FDPX) & ~MAC_RCVOWN : (v & ~MAC_FDPX) | MAC_RCVOWN;
        if (n != v) reg_write(MAC_CR, n);
    }
    return (sp & 0x08) ? 100 : 10;
}

int eth_send(const void *frame, int len)
{
    if (!ed || stage[0] || len < 14 || len > 1514) return -1;
    u32 a = TX_FIRST_SEG | TX_LAST_SEG | (u32)len, b = (u32)len;
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
    for (int pass = 0; pass < 2; pass++) {
        while (rx_off + 4 <= rx_len) {
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
