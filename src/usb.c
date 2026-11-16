/*
 * USB host driver for the Synopsys DesignWare OTG controller (DWC2) in the BCM2837.
 *
 * Simplifications: no interrupts (polling), one channel, Buffer DMA,
 * every packet is sent on its own (because of split transactions).
 *
 * On the RPi3 the root port always has the HS hub LAN9514 (+ Ethernet) behind it.
 * Keyboards are LS/FS, so talking to them goes through split transactions
 * (start-split / complete-split) handled by the hub's Transaction Translator.
 */
#include "usb.h"

#define USB_BASE      (MMIO_BASE + 0x980000)
#define R(off)        (USB_BASE + (off))

#define GOTGCTL   R(0x000)
#define GAHBCFG   R(0x008)
#define GUSBCFG   R(0x00C)
#define GRSTCTL   R(0x010)
#define GINTSTS   R(0x014)
#define GINTMSK   R(0x018)
#define GRXFSIZ   R(0x024)
#define GNPTXFSIZ R(0x028)
#define GSNPSID   R(0x040)
#define GHWCFG2   R(0x048)
#define HPTXFSIZ  R(0x100)
#define HCFG      R(0x400)
#define HFNUM     R(0x408)
#define HAINT     R(0x414)
#define HAINTMSK  R(0x418)
#define HPRT      R(0x440)
#define PCGCCTL   R(0xE00)

#define CH        0                       /* we use channel 0 */
#define HCCHAR    R(0x500 + CH * 0x20)
#define HCSPLT    R(0x504 + CH * 0x20)
#define HCINT     R(0x508 + CH * 0x20)
#define HCINTMSK  R(0x50C + CH * 0x20)
#define HCTSIZ    R(0x510 + CH * 0x20)
#define HCDMA     R(0x514 + CH * 0x20)

/* HPRT bity */
#define HPRT_CONN      (1 << 0)
#define HPRT_CONN_CHG  (1 << 1)
#define HPRT_ENA       (1 << 2)
#define HPRT_ENA_CHG   (1 << 3)
#define HPRT_OC_CHG    (1 << 5)
#define HPRT_RST       (1 << 8)
#define HPRT_PWR       (1 << 12)
#define HPRT_W1C       (HPRT_CONN_CHG | HPRT_ENA | HPRT_ENA_CHG | HPRT_OC_CHG)

/* HCINT bity */
#define HCINT_XFERCOMPL (1 << 0)
#define HCINT_CHHLTD    (1 << 1)
#define HCINT_AHBERR    (1 << 2)
#define HCINT_STALL     (1 << 3)
#define HCINT_NAK       (1 << 4)
#define HCINT_ACK       (1 << 5)
#define HCINT_NYET      (1 << 6)
#define HCINT_XACTERR   (1 << 7)
#define HCINT_BBLERR    (1 << 8)
#define HCINT_FRMOVRUN  (1 << 9)
#define HCINT_DTERR     (1 << 10)

/* DMA buffer in a non-cacheable region (see linker.ld and mmu.c) */
extern char __dma_start[];
#define DMA_BUF   ((u8 *)__dma_start)
#define BUS_ADDR(p) ((u32)(uintptr_t)(p) | 0xC0000000)

struct usb_dev usb_devs[USB_MAX_DEVS];
struct usb_fail usb_fails[USB_MAX_FAILS];
int usb_nfails;
int usb_debug;
static u32 last_hcint;
static u8  last_stage;      /* 1=SETUP 2=DATA 3=STATUS */
u32 usb_last_hcint(void) { return last_hcint; }

static void fail(struct usb_dev *parent, u8 port, u8 speed, int code, const char *what)
{
    if (usb_nfails >= USB_MAX_FAILS) return;
    struct usb_fail *f = &usb_fails[usb_nfails++];
    f->hub = parent ? parent->addr : 0;
    f->port = port; f->speed = speed; f->code = code;
    f->hcint = last_hcint; f->stage = last_stage;
    int i = 0;
    for (; what[i] && i < 23; i++) f->what[i] = what[i];
    f->what[i] = 0;
}
static int next_addr = 1;

const char *usb_speed_name(int s)
{
    return s == USB_SPEED_HS ? "HS 480M" : s == USB_SPEED_FS ? "FS 12M" : "LS 1.5M";
}

#define DBG(...) do { if (usb_debug) kprintf(__VA_ARGS__); } while (0)

/* ======================= jadro DWC2 ======================= */

static int wait_clear(u64 reg, u32 mask, u32 ms)
{
    u64 end = timer_us() + ms * 1000;
    while (mmio_read(reg) & mask)
        if (timer_us() > end) return 0;
    return 1;
}

