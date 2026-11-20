/*
 * USB HID keyboard (boot protocol, US layout).
 * Report: [modifiers, reserved, k1..k6]. New keys = the ones that were
 * not in the previous report. Autorepeat is done in software.
 */
#include "usb.h"

static struct usb_dev *kd;
static u64 k_quiet;                /* ignore reports until then (just attached) */
static u8  k_iface, k_ep, k_mps, k_pid, k_interval;
static int k_use_getreport, k_errors;
static u8  prev[8];
static int caps;
static int num = 1;                /* Num Lock is on after boot, like on a PC */
static u64 last_poll;

/* key repeat */
static u8  rep_key, rep_mods;
static u64 rep_next;

/* character queue */
static char q[64];
static int  qh, qt;
/* filled on the I/O core, emptied on core 0: one writer, one reader */
static void push(char c)
{
    int h = qh, n = (h + 1) % 64;
    if (n == __atomic_load_n(&qt, __ATOMIC_ACQUIRE)) return;    /* full */
    q[h] = c;
    __atomic_store_n(&qh, n, __ATOMIC_RELEASE);
}

static const char lower[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\033\b\t -=[]\\#;'`,./";
static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\033\b\t _+{}|~:\"~<>?";

static void kbd_unlock(void);
void kbd_reset(void) { kd = 0; k_use_getreport = 0; k_errors = 0; rep_key = 0; qh = qt = 0; kbd_unlock(); }

int kbd_present(void) { return kd != 0; }
int kbd_is(const struct usb_dev *d) { return kd && kd == d; }
const char *kbd_name(void) { return kd ? kd->name : ""; }
const char *kbd_mode(void);

/* SET_REPORT (output): bit0 = Num Lock, bit1 = Caps Lock, bit2 = Scroll Lock */
static void kbd_leds(void)
{
    if (!kd) return;
    u8 led = (num ? 1 : 0) | (caps ? 2 : 0);
    usb_control(kd, 0x21, 0x09, 0x0200, k_iface, &led, 1);
}

int kbd_attach(struct usb_dev *d, const u8 *cfg, int len)
{
    int found = 0;
    u8 iface = 0;
    for (int i = 0; i + 1 < len && cfg[i]; i += cfg[i]) {
        if (cfg[i + 1] == 4) {                               /* interface */
            found = cfg[i + 5] == 3 && cfg[i + 6] == 1 && cfg[i + 7] == 1;
            iface = cfg[i + 2];
        } else if (cfg[i + 1] == 5 && found && !kd) {        /* endpoint */
            if ((cfg[i + 2] & 0x80) && (cfg[i + 3] & 3) == 3) {
                kd = d;
                k_quiet = timer_us() + 300000;       /* the first reports may be junk */
                k_iface = iface;
                k_ep = cfg[i + 2] & 0xF;
                k_mps = cfg[i + 4];
                k_interval = cfg[i + 6];
                k_pid = PID_DATA0;
            }
        }
    }
    if (!kd || kd != d) return 0;

    /* Like Windows/Linux: fetch the HID report descriptor. Some keyboards
     * (gaming ones especially) won't send data or light up without it. */
    {
        static u8 rd[512];
        u16 rlen = 0;
        for (int i = 0; i + 1 < len && cfg[i]; i += cfg[i])
            if (cfg[i + 1] == 0x21 && i + 8 < len && !rlen)   /* HID deskriptor */
                rlen = cfg[i + 7] | (cfg[i + 8] << 8);
        if (!rlen || rlen > sizeof(rd)) rlen = sizeof(rd);
        usb_control(d, 0x81, 6, 0x2200, k_iface, rd, rlen);
    }
    usb_control(d, 0x21, 0x0B, 0, k_iface, 0, 0);            /* SET_PROTOCOL boot */
    usb_control(d, 0x21, 0x0A, 0, k_iface, 0, 0);            /* SET_IDLE 0 */
    kbd_leds();
    memset(prev, 0, sizeof(prev));

    /* A keyboard behind a HS hub (always on the RPi3) needs periodic split
     * transactions with exact microframe timing. Asking via the control
     * endpoint (GET_REPORT) is a safe fallback - it already works for enumeration. */
    k_use_getreport = 0;
    k_errors = 0;
    if (d->tt_hub) {
        u8 r[8];
        if (usb_control(d, 0xA1, 0x01, 0x0100, k_iface, r, 8) >= 0)
            k_use_getreport = 1;
    }
    return 1;
}

static void emit(u8 key, u8 mods)
{
    int shift = (mods & 0x22) != 0, ctrl = (mods & 0x11) != 0;
    char c = 0;

    switch (key) {
    case 0x39: caps = !caps; kbd_leds(); return;             /* Caps Lock */
    case 0x53: num = !num;   kbd_leds(); return;             /* Num Lock */
    case 0x29: return;                                       /* ignore Esc */
    case 0x52: push(27); push('['); push('A'); return;       /* up */
    case 0x51: push(27); push('['); push('B'); return;       /* down */
    case 0x4F: push(27); push('['); push('C'); return;       /* right */
    case 0x50: push(27); push('['); push('D'); return;       /* left */
    case 0x4A: push(27); push('['); push('H'); return;       /* Home */
    case 0x4D: push(27); push('['); push('F'); return;       /* End */
    case 0x4C: push(27); push('['); push('3'); push('~'); return;   /* Delete */
    case 0x4B: push(27); push('['); push('5'); push('~'); return;   /* Page Up */
    case 0x4E: push(27); push('['); push('6'); push('~'); return;   /* Page Down */
    }
    if (key >= 0x54 && key <= 0x63) {                       /* numeric keypad */
        static const char kp[] = "/*-+\n1234567890.";
        if (key <= 0x58 || num) {
            c = kp[key - 0x54];
        } else switch (key) {                                /* Num Lock off: navigation */
            case 0x60: push(27); push('['); push('A'); return;   /* KP8 = up */
            case 0x5A: push(27); push('['); push('B'); return;   /* KP2 = down */
            case 0x5C: push(27); push('['); push('D'); return;   /* KP4 = left */
            case 0x5E: push(27); push('['); push('C'); return;   /* KP6 = right */
            case 0x5F: push(27); push('['); push('H'); return;   /* KP7 = Home */
            case 0x59: push(27); push('['); push('F'); return;   /* KP1 = End */
            case 0x61: push(27); push('['); push('5'); push('~'); return;   /* KP9 = Page Up */
            case 0x5B: push(27); push('['); push('6'); push('~'); return;   /* KP3 = Page Down */
            case 0x63: push(27); push('['); push('3'); push('~'); return;   /* KP. = Delete */
            default:   return;
        }
    }
    if (!c && key >= 0x04 && key <= 0x38) {
        int i = key - 0x04;
        int up = shift;
        if (key <= 0x1D && caps) up = !up;
        c = up ? upper[i] : lower[i];
    }
    if (!c) return;
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c = (c & 0x1F);                                      /* Ctrl+C = 3, Ctrl+L = 12 */
    push(c);
}

static void handle_report(const u8 *r)
{
    for (int i = 2; i < 8; i++) {
        u8 k = r[i];
        if (k < 4) continue;                                 /* 0 = none, 1 = rollover */
        int was = 0;
        for (int j = 2; j < 8; j++) if (prev[j] == k) was = 1;
        if (!was) {
            emit(k, r[0]);
            rep_key = k; rep_mods = r[0];
            rep_next = timer_us() + 500000;
        }
    }
    /* repeated key released? */
    int held = 0;
    for (int j = 2; j < 8; j++) if (r[j] == rep_key) held = 1;
    if (!held) rep_key = 0;
    rep_mods = r[0];
    memcpy(prev, r, 8);
}

/* a boot-protocol report (8 bytes) from a Bluetooth keyboard (bt.c, core 1) */
void kbd_bt_report(const u8 *r)
{
    u8 b[8];
    memcpy(b, r, 8);
    handle_report(b);
}

/*
 * The read method is picked at runtime: until the keyboard sends the first
 * report with a key pressed, interrupt IN and GET_REPORT alternate. Whichever
 * delivers a key press first gets "locked in".
 */
static int k_lock;                 /* 0 = searching, 1 = INT, 2 = GET_REPORT */
static int k_turn, k_trace;

static void trace(const u8 *r, int n, char how)
{
    if (k_trace >= 4) return;
    k_trace++;
    con_color(COL_GRAY);
    kprintf("\n[KBD %c] %d B:", how, n);
    for (int i = 0; i < n && i < 16; i++) kprintf(" %02x", r[i]);
    con_putc('\n');
    con_color(COL_DEFAULT);
}

void kbd_poll(void)
{
    u8 r[16];
    int n, use_int;
    u64 now = timer_us();
    if (!kd) {                                               /* no USB keyboard: autorepeat for a Bluetooth one */
        if (rep_key && now >= rep_next) { emit(rep_key, rep_mods); rep_next = now + 50000; }
        return;
    }

    /* nobody polled for a while (a long command, decoding a picture): the key may
     * have been let go meanwhile - repeat only after a fresh report says it is held */
    if (rep_key && now - last_poll > 100000 && rep_next < now + 50000) rep_next = now + 50000;
    if (rep_key && now >= rep_next) {                        /* autorepeat */
        emit(rep_key, rep_mods);
        rep_next = now + 50000;
    }
    if (now - last_poll < 10000) return;                     /* at most every 10 ms */
    last_poll = now;

    use_int = k_lock ? (k_lock == 1) : (k_turn ^= 1);
    memset(r, 0, sizeof(r));
    if (use_int) {
        n = usb_interrupt_in(kd, k_ep, k_mps, &k_pid, r, sizeof(r));
        if (n == USB_NAK) return;                            /* no change */
    } else {
        n = usb_control(kd, 0xA1, 0x01, 0x0100, k_iface, r, 8);
    }
    if (n < 0) {
        if (k_lock && ++k_errors >= 20) { k_lock = 0; k_errors = 0; }
        return;
    }
    k_errors = 0;
    if (n < 3) return;
    if (now < k_quiet) return;                                   /* just attached: drop */

    int pressed = r[0] || r[2] || r[3];
    if (usb_debug && (pressed || k_lock)) trace(r, n, use_int ? 'I' : 'G');
    if (n == 9 && r[0] && r[0] < 8) {                        /* leading report ID */
        memmove(r, r + 1, 8);
    }
    if (!k_lock) {
        if (!pressed) return;                                /* ignore zero reports until locked */
        k_lock = use_int ? 1 : 2;
        k_use_getreport = !use_int;
    }
    handle_report(r);
}

int kbd_getc(void)
{
    int t = qt;
    if (t == __atomic_load_n(&qh, __ATOMIC_ACQUIRE)) return -1;
    char c = q[t];
    __atomic_store_n(&qt, (t + 1) % 64, __ATOMIC_RELEASE);
    return (u8)c;
}

/*
 * Keyboard test: alternates interrupt IN and GET_REPORT, shows live
 * statistics on screen and picks the method that works.
 */
static void hex8(const u8 *r) { for (int i = 0; i < 8; i++) kprintf("%02x ", r[i]); }

void kbd_selftest(u32 ms)
{
    if (!kd) return;
    u32 i_ok = 0, i_nak = 0, i_err = 0, i_hit = 0, g_ok = 0, g_err = 0, g_hit = 0;
    int i_last = 0, g_last = 0;
    u32 i_hc = 0, g_hc = 0;
    u8 r[8], last[8], src = '-';
    memset(last, 0, 8);
    u64 end = timer_us() + (u64)ms * 1000, next_print = 0;
    int turn = 0;

    con_color(COL_YELLOW);
    kprintf("  Keyboard test %u s - PRESS SOME KEYS (e.g. a, s, d)\n", ms / 1000);
    con_color(COL_DEFAULT);
    while (timer_us() < end) {
        memset(r, 0, 8);
        int n;
        if (turn ^= 1) {
            n = usb_interrupt_in(kd, k_ep, k_mps, &k_pid, r, 8);
            if (n == USB_NAK) i_nak++;
            else if (n < 0) { i_err++; i_last = n; i_hc = usb_last_hcint(); }
            else { i_ok++; if (n >= 3 && (r[0] | r[2])) { i_hit++; memcpy(last, r, 8); src = 'I'; } }
        } else {
            n = usb_control(kd, 0xA1, 0x01, 0x0100, k_iface, r, 8);
            if (n < 0) { g_err++; g_last = n; g_hc = usb_last_hcint(); }
            else { g_ok++; if (n >= 3 && (r[0] | r[2])) { g_hit++; memcpy(last, r, 8); src = 'G'; } }
        }
        delay_us(8000);
        if (timer_us() >= next_print) {
            next_print = timer_us() + 250000;
            kprintf("\r  INT ok%u nak%u err%u(%d/%x) hit%u | GR ok%u err%u(%d/%x) hit%u | %c ",
                    i_ok, i_nak, i_err, i_last, i_hc, i_hit, g_ok, g_err, g_last, g_hc, g_hit, src);
            hex8(last);
        }
    }
    con_putc('\n');

    k_lock = g_hit ? 2 : i_hit ? 1 : 0;                       /* otherwise picked at runtime */
    k_use_getreport = (k_lock == 2);
    k_errors = 0;
    memset(prev, 0, sizeof(prev));
    kprintf("  -> using %s%s\n", kbd_mode(),
            (g_hit || i_hit) ? "" : "  (no key press seen!)");
}

static void kbd_unlock(void) { k_lock = 0; k_trace = 0; }
const char *kbd_mode(void)
{
    return k_lock == 1 ? "interrupt IN" : k_lock == 2 ? "GET_REPORT (control)"
                                      : "auto (waiting for first key)";
}
