/*
 * Bluetooth Low Energy on the BT half of the WiFi/BT combo chip (CYW43430 on the Pi 3B, CYW43455
 * on the 3B+, CYW43436 on the Zero 2 W): HCI over the PL011 UART (GPIO32/33, H4 framing, 115200).
 * Switched on only when asked ('bt on' / 'bt scan', or 'bt auto on' = at boot): then the console
 * moves to the mini UART (uart.c). Patch RAM firmware: /FIRMWARE/<local name>.hcd (Cypress license).
 *
 * Start-up runs on core 0 (it reads the firmware file). After that the I/O core (core 1) owns the
 * UART: bt_poll() from io_poll_all() reads every byte, sends queued HCI commands and ACL packets.
 * Core 0 queues commands under the io lock and waits for their Command Complete.
 *
 * While on, the board
 *  - scans all the time (active, duplicates filtered, restarted every 15 s): 'bt scan' lists what
 *    is around; Xiaomi LYWSD03MMC thermometers with the ATC / pvvx firmware become sensors
 *    (MQTT 'ble/<mac>' + Home Assistant, GET /api/ble, 'bt sensors')
 *  - advertises as <board name> with the Nordic UART Service (NUS): a phone app (Serial Bluetooth
 *    Terminal, nRF Toolbox, Bluefruit Connect) connects and types commands: help, status, sensors,
 *    gpio <pin> [0|1|toggle], login <name> <password>. With users ('users add') a login is needed;
 *    'view' users may only read. The link is not encrypted (no pairing): use it nearby only.
 */
#include "kernel.h"
#include "sd.h"

static volatile int bt_up, bt_live;        /* live = core 1 owns the UART */
static int bt_flow;
static char bt_name[32], bt_err[64];
static u8 bt_addr[6];

static int err(const char *s) { int i = 0; for (; s[i] && i < 63; i++) bt_err[i] = s[i]; bt_err[i] = 0; return -1; }

/* BT_ON: firmware GPIO expander pin 0 (128) on the 3B / 3B+ / 3A+, GPIO42 on the Zero 2 W (assumed) */
static void bt_power(int on)
{
    if (board_type() == 0x12) { gpio_func(42, 1); gpio_set(42, on); return; }
    u32 v[2] = { 128, (u32)on };
    mbox_prop(0x00038041, v, 2);
}

/* ======================= HCI, synchronous (core 0, before core 1 takes over) ======================= */

static u8 ev[260];                         /* ev[0] event code, ev[1] length, ev[2..] parameters */

static void send_cmd(u16 op, const void *p, int n)
{
    uart_bt_putc(1);
    uart_bt_putc((u8)op); uart_bt_putc((u8)(op >> 8));
    uart_bt_putc((u8)n);
    for (int i = 0; i < n; i++) uart_bt_putc(((const u8 *)p)[i]);
}

static int read_packet(u32 us)
{
    int t = uart_bt_getc(us);
    if (t < 0) return -1;
    if (t == 4) {
        int c = uart_bt_getc(100000), l = uart_bt_getc(100000);
        if (c < 0 || l < 0) return -1;
        ev[0] = (u8)c; ev[1] = (u8)l;
        for (int i = 0; i < l; i++) { int b = uart_bt_getc(100000); if (b < 0) return -1; ev[2 + i] = (u8)b; }
        return 1;
    }
    if (t == 2) {
        u8 h[4];
        for (int i = 0; i < 4; i++) { int b = uart_bt_getc(100000); if (b < 0) return -1; h[i] = (u8)b; }
        for (int i = 0, n = h[2] | h[3] << 8; i < n; i++) if (uart_bt_getc(100000) < 0) return -1;
        return 0;
    }
    return 0;
}

/* a command and its Command Complete: its status (0 = ok), -1 = no answer; out = return parameters */
static int cmd(u16 op, const void *p, int n, u8 *out, int outmax, u32 ms)
{
    send_cmd(op, p, n);
    u64 end = timer_us() + (u64)ms * 1000;
    while (timer_us() < end) {
        if (read_packet(20000) != 1) continue;
        if (ev[0] == 0x0E && ev[1] >= 4 && (ev[3] | ev[4] << 8) == op) {
            int rl = ev[1] - 4;
            if (out) for (int i = 0; i < rl && i < outmax; i++) out[i] = ev[6 + i];
            return ev[5];
        }
        if (ev[0] == 0x0F && ev[1] >= 4 && (ev[4] | ev[5] << 8) == op && ev[2]) return ev[2];
    }
    return -1;
}

static int load_firmware(const char *name)
{
    char path[64] = "/FIRMWARE/";
    int k = 10;
    for (const char *s = name; *s && k < 50; s++) path[k++] = *s;
    for (const char *s = ".hcd"; *s; s++) path[k++] = *s;
    path[k] = 0;
    struct fat_file f;
    if (fat_open(path, &f) || f.is_dir) { char m[64] = "missing "; int j = 8; for (int i = 0; path[i] && j < 62; i++) m[j++] = path[i]; m[j] = 0; return err(m); }
    kprintf("  firmware %s (%u bytes)...\n", path, f.size);
    if (cmd(0xFC2E, 0, 0, 0, 0, 1000)) return err("'download minidriver' got no answer");
    delay_us(50000);
    static u8 rec[260];
    int n = 0;
    while (f.pos < f.size) {
        if (fat_read(&f, rec, 3) != 3) return err("firmware file cut short");
        u16 op = (u16)(rec[0] | rec[1] << 8);
        int l = rec[2];
        if (l && fat_read(&f, rec + 3, l) != l) return err("firmware file cut short");
        int st = cmd(op, rec + 3, l, 0, 0, 1000);
        if (st) return err(st < 0 ? "no answer while loading the firmware" : "the chip refused a firmware record");
        n++;
    }
    delay_us(250000);
    kprintf("  %d records loaded\n", n);
    return 0;
}