static int core_reset(void)
{
    u64 end = timer_us() + 100000;
    while (!(mmio_read(GRSTCTL) & (1u << 31)))      /* AHB idle */
        if (timer_us() > end) return 0;
    mmio_write(GRSTCTL, 1);                          /* soft reset */
    if (!wait_clear(GRSTCTL, 1, 100)) return 0;
    delay_us(100000);
    return 1;
}

static void flush_fifos(void)
{
    mmio_write(GRSTCTL, (0x10 << 6) | (1 << 5));     /* all TX FIFOs */
    wait_clear(GRSTCTL, 1 << 5, 10);
    mmio_write(GRSTCTL, 1 << 4);                     /* RX FIFO */
    wait_clear(GRSTCTL, 1 << 4, 10);
}

static int power_on(void)
{
    u32 v[2] = { 3 /* USB HCD */, 3 /* on + wait */ };
    if (!mbox_prop(0x00028001, v, 2)) return 0;
    return v[1] & 1;
}

static int core_init(void)
{
    u32 cfg;

    if (!power_on()) { kprintf("USB: power-on failed\n"); return 0; }
    DBG("USB: core id %08x\n", mmio_read(GSNPSID));

    mmio_write(GAHBCFG, mmio_read(GAHBCFG) & ~1u);   /* global IRQ off */

    cfg = mmio_read(GUSBCFG);
    cfg &= ~((1u << 20) | (1u << 22));               /* ULPI ext vbus, term sel */
    mmio_write(GUSBCFG, cfg);
    if (!core_reset()) { kprintf("USB: core reset failed\n"); return 0; }

    cfg = mmio_read(GUSBCFG);
    cfg &= ~((1u << 4) | (1u << 3));                 /* UTMI+, 8 bit */
    u32 hw2 = mmio_read(GHWCFG2);
    if (((hw2 >> 6) & 3) == 2 && ((hw2 >> 8) & 3) == 1)
        cfg |= (1u << 17) | (1u << 19);              /* ULPI FS/LS */
    else
        cfg &= ~((1u << 17) | (1u << 19));
    cfg &= ~((1u << 8) | (1u << 9));                 /* no SRP/HNP */
    cfg &= ~(1u << 30);
    cfg |= (1u << 29);                               /* force host mode */
    mmio_write(GUSBCFG, cfg);
    delay_us(50000);

    u32 ahb = mmio_read(GAHBCFG);
    ahb |= (1u << 5);                                /* DMA */
    ahb |= (1u << 4);                                /* BCM: wait for AXI writes */
    ahb &= ~(3u << 1);
    mmio_write(GAHBCFG, ahb);

    /* host mode */
    mmio_write(PCGCCTL, 0);
    u32 hcfg = mmio_read(HCFG) & ~3u;
    if (((hw2 >> 6) & 3) == 2 && ((hw2 >> 8) & 3) == 1 && (cfg & (1u << 17)))
        hcfg |= 1;                                   /* 48 MHz */
    mmio_write(HCFG, hcfg);

    mmio_write(GRXFSIZ, 1024);
    mmio_write(GNPTXFSIZ, (1024 << 16) | 1024);
    mmio_write(HPTXFSIZ, (1024 << 16) | 2048);
    flush_fifos();

    mmio_write(GINTMSK, 0);
    mmio_write(GINTSTS, 0xFFFFFFFF);
    mmio_write(HAINTMSK, 0);
    return 1;
}

static int root_port_reset(int *speed)
{
    u32 p = mmio_read(HPRT);
    if (!(p & HPRT_PWR)) {
        mmio_write(HPRT, (p & ~HPRT_W1C) | HPRT_PWR);
        delay_us(100000);
    }
    /* wait for a connection (max 1 s) */
    u64 end = timer_us() + 1000000;
    while (!(mmio_read(HPRT) & HPRT_CONN))
        if (timer_us() > end) return 0;
    delay_us(100000);

    p = mmio_read(HPRT) & ~HPRT_W1C;
    mmio_write(HPRT, p | HPRT_RST);
    delay_us(60000);
    mmio_write(HPRT, p & ~HPRT_RST);
    delay_us(20000);

    p = mmio_read(HPRT);
    mmio_write(HPRT, (p & ~HPRT_W1C) | (p & (HPRT_CONN_CHG | HPRT_ENA_CHG | HPRT_OC_CHG)));
    *speed = (p >> 17) & 3;
    DBG("USB: root port HPRT=%08x\n", p);
    return (p & HPRT_ENA) != 0;
}

/* ======================= single packet transfer ======================= */

