/*
 * Bluetooth Low Energy, step 1: the BT half of the WiFi/BT combo chip (CYW43430 on the Pi 3B,
 * CYW43455 on the 3B+, CYW43436 on the Zero 2 W) over HCI on the PL011 UART (GPIO32/33, H4
 * framing, 115200 8N1). Switched on only when asked ('bt on' / 'bt scan'): then the console moves
 * to the mini UART (uart.c). The chip needs its patch RAM firmware from /FIRMWARE/<name>.hcd
 * (sd/FIRMWARE, from Raspberry Pi's bluez-firmware, Cypress license).
 *
 * 'bt scan [seconds]' lists BLE devices: address, signal, name, maker, and the temperature /
 * humidity of Xiaomi LYWSD03MMC thermometers with the ATC / pvvx firmware (service data 0x181A).
 */
#include "kernel.h"
#include "sd.h"

static int bt_up, bt_flow;
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

/* ---------------- HCI (H4) ---------------- */

static u8 ev[260];                         /* the last event: ev[0] code, ev[1] length, ev[2..] parameters */
static void (*on_event)(const u8 *e);       /* events that are not the answer we wait for (scan reports) */

static void send_cmd(u16 op, const void *p, int n)
{
    uart_bt_putc(1);
    uart_bt_putc((u8)op); uart_bt_putc((u8)(op >> 8));
    uart_bt_putc((u8)n);
    for (int i = 0; i < n; i++) uart_bt_putc(((const u8 *)p)[i]);
}

/* one packet from the chip; 1 = an event in ev[], 0 = something else (skipped), -1 = timeout */
static int read_packet(u32 us)
{
    int t = uart_bt_getc(us);
    if (t < 0) return -1;
    if (t == 4) {                                       /* event */
        int c = uart_bt_getc(100000), l = uart_bt_getc(100000);
        if (c < 0 || l < 0) return -1;
        ev[0] = (u8)c; ev[1] = (u8)l;
        for (int i = 0; i < l; i++) { int b = uart_bt_getc(100000); if (b < 0) return -1; ev[2 + i] = (u8)b; }
        return 1;
    }
    if (t == 2) {                                       /* ACL data: handle(2) len(2) data - not used yet */
        u8 h[4];
        for (int i = 0; i < 4; i++) { int b = uart_bt_getc(100000); if (b < 0) return -1; h[i] = (u8)b; }
        for (int i = 0, n = h[2] | h[3] << 8; i < n; i++) if (uart_bt_getc(100000) < 0) return -1;
        return 0;
    }
    return 0;                                           /* noise: wait for the next type byte */
}

/* a command and its Command Complete; returns its status (0 = ok), -1 = no answer.
 * out (if given) gets the return parameters after the status */
static int cmd(u16 op, const void *p, int n, u8 *out, int outmax, u32 ms)
{
    send_cmd(op, p, n);
    u64 end = timer_us() + (u64)ms * 1000;
    while (timer_us() < end) {
        if (read_packet(20000) != 1) continue;
        if (ev[0] == 0x0E && ev[1] >= 4 && (ev[3] | ev[4] << 8) == op) {     /* Command Complete */
            int rl = ev[1] - 4;
            if (out) for (int i = 0; i < rl && i < outmax; i++) out[i] = ev[6 + i];
            return ev[5];
        }
        if (ev[0] == 0x0F && ev[1] >= 4 && (ev[4] | ev[5] << 8) == op && ev[2]) return ev[2];   /* Command Status: failed */
        if (on_event) on_event(ev);
    }
    return -1;
}

static void hexb(char *d, u8 v) { const char *h = "0123456789abcdef"; d[0] = h[v >> 4]; d[1] = h[v & 15]; }

/* the patch RAM firmware: the .hcd file is a list of HCI commands (opcode, length, data) */
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
    delay_us(250000);                                   /* the chip restarts with the new code (115200) */
    kprintf("  %d records loaded\n", n);
    return 0;
}