/* ======================= HCI, asynchronous (core 1 reads; anyone queues) ======================= */

#define CQ 12
static struct { u16 op; u8 n; u8 p[34]; } cq[CQ];
static int cq_h, cq_t, credits = 1;
static struct { u16 op; u8 st; u8 ret[8]; u32 seq; } done[8];   /* the last Command Completes (for a later 'wait') */
static u32 done_seq;
static int acl_credits = 4, acl_len = 27;

/* io lock held */
static int hci_queue(u16 op, const void *p, int n)
{
    int nx = (cq_h + 1) % CQ;
    if (nx == cq_t || n > 34) return -1;
    cq[cq_h].op = op; cq[cq_h].n = (u8)n;
    if (n) memcpy(cq[cq_h].p, p, (size_t)n);
    cq_h = nx;
    return 0;
}

/* ACL out (one L2CAP frame, fits one packet: ATT MTU 23) */
#define AQ 16
static struct { u8 n; u8 d[32]; } aq[AQ];
static int aq_h, aq_t;
static int conn = -1;                      /* connection handle of the phone, -1 = none */

static int acl_queue(u16 cid, const u8 *d, int n)
{
    int nx = (aq_h + 1) % AQ;
    if (nx == aq_t || n + 4 > 32 || conn < 0) return -1;
    u8 *b = aq[aq_h].d;
    b[0] = (u8)n; b[1] = (u8)(n >> 8); b[2] = (u8)cid; b[3] = (u8)(cid >> 8);
    memcpy(b + 4, d, (size_t)n);
    aq[aq_h].n = (u8)(n + 4);
    aq_h = nx;
    return 0;
}

/* ======================= the devices around (scan) ======================= */

#define MAXDEV 40
static struct dev {
    u8 addr[6], type, has_th, disc;
    u8 classic, kbd;                       /* classic = BR/EDR (inquiry); kbd 1 keyboard, 2 mouse, 3 combo, 4 other HID */
    u32 cod;                               /* class of device (classic) */
    int rssi, maker, temp, hum, batt;      /* temp 0.01 C, hum 0.01 %, batt % */
    u32 seq, sent_seq;                     /* new thermometer data / what MQTT sent */
    u64 seen, sent_at;
    char name[30];
} devs[MAXDEV];
static int ndev;

static const char *maker_name(int id)
{
    switch (id) {
    case 0x004C: return "Apple";      case 0x0006: return "Microsoft";  case 0x0075: return "Samsung";
    case 0x00E0: return "Google";     case 0x0059: return "Nordic";     case 0x038F: return "Xiaomi";
    case 0x0157: return "Huami";      case 0x02E5: return "Espressif";  case 0x0131: return "Cypress";
    case 0x000F: return "Broadcom";   case 0x0087: return "Garmin";     case 0x0499: return "Ruuvi";
    case 0x00C4: return "LG";         case 0x000D: return "TI";         case 0x0171: return "Amazon";
    case 0x0002: return "Intel";      case 0x001D: return "Qualcomm";   case 0x0046: return "MediaTek";
    case 0x0822: return "Adafruit";
    }
    return 0;
}

static void adv_data(struct dev *d, const u8 *p, int n)
{
    for (int i = 0; i + 1 < n; ) {
        int l = p[i], t = p[i + 1];
        if (l == 0 || i + 1 + l > n) break;
        const u8 *v = p + i + 2;
        int vl = l - 1;
        if (t == 0x09 || (t == 0x08 && !d->name[0])) {
            int k = 0;
            for (; k < vl && k < 29; k++) d->name[k] = (v[k] >= 32 && v[k] < 127) ? (char)v[k] : '?';
            d->name[k] = 0;
        } else if (t == 0xFF && vl >= 2) d->maker = v[0] | v[1] << 8;
        else if (t == 0x19 && vl >= 2) {                                   /* Appearance: 0x03C0 HID group */
            int ap = v[0] | v[1] << 8;
            if ((ap & 0xFFC0) == 0x03C0) d->kbd = ap == 0x03C1 ? 1 : ap == 0x03C2 ? 2 : 4;
        } else if ((t == 0x02 || t == 0x03) && !d->kbd) {                  /* 16-bit service UUIDs: 0x1812 = HID */
            for (int k = 0; k + 1 < vl; k += 2) if ((v[k] | v[k + 1] << 8) == 0x1812) d->kbd = 4;
        }
        else if (t == 0x16 && vl >= 2 && (v[0] | v[1] << 8) == 0x181A) {   /* Environmental Sensing service data */
            const u8 *s = v + 2;
            int sl = vl - 2, ok = 0;
            if (sl == 13) {                        /* ATC1441: MAC, temp BE 0.1 C, hum %, batt %, mV, count */
                d->temp = (short)(s[6] << 8 | s[7]) * 10; d->hum = s[8] * 100; d->batt = s[9]; ok = 1;
            } else if (sl >= 15) {                 /* pvvx: MAC, temp LE 0.01 C, hum LE 0.01 %, mV, batt % */
                d->temp = (short)(s[6] | s[7] << 8); d->hum = s[8] | s[9] << 8; d->batt = s[12]; ok = 1;
            }
            if (ok) { d->has_th = 1; d->seq++; }
        }
        i += 1 + l;
    }
}

static struct dev *dev_for(const u8 *a)
{
    struct dev *d = 0;
    for (int i = 0; i < ndev; i++) if (!memcmp_n(devs[i].addr, a, 6)) return &devs[i];
    if (ndev < MAXDEV) d = &devs[ndev++];
    else for (int i = 0; i < MAXDEV; i++) if (!devs[i].has_th && (!d || devs[i].seen < d->seen)) d = &devs[i];
    if (!d) return 0;
    memset(d, 0, sizeof(*d)); memcpy(d->addr, a, 6); d->maker = -1;
    return d;
}