static u32 run_channel(u32 hcchar, u32 hctsiz, u32 hcsplt, u32 ms)
{
    if (hcchar & (1u << 29)) {                   /* periodic: next (micro)frame */
        hcchar &= ~(1u << 29);
        if (!(mmio_read(HFNUM) & 1)) hcchar |= (1u << 29);
    }
    mmio_write(HCINT, 0xFFFFFFFF);
    mmio_write(HCINTMSK, 0x7FF);
    mmio_write(HCSPLT, hcsplt);
    mmio_write(HCTSIZ, hctsiz);
    mmio_write(HCDMA, BUS_ADDR(DMA_BUF));
    mmio_write(HCCHAR, hcchar | (1u << 31));     /* CHEN */

    u64 end = timer_us() + ms * 1000;
    u32 st;
    while (!((st = mmio_read(HCINT)) & HCINT_CHHLTD)) {
        if (timer_us() > end) {                  /* halt the channel */
            last_hcint = st;
            mmio_write(HCCHAR, mmio_read(HCCHAR) | (1u << 30) | (1u << 31));
            wait_clear(HCCHAR, 1u << 31, 10);
            return 0;
        }
    }
    last_hcint = st;
    return st;
}

/*
 * Periodic (interrupt) transfer through a Transaction Translator.
 * The USB 2.0 spec (11.18) requires exact microframe (125 us) timing:
 *   start-split in microframe N, complete-split in N+2, N+3 (and N+4).
 * Otherwise the TT drops the data and answers NYET/error. Scheme as in Circle/USPi.
 */
static int wait_uframe(u32 target)
{
    u64 end = timer_us() + 3000;
    while ((mmio_read(HFNUM) & 7) != target)
        if (timer_us() > end) return 0;
    return 1;
}

static int periodic_split(u32 hcchar, u32 hctsiz, u32 hcsplt, int in, u16 len, u16 *actual)
{
    u32 st;
    for (int attempt = 0; attempt < 3; attempt++) {
        u32 next = (mmio_read(HFNUM) + 1) & 7;
        if (next == 6) next = 7;
        if (!wait_uframe(next)) return USB_TIMEOUT;
        st = run_channel(hcchar, hctsiz, hcsplt, 20);
        if (!st) return USB_TIMEOUT;
        if (st & HCINT_STALL) return USB_STALL;
        if (st & HCINT_NAK)   return USB_NAK;
        if (!(st & HCINT_ACK)) { DBG("USB: per. SSPLIT HCINT=%x\n", st); continue; }

        int tries = (next != 5) ? 3 : 2;
        next = (next + 2) & 7;
        while (tries-- > 0) {
            if (!wait_uframe(next)) return USB_TIMEOUT;
            st = run_channel(hcchar, hctsiz, hcsplt | (1u << 16), 20);
            if (!st) return USB_TIMEOUT;
            if (st & HCINT_XFERCOMPL) {
                if (actual) *actual = in ? (u16)(len - (mmio_read(HCTSIZ) & 0x7FFFF)) : len;
                return USB_OK;
            }
            if (st & HCINT_STALL) return USB_STALL;
            if (st & HCINT_NAK)   return USB_NAK;
            if (st & HCINT_NYET) { next = (next + 1) & 7; continue; }
            DBG("USB: per. CSPLIT HCINT=%x\n", st);
            break;
        }
    }
    /* TT did not make it - for a keyboard that just means "nothing yet" */
    return USB_NAK;
}

/*
 * Send/receive one packet (max. MPS bytes) via DMA_BUF.
 * For IN, the number of received bytes is returned in *actual.
 */