static int bt_start(void)
{
    int t = board_type();
    if (t != 0x08 && t != 0x0D && t != 0x0E && t != 0x12) return err("this board has no Bluetooth");
    if (!fat_mounted()) return err("no SD card (the firmware is in /FIRMWARE)");
    bt_flow = t != 0x08;                                /* the 3B has no RTS/CTS lines to the chip */
    kprintf("Bluetooth: the console UART moves to the mini UART (same pins, same speed)\n");
    uart_console_mini(1);
    if (t == 0x08 || t == 0x0D || t == 0x0E) gpio_func(43, 4);   /* GPCLK2: the chip's 32 kHz sleep clock */
    uart_bt_init(bt_flow);
    bt_power(0);
    delay_us(20000);
    bt_power(1);
    delay_us(200000);
    while (uart_bt_getc(1000) >= 0) ;                   /* whatever the chip said while starting */
    int st = -1;
    for (int i = 0; i < 3 && st; i++) st = cmd(0x0C03, 0, 0, 0, 0, 1000);   /* HCI_Reset */
    if (st) { bt_power(0); uart_console_mini(0); return err("the Bluetooth chip does not answer (HCI reset)"); }
    u8 nm[248];
    memset(nm, 0, sizeof(nm));
    if (cmd(0x0C14, 0, 0, nm, sizeof(nm), 1000) == 0) {             /* Read Local Name: "BCM43430A1" */
        int i = 0;
        for (; nm[i] && i < 31; i++) bt_name[i] = (char)nm[i];
        bt_name[i] = 0;
    }
    if (!bt_name[0] || bt_name[0] != 'B') {             /* not reported: by board */
        const char *d = t == 0x08 ? "BCM43430A1" : t == 0x12 ? "BCM43430B0" : "BCM4345C0";
        int i = 0;
        for (; d[i]; i++) bt_name[i] = d[i];
        bt_name[i] = 0;
    }
    kprintf("  chip %s\n", bt_name);
    if (load_firmware(bt_name)) { bt_power(0); uart_console_mini(0); return -1; }
    if (cmd(0x0C03, 0, 0, 0, 0, 2000)) { bt_power(0); uart_console_mini(0); return err("no answer after the firmware"); }
    u8 a[6];
    if (cmd(0x1009, 0, 0, a, 6, 1000) == 0) for (int i = 0; i < 6; i++) bt_addr[i] = a[5 - i];   /* Read BD_ADDR */
    u8 mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0x00, 0x20 };   /* + LE Meta events */
    cmd(0x0C01, mask, 8, 0, 0, 1000);
    bt_up = 1;
    kprintf("  Bluetooth on, address %02x:%02x:%02x:%02x:%02x:%02x\n", bt_addr[0], bt_addr[1], bt_addr[2], bt_addr[3], bt_addr[4], bt_addr[5]);
    return 0;
}

static void bt_stop(void)
{
    if (!bt_up) return;
    cmd(0x0C03, 0, 0, 0, 0, 500);
    bt_power(0);
    bt_up = 0;
    uart_console_mini(0);
    gpio_func(30, 0); gpio_func(31, 0);
}

/* ---------------- BLE scan ---------------- */

#define MAXDEV 40
static struct dev {
    u8 addr[6], type;
    int rssi, maker, temp, hum, batt, has_th;           /* temp in 0.01 C, hum in 0.01 %, batt % */
    char name[30];
} devs[MAXDEV];
static int ndev;

static const char *maker_name(int id)
{
    switch (id) {
    case 0x004C: return "Apple";
    case 0x0006: return "Microsoft";
    case 0x0075: return "Samsung";
    case 0x00E0: return "Google";
    case 0x0059: return "Nordic";
    case 0x038F: return "Xiaomi";
    case 0x0157: return "Huami";
    case 0x02E5: return "Espressif";
    case 0x0131: return "Cypress";
    case 0x000F: return "Broadcom";
    case 0x0087: return "Garmin";
    case 0x0499: return "Ruuvi";
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
        if (t == 0x09 || (t == 0x08 && !d->name[0])) {               /* complete / short name */
            int k = 0;
            for (; k < vl && k < 29; k++) d->name[k] = (v[k] >= 32 && v[k] < 127) ? (char)v[k] : '?';
            d->name[k] = 0;
        } else if (t == 0xFF && vl >= 2) d->maker = v[0] | v[1] << 8;
        else if (t == 0x16 && vl >= 2 && (v[0] | v[1] << 8) == 0x181A) {   /* Environmental Sensing service data */
            const u8 *s = v + 2;
            int sl = vl - 2;
            if (sl == 13) {                                 /* ATC1441: MAC, temp BE 0.1 C, hum %, batt %, mV, count */
                d->temp = (short)(s[6] << 8 | s[7]) * 10; d->hum = s[8] * 100; d->batt = s[9]; d->has_th = 1;
            } else if (sl >= 15) {                          /* pvvx: MAC, temp LE 0.01 C, hum LE 0.01 %, mV, batt % */
                d->temp = (short)(s[6] | s[7] << 8); d->hum = s[8] | s[9] << 8; d->batt = s[12]; d->has_th = 1;
            }
        }
        i += 1 + l;
    }
}