static void adv_report(const u8 *e)            /* LE Meta 0x02, e = event (code, len, params) */
{
    int num = e[3];
    const u8 *p = e + 4, *end = e + 2 + e[1];
    for (int r = 0; r < num && p + 10 <= end; r++) {
        u8 type = p[1];
        const u8 *a = p + 2;
        int dl = p[8];
        if (p + 9 + dl + 1 > end) break;
        int rssi = (signed char)p[9 + dl];
        int fresh = ndev;
        struct dev *d = dev_for(a);                /* full: replaces the oldest that is no thermometer */
        if (!d) { p += 10 + dl; continue; }
        if (ndev != fresh || !d->seen) d->type = type;
        d->rssi = rssi;
        d->seen = timer_us();
        adv_data(d, p + 9, dl);
        p += 10 + dl;
    }
}

/* classic Bluetooth (BR/EDR) inquiry: e = event (code, len, params); 0x02 plain, 0x22 with RSSI, 0x2F extended */
static void inquiry_result(const u8 *e)
{
    int num = e[2];
    const u8 *p = e + 3, *end = e + 2 + e[1];
    for (int r = 0; r < num; r++) {
        const u8 *a, *c;
        int rssi = -127;
        if (e[0] == 0x02) { if (e + 3 + 14 * num > end) return; a = e + 3 + 6 * r; c = e + 3 + 9 * num + 3 * r; }
        else if (e[0] == 0x22) { if (p + 14 > end) return; a = p; c = p + 8; rssi = (signed char)p[13]; p += 14; }
        else { if (e + 3 + 14 > end) return; a = e + 3; c = e + 3 + 8; rssi = (signed char)e[3 + 13]; num = 1; }
        struct dev *d = dev_for(a);
        if (!d) continue;
        d->classic = 1;
        d->cod = (u32)(c[0] | c[1] << 8 | c[2] << 16);
        if (((d->cod >> 8) & 0x1F) == 0x05) {                      /* major class peripheral: keyboard / mouse */
            int m = (d->cod >> 6) & 3;
            d->kbd = m == 1 ? 1 : m == 2 ? 2 : m == 3 ? 3 : 4;
        }
        if (rssi != -127) d->rssi = rssi;
        d->seen = timer_us();
        if (e[0] == 0x2F) adv_data(d, e + 3 + 14, (int)(end - (e + 3 + 14)));   /* EIR: same format as advertising data */
    }
}

static void mac_str(char *o, const u8 *a)      /* a = little endian as in HCI */
{
    const char *h = "0123456789abcdef";
    for (int k = 0; k < 6; k++) { o[3 * k] = h[a[5 - k] >> 4]; o[3 * k + 1] = h[a[5 - k] & 15]; o[3 * k + 2] = k < 5 ? ':' : 0; }
}

/* ======================= GATT server: GAP + Nordic UART Service ======================= */

static const u8 U_PRIM[2] = { 0x00, 0x28 }, U_CHAR[2] = { 0x03, 0x28 }, U_CCCD[2] = { 0x02, 0x29 };
static const u8 U_GAP[2] = { 0x00, 0x18 }, U_NAME[2] = { 0x00, 0x2A };
static const u8 U_NUS[16] = { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E };
static const u8 U_RX[16]  = { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E };
static const u8 U_TX[16]  = { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E };
enum { H_GAP = 1, H_NAME_DECL, H_NAME, H_NUS, H_RX_DECL, H_RX, H_TX_DECL, H_TX, H_CCCD, H_LAST = H_CCCD };

static u8 decl_name[5] = { 0x02, H_NAME, 0, 0x00, 0x2A };
static u8 decl_rx[19] = { 0x0C, H_RX, 0 }, decl_tx[19] = { 0x10, H_TX, 0 };
static char gap_name[24];
static u8 cccd[2];

/* attribute h: its type (uuid) and value */
static int attr(int h, const u8 **type, int *tl, const u8 **val, int *vl)
{
    static u8 none[1];
    switch (h) {
    case H_GAP:       *type = U_PRIM; *tl = 2; *val = U_GAP; *vl = 2; return 1;
    case H_NAME_DECL: *type = U_CHAR; *tl = 2; *val = decl_name; *vl = 5; return 1;
    case H_NAME:      *type = U_NAME; *tl = 2; *val = (const u8 *)gap_name; *vl = (int)strlen(gap_name); return 1;
    case H_NUS:       *type = U_PRIM; *tl = 2; *val = U_NUS; *vl = 16; return 1;
    case H_RX_DECL:   *type = U_CHAR; *tl = 2; *val = decl_rx; *vl = 19; return 1;
    case H_RX:        *type = U_RX; *tl = 16; *val = none; *vl = 0; return 1;
    case H_TX_DECL:   *type = U_CHAR; *tl = 2; *val = decl_tx; *vl = 19; return 1;
    case H_TX:        *type = U_TX; *tl = 16; *val = none; *vl = 0; return 1;
    case H_CCCD:      *type = U_CCCD; *tl = 2; *val = cccd; *vl = 2; return 1;
    }
    return 0;
}

static int group_end(int h) { return h == H_GAP ? H_NAME : H_LAST; }

static void att_send(const u8 *d, int n) { acl_queue(4, d, n); }

static void att_error(u8 req, int h, u8 code)
{
    u8 r[5] = { 0x01, req, (u8)h, (u8)(h >> 8), code };
    att_send(r, 5);
}

/* ---- the phone's commands (core 1) ---- */
static char rx_line[96];
static int session_role;                    /* 0 none, 1 view, 2 admin (with users); without users: 2 */
static char session_user[24];
static char tx_ring[1024];
static int tx_h, tx_t;