static int xfer_packet(struct usb_dev *d, u8 ep, u8 type, int in, u8 pid,
                       u16 mps, u16 len, u16 *actual)
{
    int periodic = (type == EP_INTERRUPT);
    int split = d->tt_hub != 0;
    u32 hcchar = (mps & 0x7FF) | ((u32)(ep & 0xF) << 11) | (in ? (1u << 15) : 0)
               | (d->speed == USB_SPEED_LS ? (1u << 17) : 0)
               | ((u32)type << 18) | (1u << 20) | ((u32)d->addr << 22)
               | (periodic ? (1u << 29) : 0);
    u32 hctsiz = len | (1u << 19) | ((u32)pid << 29);
    u32 hcsplt = 0, st;
    int tries;

    if (split)
        hcsplt = (1u << 31) | (3u << 14) | ((u32)d->tt_hub << 7) | d->tt_port;
    if (split && periodic)
        return periodic_split(hcchar, hctsiz, hcsplt, in, len, actual);

    for (tries = 0; tries < 3; tries++) {
        st = run_channel(hcchar, hctsiz, hcsplt, 100);
        if (!st) return USB_TIMEOUT;
        /* In DMA mode the core retries NAKs on control/bulk transfers by itself,
         * so the NAK bit can be set even on a successful transfer -
         * that is why XFERCOMPL is checked first. */
        if (!split && (st & HCINT_XFERCOMPL)) goto done;
        if (st & HCINT_STALL) return USB_STALL;
        if (st & HCINT_NAK)   return USB_NAK;
        if (st & HCINT_AHBERR) { kprintf("USB: AHB/DMA error\n"); return USB_ERR; }

        if (!split) {
            DBG("USB: transfer error HCINT=%x\n", st);
            continue;                              /* XACTERR etc. -> retry */
        }

        /* start-split accepted -> complete-split */
        if (!(st & HCINT_ACK)) { DBG("USB: SSPLIT HCINT=%x\n", st); continue; }
        for (int c = 0; c < 200; c++) {
            st = run_channel(hcchar, hctsiz, hcsplt | (1u << 16), 100);
            if (!st) return USB_TIMEOUT;
            if (st & HCINT_XFERCOMPL) goto done;
            if (st & HCINT_STALL) return USB_STALL;
            if (st & HCINT_NAK)   return USB_NAK;
            if (st & HCINT_NYET) { delay_us(125); continue; }     /* TT has no data yet */
            if (st & HCINT_ACK)   goto done;                       /* OUT acknowledged */
            DBG("USB: CSPLIT HCINT=%x\n", st);
            break;                                 /* error -> new start-split */
        }
    }
    return USB_ERR;

done:
    if (actual) *actual = in ? (u16)(len - (mmio_read(HCTSIZ) & 0x7FFFF)) : len;
    return USB_OK;
}

/* retry on NAK until the timeout */
static int xfer_retry(struct usb_dev *d, u8 ep, u8 type, int in, u8 pid,
                      u16 mps, u16 len, u16 *actual, u32 ms)
{
    u64 end = timer_us() + ms * 1000;
    for (;;) {
        int r = xfer_packet(d, ep, type, in, pid, mps, len, actual);
        if ((r != USB_NAK && r != USB_TIMEOUT) || timer_us() > end) return r;
        delay_us(200);
    }
}

/* ======================= control prenos ======================= */

int usb_control(struct usb_dev *d, u8 reqtype, u8 req, u16 val, u16 idx, void *data, u16 len)
{
    u8 *setup = DMA_BUF;
    u16 got, total = 0;
    int in = reqtype & 0x80, r;
    u16 mps = d->mps0 ? d->mps0 : 8;

    setup[0] = reqtype; setup[1] = req;
    setup[2] = val & 0xFF; setup[3] = val >> 8;
    setup[4] = idx & 0xFF; setup[5] = idx >> 8;
    setup[6] = len & 0xFF; setup[7] = len >> 8;
    last_stage = 1;
    r = xfer_retry(d, 0, EP_CONTROL, 0, PID_SETUP, mps, 8, 0, 500);
    if (r) { DBG("USB: SETUP %02x/%02x failed %d\n", reqtype, req, r); return r; }

    u8 pid = PID_DATA1;
    last_stage = 2;
    while (total < len) {
        u16 chunk = len - total;
        if (chunk > mps) chunk = mps;
        if (!in) memcpy(DMA_BUF, (u8 *)data + total, chunk);
        r = xfer_retry(d, 0, EP_CONTROL, in, pid, mps, chunk, &got, 500);
        if (r) { DBG("USB: DATA failed %d\n", r); return r; }
        if (in) memcpy((u8 *)data + total, DMA_BUF, got);
        total += got;
        pid = (pid == PID_DATA1) ? PID_DATA0 : PID_DATA1;
        if (in && got < mps) break;                  /* short packet = end */
    }

    last_stage = 3;
    r = xfer_retry(d, 0, EP_CONTROL, !in || !len, PID_DATA1, mps, 0, 0, 500);
    if (r) { DBG("USB: STATUS failed %d\n", r); return r; }
    return total;
}

int usb_interrupt_in(struct usb_dev *d, u8 ep, u16 mps, u8 *pid, void *data, u16 len)
{
    u16 got = 0;
    if (len > mps) len = mps;
    int r = xfer_packet(d, ep, EP_INTERRUPT, 1, *pid, mps, len, &got);
    if (r) return r;
    *pid = (*pid == PID_DATA0) ? PID_DATA1 : PID_DATA0;
    memcpy(data, DMA_BUF, got);
    return got;
}

/*
 * Bulk transfer in one channel run: the core sends/receives all the packets
 * and keeps the data toggle; the next PID is read back from HCTSIZ.
 */