static void scan_event(const u8 *e)
{
    if (e[0] != 0x3E || e[2] != 0x02) return;           /* LE Meta: Advertising Report */
    int num = e[3];
    const u8 *p = e + 4, *end = e + 2 + e[1];
    for (int r = 0; r < num && p + 10 <= end; r++) {
        u8 type = p[1];
        const u8 *a = p + 2;
        int dl = p[8];
        if (p + 9 + dl + 1 > end) break;
        int rssi = (signed char)p[9 + dl];
        struct dev *d = 0;
        for (int i = 0; i < ndev; i++) if (!memcmp_n(devs[i].addr, a, 6)) d = &devs[i];
        if (!d && ndev < MAXDEV) { d = &devs[ndev++]; memset(d, 0, sizeof(*d)); memcpy(d->addr, a, 6); d->type = type; d->maker = -1; }
        if (d) { if (!d->rssi || rssi > d->rssi) d->rssi = rssi; adv_data(d, p + 9, dl); }
        p += 10 + dl;
    }
}

static void scan(int secs)
{
    u8 sp[7] = { 1, 0x60, 0, 0x30, 0, 0, 0 };          /* active (asks for the scan response = names), 60 / 30 ms, public, all */
    u8 on[2] = { 1, 0 }, off[2] = { 0, 0 };
    ndev = 0;
    on_event = scan_event;
    if (cmd(0x200B, sp, 7, 0, 0, 1000) || cmd(0x200C, on, 2, 0, 0, 1000)) {
        on_event = 0;
        kprintf("scan: the chip refused to scan\n");
        return;
    }
    kprintf("scanning %d s (Ctrl+C stops)...\n", secs);
    u64 end = timer_us() + (u64)secs * 1000000;
    while (timer_us() < end) {
        if (read_packet(20000) == 1) scan_event(ev);
        if (con_trygetc() == 3) break;
    }
    cmd(0x200C, off, 2, 0, 0, 1000);
    on_event = 0;
    for (int i = 1; i < ndev; i++)                       /* strongest first */
        for (int j = i; j > 0 && devs[j].rssi > devs[j - 1].rssi; j--) { struct dev t = devs[j]; devs[j] = devs[j - 1]; devs[j - 1] = t; }
    for (int i = 0; i < ndev; i++) {
        struct dev *d = &devs[i];
        char a[18];
        for (int k = 0; k < 6; k++) { hexb(a + 3 * k, d->addr[5 - k]); a[3 * k + 2] = k < 5 ? ':' : 0; }
        kprintf("  %s %s %4d dBm  %-22s", a, d->type & 1 ? "rnd" : "pub", d->rssi, d->name[0] ? d->name : "-");
        if (d->maker >= 0) { const char *m = maker_name(d->maker); if (m) kprintf(" %s", m); else kprintf(" maker %04x", (u32)d->maker); }
        if (d->has_th) {
            int t = d->temp < 0 ? -d->temp : d->temp;
            kprintf("  %s%d.%d C %d.%d %% batt %d %%", d->temp < 0 ? "-" : "", t / 100, t % 100 / 10, d->hum / 100, d->hum % 100 / 10, d->batt);
        }
        kprintf("\n");
    }
    kprintf("%d device(s)\n", ndev);
}

/* ---------------- command ---------------- */

void bt_cmd(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "off")) { bt_stop(); kprintf("Bluetooth off, the console is back on the PL011\n"); return; }
    if (argc > 1 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "scan"))) {
        if (!bt_up && bt_start()) { kprintf("Bluetooth: %s\n", bt_err); return; }
        if (!strcmp(argv[1], "scan")) {
            u64 s = 5;
            if (argc > 2) parse_num(argv[2], &s);
            if (s < 1) s = 1;
            if (s > 120) s = 120;
            scan((int)s);
        }
        return;
    }
    if (argc > 1) { kprintf("usage: bt [on | scan [seconds] | off]\n"); return; }
    if (!bt_up) { kprintf("Bluetooth off ('bt scan' or 'bt on' starts it; firmware in /FIRMWARE/*.hcd)\n"); return; }
    kprintf("Bluetooth on: chip %s, address %02x:%02x:%02x:%02x:%02x:%02x, console on the mini UART\n", bt_name,
            bt_addr[0], bt_addr[1], bt_addr[2], bt_addr[3], bt_addr[4], bt_addr[5]);
}