static void out(const char *s)
{
    while (*s) {
        int nx = (tx_h + 1) % (int)sizeof(tx_ring);
        if (nx == tx_t) return;
        tx_ring[tx_h] = *s++;
        tx_h = nx;
    }
}
static void outn(long v) { char b[24]; int k = 0, neg = v < 0; if (neg) v = -v; do b[k++] = (char)('0' + v % 10); while (v /= 10); if (neg) out("-"); while (k) { char c[2] = { b[--k], 0 }; out(c); } }
static void out_c100(int v) { if (v < 0) { out("-"); v = -v; } outn(v / 100); out("."); outn(v % 100 / 10); }

static void phone_command(char *l)
{
    char *argv[4];
    int argc = 0;
    for (char *p = l; *p && argc < 4; ) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    if (!argc) return;
    int need = users_count() > 0;
    if (!strcmp(argv[0], "login")) {
        if (argc < 3) { out("login <name> <password>\n"); return; }
        char who[24];
        int r = users_check(argv[1], argv[2], "bluetooth", who, sizeof(who));
        if (r < 0) { out("too many wrong logins - wait a minute\n"); return; }
        if (!r) { out("wrong name or password\n"); return; }
        session_role = r;
        memcpy(session_user, who, sizeof(session_user));
        out("hello "); out(who); out(r == 2 ? " (admin)\n" : " (view: read only)\n");
        return;
    }
    if (!strcmp(argv[0], "logout")) { session_role = need ? 0 : 2; session_user[0] = 0; out("bye\n"); return; }
    if (!strcmp(argv[0], "help")) {
        out("RoboOS over Bluetooth: status, sensors, gpio <pin> [0|1|toggle]");
        out(need ? ", login <name> <password>, logout\n" : "\n");
        return;
    }
    if (need && !session_role) { out("log in first: login <name> <password>\n"); return; }
    if (!strcmp(argv[0], "status")) {
        out(net_name()); out(" - RoboOS "); out(os_version()); out(", "); out(board_name());
        out("\nup "); outn((long)(timer_us() / 1000000)); out(" s, IP "); out(ip_address_str());
        u64 now = clock_now();
        if (now) { char tb[32]; clock_format(tb, now, 0); out(", "); out(tb); }
        out("\n");
        return;
    }
    if (!strcmp(argv[0], "sensors")) {
        struct sensor_values v;
        sensors_read(&v);
        if (v.have & SV_CPU) { out("CPU "); out_c100(v.cpu_mc / 10); out(" C\n"); }
        if (v.have & SV_TEMP) { out("temp "); out_c100(v.temp_c100); out(" C\n"); }
        if (v.have & SV_HUM) { out("hum "); out_c100(v.hum_c100); out(" %\n"); }
        if (v.have & SV_PRESS) { out("press "); outn(v.press_pa / 100); out(" hPa\n"); }
        if (v.have & SV_AIN) for (int i = 0; i < 4; i++) { out("A"); outn(i); out(" "); outn(v.ain_mv[i]); out(" mV\n"); }
        for (int i = 0; i < ndev; i++) if (devs[i].has_th) {
            out(devs[i].name[0] ? devs[i].name : "BLE"); out(" "); out_c100(devs[i].temp); out(" C ");
            out_c100(devs[i].hum); out(" % batt "); outn(devs[i].batt); out(" %\n");
        }
        return;
    }
    if (!strcmp(argv[0], "gpio") && argc >= 2) {
        u64 pin;
        if (!parse_num(argv[1], &pin) || pin > 27) { out("pin 0-27\n"); return; }
        if (argc >= 3) {
            if (session_role < 2) { out("this user may only read\n"); return; }
            if (pin == 14 || pin == 15) { out("pins 14/15 are the UART\n"); return; }
            gpio_func((int)pin, 1);
            if (!strcmp(argv[2], "toggle")) gpio_set((int)pin, !gpio_get((int)pin));
            else gpio_set((int)pin, argv[2][0] == '1' || !strcmp(argv[2], "on"));
            kprintf("\n[BT] gpio %d = %d (%s)\n", (int)pin, gpio_get((int)pin), session_user[0] ? session_user : "phone");
        }
        out("GPIO"); outn((long)pin); out(" = "); outn(gpio_get((int)pin)); out("\n");
        return;
    }
    out("unknown command - try help\n");
}

static void nus_rx(const u8 *d, int n)
{
    int have_nl = 0;
    for (int i = 0; i < n; i++) if (d[i] == '\n' || d[i] == '\r') have_nl = 1;
    static int ln;
    for (int i = 0; i < n; i++) {
        char c = (char)d[i];
        if (c == '\n' || c == '\r') { if (ln) { rx_line[ln] = 0; phone_command(rx_line); ln = 0; } continue; }
        if (ln < (int)sizeof(rx_line) - 1) rx_line[ln++] = c;
    }
    if (!have_nl && ln) { rx_line[ln] = 0; phone_command(rx_line); ln = 0; }   /* nRF Connect sends no newline */
}