int usb_bulk(struct usb_dev *d, u8 ep, int in, u8 *pid, void *data, u32 len, u32 ms)
{
    if (d->tt_hub) return USB_ERR;                   /* bulk through a TT: not supported */
    u32 mps = d->speed == USB_SPEED_HS ? 512 : 64;
    u32 pkts = len ? (len + mps - 1) / mps : 1;
    u32 size = in ? pkts * mps : len;
    if (pkts > 128) return USB_ERR;
    u32 hcchar = mps | ((u32)(ep & 0xF) << 11) | (in ? (1u << 15) : 0)
               | (d->speed == USB_SPEED_LS ? (1u << 17) : 0)
               | ((u32)EP_BULK << 18) | (1u << 20) | ((u32)d->addr << 22);
    if (!in && len) memcpy(DMA_BUF, data, len);
    u32 st = run_channel(hcchar, size | (pkts << 19) | ((u32)*pid << 29), 0, ms);
    u32 tsz = mmio_read(HCTSIZ);
    *pid = (tsz >> 29) & 3;                          /* toggle for the next transfer */
    if (!st) return USB_TIMEOUT;
    if (st & HCINT_XFERCOMPL) {
        u32 got = in ? size - (tsz & 0x7FFFF) : len;
        if (in) memcpy(data, DMA_BUF, got);
        return (int)got;
    }
    if (st & HCINT_STALL) return USB_STALL;
    if (st & HCINT_NAK)   return USB_NAK;
    if (st & HCINT_AHBERR) kprintf("USB: AHB/DMA error\n");
    return USB_ERR;
}

/* ======================= enumeration ======================= */

static void get_string(struct usb_dev *d, u8 idx, char *out, int max)
{
    u8 buf[64];
    out[0] = 0;
    if (!idx) return;
    int n = usb_control(d, 0x80, 6, 0x0300 | idx, 0x0409, buf, sizeof(buf));
    if (n < 4) return;
    int j = 0;
    for (int i = 2; i + 1 < n && i < buf[0] && j < max - 1; i += 2)
        out[j++] = (buf[i] >= 32 && buf[i] < 127 && !buf[i + 1]) ? buf[i] : '?';
    while (j && out[j - 1] == ' ') j--;
    out[j] = 0;
}

static void hub_init(struct usb_dev *hub);
static void hub_port_attach(struct usb_dev *hub, int p);

static struct usb_dev *enumerate(u8 speed, struct usb_dev *parent, u8 port)
{
    struct usb_dev tmp, *d = 0;
    u8 desc[18], cfg[256];
    int i, r;

    memset(&tmp, 0, sizeof(tmp));
    tmp.speed = speed;
    tmp.mps0 = speed == USB_SPEED_LS ? 8 : 64;
    if (parent) {
        tmp.depth = parent->depth + 1;
        tmp.parent = parent->addr;
        tmp.port = port;
        if (parent->speed == USB_SPEED_HS && speed != USB_SPEED_HS) {
            tmp.tt_hub = parent->addr; tmp.tt_port = port;
        } else {
            tmp.tt_hub = parent->tt_hub; tmp.tt_port = parent->tt_port;
        }
    }

    /* 1. first 8 bytes of the descriptor at address 0 -> EP0 max packet size */
    tmp.mps0 = 8;
    for (i = 0; i < 4; i++) {
        r = usb_control(&tmp, 0x80, 6, 0x0100, 0, desc, 8);
        if (r >= 8) break;
        DBG("USB: port %u not responding (%d, HCINT=%x), retrying\n", port, r, last_hcint);
        delay_us(100000);
    }
    if (r < 8) { fail(parent, port, speed, r, "GET_DESCRIPTOR(8)"); return 0; }
    tmp.mps0 = desc[7] ? desc[7] : 8;

    /* 2. pridelenie adresy */
    for (i = 0; i < USB_MAX_DEVS; i++) if (!usb_devs[i].used) { d = &usb_devs[i]; break; }
    if (!d || next_addr > 127) return 0;
    if ((r = usb_control(&tmp, 0x00, 5, next_addr, 0, 0, 0)) < 0) {
        fail(parent, port, speed, r, "SET_ADDRESS"); return 0;
    }
    delay_us(10000);
    tmp.addr = next_addr++;
    tmp.used = 1;
    *d = tmp;

    /* 3. full device descriptor */
    if ((r = usb_control(d, 0x80, 6, 0x0100, 0, desc, 18)) < 18) {
        fail(parent, port, speed, r, "GET_DESCRIPTOR(18)");
        d->used = 0; return 0;
    }
    d->dev_class = desc[4];
    d->vid = desc[8] | (desc[9] << 8);
    d->pid = desc[10] | (desc[11] << 8);
    get_string(d, desc[15], d->name, sizeof(d->name));
    if (!d->name[0]) get_string(d, desc[14], d->name, sizeof(d->name));
    if (!d->name[0]) memcpy(d->name, "(no name)", 10);