static void att(const u8 *p, int n)
{
    if (n < 1) return;
    u8 op = p[0], r[23];
    int s = n >= 3 ? (p[1] | p[2] << 8) : 0, e = n >= 5 ? (p[3] | p[4] << 8) : 0;
    const u8 *ty, *va;
    int tl, vl;
    switch (op) {
    case 0x02:                                             /* Exchange MTU: we stay at 23 */
        r[0] = 0x03; r[1] = 23; r[2] = 0; att_send(r, 3); return;
    case 0x04: {                                           /* Find Information */
        int k = 2, fmt = 0;
        r[0] = 0x05;
        for (int h = s < 1 ? 1 : s; h <= e && h <= H_LAST; h++) {
            attr(h, &ty, &tl, &va, &vl);
            int f = tl == 2 ? 1 : 2;
            if (!fmt) fmt = f;
            if (f != fmt || k + 2 + tl > 23) break;
            r[k++] = (u8)h; r[k++] = 0; memcpy(r + k, ty, (size_t)tl); k += tl;
        }
        if (!fmt) { att_error(op, s, 0x0A); return; }
        r[1] = (u8)fmt; att_send(r, k); return;
    }
    case 0x06: {                                           /* Find By Type Value: services by UUID */
        int k = 1;
        r[0] = 0x07;
        if (n < 7 || (p[5] | p[6] << 8) != 0x2800) { att_error(op, s, 0x0A); return; }
        for (int h = s < 1 ? 1 : s; h <= e && h <= H_LAST; h++) {
            attr(h, &ty, &tl, &va, &vl);
            if (memcmp_n(ty, U_PRIM, 2) || vl != n - 7 || memcmp_n(va, p + 7, (size_t)vl) || k + 4 > 23) continue;
            int ge = group_end(h);
            r[k++] = (u8)h; r[k++] = 0; r[k++] = (u8)ge; r[k++] = (u8)(ge >> 8);
        }
        if (k == 1) { att_error(op, s, 0x0A); return; }
        att_send(r, k); return;
    }
    case 0x08: case 0x10: {                                /* Read By Type / Read By Group Type */
        int ul = n - 5, k = 2, len = 0;
        if (ul != 2 && ul != 16) { att_error(op, s, 0x04); return; }
        if (op == 0x10 && (ul != 2 || (p[5] | p[6] << 8) != 0x2800)) { att_error(op, s, 0x10); return; }
        r[0] = op + 1;
        for (int h = s < 1 ? 1 : s; h <= e && h <= H_LAST; h++) {
            attr(h, &ty, &tl, &va, &vl);
            if (tl != ul || memcmp_n(ty, p + 5, (size_t)ul)) continue;
            int el = (op == 0x10 ? 4 : 2) + (vl > 19 ? 19 : vl);
            if (!len) len = el;
            if (el != len || k + el > 23) break;
            r[k++] = (u8)h; r[k++] = 0;
            if (op == 0x10) { int ge = group_end(h); r[k++] = (u8)ge; r[k++] = (u8)(ge >> 8); }
            memcpy(r + k, va, (size_t)(el - (op == 0x10 ? 4 : 2))); k += el - (op == 0x10 ? 4 : 2);
        }
        if (!len) { att_error(op, s, 0x0A); return; }
        r[1] = (u8)len; att_send(r, k); return;
    }
    case 0x0A: case 0x0C: {                                /* Read / Read Blob */
        int off = op == 0x0C && n >= 5 ? e : 0;
        if (!attr(s, &ty, &tl, &va, &vl)) { att_error(op, s, 0x01); return; }
        if (off > vl) { att_error(op, s, 0x07); return; }
        int l = vl - off > 22 ? 22 : vl - off;
        r[0] = op + 1; memcpy(r + 1, va + off, (size_t)l); att_send(r, 1 + l); return;
    }
    case 0x12: case 0x52:                                  /* Write Request / Write Command */
        if (s == H_CCCD && n >= 5) { cccd[0] = p[3] & 1; cccd[1] = 0; }
        else if (s == H_RX) nus_rx(p + 3, n - 3);
        else { if (op == 0x12) att_error(op, s, s >= 1 && s <= H_LAST ? 0x03 : 0x01); return; }
        if (op == 0x12) { r[0] = 0x13; att_send(r, 1); }
        return;
    }
    if (!(op & 0x40) && op != 0x1E) att_error(op, s, 0x06);   /* not a command / confirmation: not supported */
}

/* ======================= core 1: the UART, events, ACL ======================= */

static u8 pk[1100];
static int pk_n, pk_need;
static u64 scan_restart, adv_again;

static void hci_event(const u8 *e)              /* e[0] code, e[1] length, e + 2 parameters */
{
    switch (e[0]) {
    case 0x0E: {                                   /* Command Complete */
        credits = e[2];
        u16 op = (u16)(e[3] | e[4] << 8);
        if (!op) return;
        static int dw;
        done[dw].op = op; done[dw].st = e[5]; done[dw].seq = ++done_seq;
        for (int i = 0; i < 8; i++) done[dw].ret[i] = 6 + i < 2 + e[1] ? e[6 + i] : 0;
        dw = (dw + 1) % 8;
        return;
    }
    case 0x0F:                                     /* Command Status */
        credits = e[3];
        return;
    case 0x13:                                     /* Number Of Completed Packets */
        for (int i = 0; i < e[2]; i++) acl_credits += e[3 + 4 * i + 2] | e[3 + 4 * i + 3] << 8;
        return;
    case 0x05:                                     /* Disconnection Complete */
        if (conn >= 0) { con_color(COL_CYAN); kprintf("\n[BT] phone disconnected\n"); con_color(COL_DEFAULT); }
        conn = -1; cccd[0] = 0; session_role = 0; session_user[0] = 0;
        aq_h = aq_t = 0; tx_h = tx_t = 0;
        adv_again = timer_us() + 200000;
        return;
    case 0x02: case 0x22: case 0x2F:               /* classic inquiry results */
        inquiry_result(e);
        return;
    case 0x01:                                     /* Inquiry Complete */
        return;
    case 0x3E:
        if (e[2] == 0x02) adv_report(e);
        else if ((e[2] == 0x01 || e[2] == 0x0A) && e[3] == 0) {   /* (Enhanced) Connection Complete */
            conn = e[4] | (e[5] & 0x0F) << 8;
            cccd[0] = 0;
            session_role = users_count() ? 0 : 2;
            session_user[0] = 0;
            char a[18];
            mac_str(a, e + 8);
            con_color(COL_CYAN); kprintf("\n[BT] phone connected: %s\n", a); con_color(COL_DEFAULT);
        }
        return;
    }
}

static void hci_acl(const u8 *p, int n)        /* p = handle(2) len(2) data */
{
    if (n < 8 || ((p[1] >> 4) & 3) == 1) return;   /* continuation fragments: not used with MTU 23 */
    int l2 = p[4] | p[5] << 8, cid = p[6] | p[7] << 8;
    const u8 *d = p + 8;
    if (l2 > n - 8) return;
    if (cid == 4) att(d, l2);
    else if (cid == 6 && l2 >= 1 && d[0] == 0x01) {   /* SMP Pairing Request: not supported (no pairing yet) */
        u8 f[2] = { 0x05, 0x05 };
        acl_queue(6, f, 2);
    }
}

static void feed(u8 b)
{
    if (pk_n == 0) {
        if (b != 4 && b != 2) return;              /* not a packet start: skip (resync) */
        pk_need = b == 4 ? 3 : 5;
    }
    pk[pk_n++] = b;
    if (pk_n == 3 && pk[0] == 4) pk_need = 3 + pk[2];
    if (pk_n == 5 && pk[0] == 2) { pk_need = 5 + (pk[3] | pk[4] << 8); if (pk_need > (int)sizeof(pk)) { pk_n = 0; return; } }
    if (pk_n == pk_need) {
        if (pk[0] == 4) hci_event(pk + 1); else hci_acl(pk + 1, pk_n - 1);
        pk_n = 0;
    }
}

static void put_cmd(u16 op, const u8 *p, int n) { send_cmd(op, p, n); }

static void put_acl(const u8 *d, int n)
{
    uart_bt_putc(2);
    uart_bt_putc((u8)conn); uart_bt_putc((u8)(conn >> 8));   /* PB 00 BC 00 */
    uart_bt_putc((u8)n); uart_bt_putc((u8)(n >> 8));
    for (int i = 0; i < n; i++) uart_bt_putc(d[i]);
}

static const u8 sc_on[2] = { 1, 1 }, sc_off[2] = { 0, 1 }, adv_on[1] = { 1 };

void bt_poll(void)
{
    if (!bt_live) return;
    for (int i = 0; i < 512; i++) { int b = uart_bt_getc(0); if (b < 0) break; feed((u8)b); }
    u64 now = timer_us();
    if (now >= scan_restart) {                     /* duplicates are filtered: restart for fresh data */
        scan_restart = now + 15000000;
        hci_queue(0x200C, sc_off, 2);
        hci_queue(0x200C, sc_on, 2);
    }
    if (adv_again && now >= adv_again) { adv_again = 0; hci_queue(0x200A, adv_on, 1); }
    while (cq_t != cq_h && credits > 0) { put_cmd(cq[cq_t].op, cq[cq_t].p, cq[cq_t].n); credits--; cq_t = (cq_t + 1) % CQ; }
    /* the phone's text out: notifications of up to 20 bytes */
    while (conn >= 0 && cccd[0] && tx_t != tx_h && ((aq_h + 1) % AQ) != aq_t) {
        u8 nt[23] = { 0x1B, H_TX, 0 };
        int k = 3;
        while (k < 23 && tx_t != tx_h) { nt[k++] = (u8)tx_ring[tx_t]; tx_t = (tx_t + 1) % (int)sizeof(tx_ring); }
        acl_queue(4, nt, k);
    }
    if (conn < 0) aq_t = aq_h;
    while (aq_t != aq_h && acl_credits > 0 && conn >= 0) { put_acl(aq[aq_t].d, aq[aq_t].n); acl_credits--; aq_t = (aq_t + 1) % AQ; }
}

/* ======================= start / stop (core 0) ======================= */