    /* 4. configuration */
    r = usb_control(d, 0x80, 6, 0x0200, 0, cfg, 9);
    if (r < 9) { fail(parent, port, speed, r, "GET_CONFIG(9)"); return d; }
    int total = cfg[2] | (cfg[3] << 8);
    if (total > (int)sizeof(cfg)) total = sizeof(cfg);
    r = usb_control(d, 0x80, 6, 0x0200, 0, cfg, total);
    if (r < 9) { fail(parent, port, speed, r, "GET_CONFIG(full)"); return d; }
    if (r < total) fail(parent, port, speed, r, "GET_CONFIG short");
    int r2 = usb_control(d, 0x00, 9, cfg[5], 0, 0, 0);       /* SET_CONFIGURATION */
    if (r2 < 0) fail(parent, port, speed, r2, "SET_CONFIGURATION");

    for (i = 0; i + 1 < r && cfg[i]; i += cfg[i])             /* remember interfaces */
        if (cfg[i + 1] == 4 && d->nif < 4 && i + 7 < r) {
            d->ifc[d->nif][0] = cfg[i + 5];
            d->ifc[d->nif][1] = cfg[i + 6];
            d->ifc[d->nif][2] = cfg[i + 7];
            d->nif++;
        }

    DBG("USB: addr %u %04x:%04x class %02x %s %s\n", d->addr, d->vid, d->pid,
        d->dev_class, usb_speed_name(d->speed), d->name);

    if (d->dev_class == 9) {
        d->is_hub = 1;
        hub_init(d);
    } else if (!eth_attach(d, cfg, r)) {
        kbd_attach(d, cfg, r);
    }
    return d;
}

/* ======================= hub ======================= */

#define PORT_RESET        4
#define PORT_POWER        8
#define C_PORT_CONNECTION 16
#define C_PORT_RESET      20

static int port_status_r(struct usb_dev *hub, int p, u16 *st, u16 *chg, int *code)
{
    u8 s[4];
    int r = 0;
    for (int i = 0; i < 5; i++) {
        r = usb_control(hub, 0xA3, 0, 0, p, s, 4);
        if (r >= 4) {
            *st = s[0] | (s[1] << 8);
            *chg = s[2] | (s[3] << 8);
            return 1;
        }
        delay_us(20000);
    }
    if (code) *code = r;
    return 0;
}

static int port_status(struct usb_dev *hub, int p, u16 *st, u16 *chg)
{
    return port_status_r(hub, p, st, chg, 0);
}

static struct usb_dev *find_dev(u8 addr)
{
    for (int i = 0; i < USB_MAX_DEVS; i++)
        if (usb_devs[i].used && usb_devs[i].addr == addr) return &usb_devs[i];
    return 0;
}

/* hub stopped responding - find out why (alive? detached from parent?) */
static void hub_probe(struct usb_dev *hub)
{
    u8 b[2];
    int r = usb_control(hub, 0x80, 0, 0, 0, b, 2);          /* GET_STATUS zariadenia */
    fail(hub, 0, hub->speed, r, r >= 0 ? "hub alive" : "hub not responding");
    struct usb_dev *par = find_dev(hub->parent);
    u16 st, chg;
    int code = 0;
    if (par && port_status_r(par, hub->port, &st, &chg, &code)) {
        last_hcint = 0; last_stage = 0;
        fail(par, hub->port, 0, st | (chg << 16), "parent port st|chg");
    } else if (par) {
        fail(par, hub->port, 0, code, "parent port ?");
    }
}

static void hub_init(struct usb_dev *hub)
{
    u8 hd[16];
    u16 st, chg;
    int probed = 0;
    if (usb_control(hub, 0xA0, 6, 0x2900, 0, hd, sizeof(hd)) < 7) {
        fail(hub, 0, hub->speed, -1, "hub descriptor");
        return;
    }
    hub->nports = hd[2];
    u32 pgood = hd[5] * 2;
    DBG("USB: hub addr %u, %u ports\n", hub->addr, hub->nports);

    /* check: does the hub answer GET_STATUS before the ports are powered? */
    {
        u16 st0, chg0; int c0 = 0;
        if (!port_status_r(hub, 1, &st0, &chg0, &c0))
            fail(hub, 1, 0, c0, "STATUS before PWR");
    }
    /* Power ports one at a time: powering all at once (inrush current) knocked
     * the hub in an Apple keyboard off the bus - it lost its address. */
    for (int p = 1; p <= hub->nports; p++) {
        int r = usb_control(hub, 0x23, 3, PORT_POWER, p, 0, 0);
        if (r < 0) fail(hub, p, 0, r, "PORT_POWER");
        delay_us((pgood + 100) * 1000);
    }
    /* some devices (gaming keyboards) attach later */
    delay_us(400000);

    for (int p = 1; p <= hub->nports; p++) {
        int code = 0;
        if (!port_status_r(hub, p, &st, &chg, &code)) {
            fail(hub, p, 0, code, "PORT_STATUS");
            if (!probed) { probed = 1; hub_probe(hub); }
            hub->dead = 1;
            continue;
        }
        if (p <= 8) hub->pst[p - 1] = st;
        DBG("USB: hub %u port %u status %04x change %04x\n", hub->addr, p, st, chg);
        if (st & 1) hub_port_attach(hub, p);
    }
}