static int bt_start(void)
{
    int t = board_type();
    if (t != 0x08 && t != 0x0D && t != 0x0E && t != 0x12) return err("this board has no Bluetooth");
    if (!fat_mounted()) return err("no SD card (the firmware is in /FIRMWARE)");
    bt_flow = t != 0x08;
    kprintf("Bluetooth: the console UART moves to the mini UART (same pins, same speed)\n");
    uart_console_mini(1);
    if (t == 0x08 || t == 0x0D || t == 0x0E) gpio_func(43, 4);
    uart_bt_init(bt_flow);
    bt_power(0);
    delay_us(20000);
    bt_power(1);
    delay_us(200000);
    while (uart_bt_getc(1000) >= 0) ;
    int st = -1;
    for (int i = 0; i < 3 && st; i++) st = cmd(0x0C03, 0, 0, 0, 0, 1000);
    if (st) { bt_power(0); uart_console_mini(0); return err("the Bluetooth chip does not answer (HCI reset)"); }
    u8 nm[248];
    memset(nm, 0, sizeof(nm));
    bt_name[0] = 0;
    if (cmd(0x0C14, 0, 0, nm, sizeof(nm), 1000) == 0) {
        int i = 0;
        for (; nm[i] && i < 31; i++) bt_name[i] = (char)nm[i];
        bt_name[i] = 0;
    }
    if (bt_name[0] != 'B') {
        const char *d = t == 0x08 ? "BCM43430A1" : t == 0x12 ? "BCM43430B0" : "BCM4345C0";
        int i = 0;
        for (; d[i]; i++) bt_name[i] = d[i];
        bt_name[i] = 0;
    }
    kprintf("  chip %s\n", bt_name);
    if (load_firmware(bt_name)) { bt_power(0); uart_console_mini(0); return -1; }
    if (cmd(0x0C03, 0, 0, 0, 0, 2000)) { bt_power(0); uart_console_mini(0); return err("no answer after the firmware"); }
    u8 a[8];
    if (cmd(0x1009, 0, 0, a, 6, 1000) == 0) for (int i = 0; i < 6; i++) bt_addr[i] = a[5 - i];
    u8 mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0x00, 0x60 };   /* + LE meta (61), Extended Inquiry Result (62) */
    cmd(0x0C01, mask, 8, 0, 0, 1000);
    u8 im = 2;
    cmd(0x0C45, &im, 1, 0, 0, 1000);                /* inquiry results with RSSI or extended (names) */
    if (cmd(0x2002, 0, 0, a, 3, 1000) == 0 && a[2]) { acl_len = a[0] | a[1] << 8; acl_credits = a[2]; }   /* LE buffers */
    else if (cmd(0x1005, 0, 0, a, 7, 1000) == 0) { acl_len = a[0] | a[1] << 8; acl_credits = a[3] | a[4] << 8; }
    if (acl_credits > 8) acl_credits = 8;
    /* advertising: flags + name; the scan response carries the Nordic UART Service UUID */
    int k = 0;
    for (const char *s = net_name(); *s && k < 22; s++) gap_name[k++] = *s;
    gap_name[k] = 0;
    memcpy(decl_rx + 3, U_RX, 16);
    memcpy(decl_tx + 3, U_TX, 16);
    u8 ap[15] = { 0xA0, 0, 0xA0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0 };   /* 100 ms, connectable, public */
    u8 ad[32], sr[32];
    memset(ad, 0, sizeof(ad)); memset(sr, 0, sizeof(sr));
    int n = 1;
    ad[n++] = 2; ad[n++] = 0x01; ad[n++] = 0x06;
    ad[n++] = (u8)(k + 1); ad[n++] = 0x09; memcpy(ad + n, gap_name, (size_t)k); n += k;
    ad[0] = (u8)(n - 1);
    sr[0] = 18; sr[1] = 17; sr[2] = 0x07; memcpy(sr + 3, U_NUS, 16);
    u8 sp[7] = { 1, 0x60, 0, 0x30, 0, 0, 0 };
    int r = cmd(0x2006, ap, 15, 0, 0, 1000) | cmd(0x2008, ad, 32, 0, 0, 1000) | cmd(0x2009, sr, 32, 0, 0, 1000)
          | cmd(0x200A, adv_on, 1, 0, 0, 1000) | cmd(0x200B, sp, 7, 0, 0, 1000) | cmd(0x200C, sc_on, 2, 0, 0, 1000);
    if (r) kprintf("  (advertising / scanning: the chip said %d)\n", r);
    io_lock();
    conn = -1; credits = 1; cq_h = cq_t = aq_h = aq_t = tx_h = tx_t = 0; pk_n = 0;
    scan_restart = timer_us() + 15000000; adv_again = 0;
    bt_up = 1;
    bt_live = 1;                                   /* core 1 takes the UART from here */
    io_unlock();
    kprintf("  Bluetooth on, address %02x:%02x:%02x:%02x:%02x:%02x, visible as \"%s\"\n",
            bt_addr[0], bt_addr[1], bt_addr[2], bt_addr[3], bt_addr[4], bt_addr[5], gap_name);
    return 0;
}

static void bt_stop(void)
{
    if (!bt_up) return;
    io_lock();
    bt_live = 0;
    conn = -1;
    io_unlock();
    cmd(0x0C03, 0, 0, 0, 0, 500);
    bt_power(0);
    bt_up = 0;
    uart_console_mini(0);
    gpio_func(30, 0); gpio_func(31, 0);
}

/* ======================= for MQTT and the JSON API (core 1) ======================= */

/* the next thermometer with data MQTT has not sent (at most every 30 s); 0 = none */
int bt_ble_next(struct ble_th *o)
{
    if (!bt_live) return 0;
    u64 now = timer_us();
    for (int i = 0; i < ndev; i++) {
        struct dev *d = &devs[i];
        if (!d->has_th || d->seq == d->sent_seq || (d->sent_at && now - d->sent_at < 30000000)) continue;
        d->sent_seq = d->seq; d->sent_at = now;
        for (int k = 0; k < 6; k++) o->mac[k] = d->addr[5 - k];
        memcpy(o->name, d->name, sizeof(o->name));
        o->temp = d->temp; o->hum = d->hum; o->batt = d->batt; o->rssi = d->rssi;
        o->first = !d->disc;
        d->disc = 1;
        return 1;
    }
    return 0;
}

static int kformat_u(char *o, u64 v)
{
    char t[24]; int k = 0, n = 0;
    do t[k++] = (char)('0' + v % 10); while (v /= 10);
    while (k) o[n++] = t[--k];
    o[n] = 0;
    return n;
}

/* GET /api/ble: the thermometers as JSON */
int bt_ble_json(char *b, int max)
{
    int n = 0;
    #define P(s) do { const char *q_ = (s); while (*q_ && n < max - 1) b[n++] = *q_++; } while (0)
    P("{\"ok\":true,\"bluetooth\":"); P(bt_live ? "true" : "false"); P(",\"sensors\":[");
    int first = 1;
    for (int i = 0; i < ndev; i++) {
        struct dev *d = &devs[i];
        if (!d->has_th) continue;
        char a[18], num[24];
        mac_str(a, d->addr);
        if (!first) P(",");
        first = 0;
        P("{\"mac\":\""); P(a); P("\",\"name\":\""); P(d->name); P("\",\"temp\":");
        int t = d->temp < 0 ? -d->temp : d->temp, k = 0;
        if (d->temp < 0) num[k++] = '-';
        k += (int)(kformat_u(num + k, (u64)(t / 100)));
        num[k++] = '.'; num[k++] = (char)('0' + t % 100 / 10); num[k++] = (char)('0' + t % 10); num[k] = 0;
        P(num);
        P(",\"hum\":"); k = (int)kformat_u(num, (u64)(d->hum / 100)); num[k++] = '.'; num[k++] = (char)('0' + d->hum % 100 / 10); num[k] = 0; P(num);
        P(",\"batt\":"); kformat_u(num, (u64)d->batt); P(num);
        P(",\"age_s\":"); kformat_u(num, (timer_us() - d->seen) / 1000000); P(num);
        P("}");
    }
    P("]}\n");
    #undef P
    b[n] = 0;
    return n;
}