/* reset a hub port and enumerate the device on it */
static void hub_port_attach(struct usb_dev *hub, int p)
{
    u16 st = 0, chg;
    if (p <= 8) hub->tries[p - 1]++;
    delay_us(100000);                                        /* debounce */
    usb_control(hub, 0x23, 1, C_PORT_CONNECTION, p, 0, 0);
    usb_control(hub, 0x23, 3, PORT_RESET, p, 0, 0);
    u64 end = timer_us() + 500000;
    do {
        delay_us(10000);
        if (!port_status(hub, p, &st, &chg)) break;
    } while ((st & (1 << 4)) && timer_us() < end);
    usb_control(hub, 0x23, 1, C_PORT_RESET, p, 0, 0);
    if (p <= 8) hub->pst[p - 1] = st;
    if (!(st & 2)) { fail(hub, p, 0, st, "port not enabled"); return; }
    delay_us(50000);
    u8 speed = (st & (1 << 9)) ? USB_SPEED_LS : (st & (1 << 10)) ? USB_SPEED_HS : USB_SPEED_FS;
    enumerate(speed, hub, p);
}

/* ======================= hot-plug ======================= */

static struct usb_dev *child_at(struct usb_dev *hub, int p)
{
    for (int i = 0; i < USB_MAX_DEVS; i++)
        if (usb_devs[i].used && usb_devs[i].parent == hub->addr && usb_devs[i].port == p)
            return &usb_devs[i];
    return 0;
}

static void remove_dev(struct usb_dev *d)
{
    for (int i = 0; i < USB_MAX_DEVS; i++)                   /* najprv deti */
        if (usb_devs[i].used && usb_devs[i].parent == d->addr && &usb_devs[i] != d)
            remove_dev(&usb_devs[i]);
    if (kbd_is(d)) kbd_reset();
    if (eth_is(d)) eth_reset();
    con_color(COL_GRAY); kprintf("\n[USB] disconnected: %s\n", d->name); con_color(COL_DEFAULT);
    d->used = 0;
}

static void announce(struct usb_dev *c, int kbd_before, int hub, int port)
{
    con_color(COL_GRAY);
    if (c) {
        kprintf("\n[USB] connected: %s\n", c->name);
        if (usb_debug) {
            kprintf("  %04x:%04x %s", c->vid, c->pid, usb_speed_name(c->speed));
            for (int k = 0; k < c->nif; k++)
                kprintf(" if%u:%02x/%02x/%02x", k, c->ifc[k][0], c->ifc[k][1], c->ifc[k][2]);
            con_putc('\n');
            for (int f = 0; f < usb_nfails; f++)
                kprintf("  %s -> %d (stage %u, HCINT %x)\n", usb_fails[f].what,
                        usb_fails[f].code, usb_fails[f].stage, usb_fails[f].hcint);
        }
    } else if (usb_debug) {
        kprintf("\n[USB] port %u.%u: device not responding\n", hub, port);
    }
    con_color(COL_DEFAULT);
    if (!kbd_before && kbd_present()) {
        con_color(COL_GREEN); kprintf("[USB] keyboard ready\n"); con_color(COL_DEFAULT);
    }
}

static int core_ok, root_tries;

/* root port: on the Zero 2 W / 3A+ the keyboard (or a hub) sits right on it */
static void root_hotplug(void)
{
    if (!core_ok) return;
    u32 p = mmio_read(HPRT);
    struct usb_dev *root = 0;
    for (int i = 0; i < USB_MAX_DEVS; i++)
        if (usb_devs[i].used && usb_devs[i].parent == 0) { root = &usb_devs[i]; break; }
    if (p & HPRT_CONN_CHG) {
        mmio_write(HPRT, (p & ~HPRT_W1C) | HPRT_CONN_CHG);
        if (root) { remove_dev(root); root = 0; }
        root_tries = 0;
    }
    if (!(p & HPRT_CONN)) {
        if (root) remove_dev(root);
        return;
    }
    if (!root && root_tries < 3) {
        int speed, before = kbd_present();
        root_tries++;
        usb_nfails = 0;
        if (!root_port_reset(&speed)) return;
        next_addr = 1;
        enumerate(speed, 0, 0);
        for (int i = 0; i < USB_MAX_DEVS; i++)
            if (usb_devs[i].used && usb_devs[i].parent == 0) { root = &usb_devs[i]; break; }
        announce(root, before, 0, 0);
    }
}

void usb_hotplug(void)
{
    static u64 next;
    u64 now = timer_us();
    if (now < next) return;
    next = now + 700000;
    root_hotplug();

    for (int i = 0; i < USB_MAX_DEVS; i++) {
        struct usb_dev *hub = &usb_devs[i];
        if (!hub->used || !hub->is_hub || hub->dead) continue;
        for (int p = 1; p <= hub->nports && p <= 8; p++) {
            u8 s[4];
            if (usb_control(hub, 0xA3, 0, 0, p, s, 4) < 4) { hub->dead = 1; break; }
            u16 st = s[0] | (s[1] << 8), chg = s[2] | (s[3] << 8);
            hub->pst[p - 1] = st;
            struct usb_dev *c = child_at(hub, p);
            if (chg & 1) {
                usb_control(hub, 0x23, 1, C_PORT_CONNECTION, p, 0, 0);
                hub->tries[p - 1] = 0;                       /* new connection */
                if (c) { remove_dev(c); c = 0; }
            }
            if (!(st & 1)) { if (c) remove_dev(c); continue; }
            if (!c && hub->tries[p - 1] < 3) {
                int before = kbd_present();
                usb_nfails = 0;
                hub_port_attach(hub, p);
                announce(child_at(hub, p), before, hub->addr, p);
            }
        }
    }
}

/* ======================= startup ======================= */

int usb_init(void)
{
    int speed, n = 0;
    memset(usb_devs, 0, sizeof(usb_devs));
    usb_nfails = 0;
    next_addr = 1;
    kbd_reset();
    eth_reset();

    core_ok = 0;
    if (!core_init()) return -1;
    core_ok = 1;
    root_tries = 0;
    if (!root_port_reset(&speed)) { DBG("USB: nothing connected\n"); return 0; }
    DBG("USB: root port %s\n", usb_speed_name(speed));
    enumerate(speed, 0, 0);
    for (int i = 0; i < USB_MAX_DEVS; i++) n += usb_devs[i].used;
    return n;
}

/* ======================= diagnostika ======================= */

static const char *code_name(int c)
{
    switch (c) {
    case USB_NAK: return "NAK"; case USB_STALL: return "STALL";
    case USB_ERR: return "error"; case USB_TIMEOUT: return "timeout";
    default: return "";
    }
}

void usb_print_tree(void)
{
    int any = 0;
    for (int i = 0; i < USB_MAX_DEVS; i++) {
        struct usb_dev *d = &usb_devs[i];
        if (!d->used) continue;
        any = 1;
        for (int j = 0; j < d->depth; j++) con_puts("  ");
        con_color(COL_YELLOW); kprintf("[%u] ", d->addr); con_color(COL_DEFAULT);
        kprintf("%04x:%04x %s %s", d->vid, d->pid, usb_speed_name(d->speed), d->name);
        if (d->is_hub) {
            kprintf(" (hub %u p.%s)", d->nports, d->dead ? ", NOT RESPONDING" : "");
            con_color(COL_GRAY); con_puts(" ports:");
            for (int p = 0; p < d->nports && p < 8; p++) kprintf(" %u:%x", p + 1, d->pst[p]);
            con_color(COL_DEFAULT);
        }
        con_color(COL_GRAY);
        if (d->parent) kprintf(" @%u.%u", d->parent, d->port);
        for (int k = 0; k < d->nif; k++)
            kprintf(" if%u:%02x/%02x/%02x", k, d->ifc[k][0], d->ifc[k][1], d->ifc[k][2]);
        con_color(COL_DEFAULT);
        con_putc('\n');
    }
    if (!any) kprintf("no USB devices\n");
    for (int i = 0; i < usb_nfails; i++) {
        struct usb_fail *f = &usb_fails[i];
        con_color(COL_RED);
        if (f->code > 0xFFFF || !strncmp(f->what, "parent port st", 14))
            kprintf("  hub %u port %u: %s = %x\n", f->hub, f->port, f->what, (u32)f->code);
        else
            kprintf("  hub %u port %u: %s -> %d %s (stage %u, HCINT %x)\n", f->hub, f->port,
                    f->what, f->code, code_name(f->code), f->stage, f->hcint);
        con_color(COL_DEFAULT);
    }
}