/* ======================= command (core 0) ======================= */

static void list(u64 since)
{
    struct dev copy[MAXDEV];
    int nd;
    io_lock();
    nd = ndev;
    memcpy(copy, devs, sizeof(copy));
    io_unlock();
    for (int i = 1; i < nd; i++)
        for (int j = i; j > 0 && copy[j].rssi > copy[j - 1].rssi; j--) { struct dev t = copy[j]; copy[j] = copy[j - 1]; copy[j - 1] = t; }
    int shown = 0;
    for (int i = 0; i < nd; i++) {
        struct dev *d = &copy[i];
        if (d->seen < since) continue;
        char a[18];
        mac_str(a, d->addr);
        kprintf("  %s %s %4d dBm  %-22s", a, d->classic ? "BR " : d->type & 1 ? "rnd" : "pub", d->rssi, d->name[0] ? d->name : "-");
        if (d->maker >= 0) { const char *m = maker_name(d->maker); if (m) kprintf(" %s", m); else kprintf(" maker %04x", (u32)d->maker); }
        if (d->has_th) {
            int t = d->temp < 0 ? -d->temp : d->temp;
            kprintf("  %s%d.%d C %d.%d %% batt %d %%", d->temp < 0 ? "-" : "", t / 100, t % 100 / 10, d->hum / 100, d->hum % 100 / 10, d->batt);
        }
        if (d->kbd) {
            static const char *k[] = { "", "keyboard", "mouse", "keyboard+mouse", "HID device" };
            kprintf("  [%s, %s]", k[d->kbd], d->classic ? "classic BT" : "BLE");
        } else if (d->classic && d->cod) kprintf("  class %06x", d->cod);
        kprintf("\n");
        shown++;
    }
    kprintf("%d device(s)\n", shown);
}

/* /BT.TXT: "auto on" = switch Bluetooth on at boot */
void bt_boot(void)
{
    struct fat_file f;
    char b[32];
    if (!fat_mounted() || fat_open("/BT.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    if (!strstr(b, "auto on")) return;
    if (bt_start()) kprintf("Bluetooth: %s\n", bt_err);
}

void bt_cmd(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "off")) { bt_stop(); kprintf("Bluetooth off, the console is back on the PL011\n"); return; }
    if (argc > 2 && !strcmp(argv[1], "auto")) {
        int on = !strcmp(argv[2], "on");
        const char *t = on ? "auto on\r\n" : "auto off\r\n";
        if (!fat_mounted() || fat_write_file("/BT.TXT", t, (u32)strlen(t))) kprintf("(not saved: no SD card or write error)\n");
        kprintf("Bluetooth %s at boot\n", on ? "switches on" : "stays off");
        return;
    }
    if (argc > 1 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "scan") || !strcmp(argv[1], "sensors"))) {
        if (!bt_up && bt_start()) { kprintf("Bluetooth: %s\n", bt_err); return; }
        if (!strcmp(argv[1], "scan")) {
            u64 s = 5;
            if (argc > 2) parse_num(argv[2], &s);
            if (s < 1) s = 1;
            if (s > 120) s = 120;
            u64 t0 = timer_us();
            u64 il = s * 100 / 128;                /* classic inquiry too, in units of 1.28 s (max 0x30) */
            u8 inq[5] = { 0x33, 0x8B, 0x9E, (u8)(il < 1 ? 1 : il > 0x30 ? 0x30 : il), 0 };   /* GIAC */
            io_lock();
            hci_queue(0x0401, inq, 5);
            io_unlock();
            kprintf("listening %d s (Ctrl+C stops)...\n", (int)s);
            while (timer_us() < t0 + s * 1000000) {
                if (!io_core_active()) io_poll_all();   /* no I/O core: poll here */
                if (con_trygetc() == 3) break;
                delay_us(2000);
            }
            list(t0);
        } else if (!strcmp(argv[1], "sensors")) {
            char b[1024];
            io_lock();
            bt_ble_json(b, sizeof(b));
            io_unlock();
            kprintf("%s", b);
        }
        return;
    }
    if (argc > 1) { kprintf("usage: bt [on | scan [seconds] | sensors | off | auto on|off]\n"); return; }
    if (!bt_up) { kprintf("Bluetooth off ('bt on', 'bt scan'; 'bt auto on' = at boot; firmware in /FIRMWARE/*.hcd)\n"); return; }
    kprintf("Bluetooth on: chip %s, address %02x:%02x:%02x:%02x:%02x:%02x, visible as \"%s\"\n", bt_name,
            bt_addr[0], bt_addr[1], bt_addr[2], bt_addr[3], bt_addr[4], bt_addr[5], gap_name);
    if (conn >= 0) kprintf("  phone connected%s%s%s\n", session_user[0] ? " as " : "", session_user, cccd[0] ? "" : " (not listening yet)");
    else kprintf("  no phone connected (Nordic UART Service: Serial Bluetooth Terminal, nRF Toolbox, Bluefruit Connect)\n");
    int th = 0;
    for (int i = 0; i < ndev; i++) th += devs[i].has_th;
    kprintf("  %d device(s) seen, %d thermometer(s) ('bt scan', 'bt sensors')\n", ndev, th);
}
