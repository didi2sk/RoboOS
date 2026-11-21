/*
 * radio.c - FM radio with an RDA5807M module (RRD-102) on the I2C bus (0.71).
 * The chip answers on two addresses: 0x11 = random register access (writes: reg, hi, lo) and
 * 0x10 = sequential access (a read returns registers 0x0A, 0x0B, 0x0C.. in order - one read gives the
 * status + RSSI + the 4 RDS blocks, no repeated start needed). Band 87-108 MHz, 100 kHz steps,
 * de-emphasis 50 us (Europe). The sound comes out of the module itself (L/R pins).
 * RDS: group 0A/0B = station name (PS, 8 chars), 2A/2B = radio text (64 chars); a character pair is
 * taken only when the same pair arrived twice in a row (the chip reports block errors for A/B only).
 * Core 0: `radio` command, radio_poll() from con_trygetc (RDS every 30 ms, network requests);
 * core 1 (JSON API, MQTT) only queues a command line. Settings in /RADIO.TXT.
 */
#ifndef RADIO_HOST_TEST
#include "kernel.h"
#include "sd.h"
#endif

#define RA 0x11                                  /* random access */
#define RS 0x10                                  /* sequential access */

/* register 2 bits */
#define R2_DHIZ    0x8000
#define R2_DMUTE   0x4000
#define R2_MONO    0x2000
#define R2_BASS    0x1000
#define R2_SEEKUP  0x0200
#define R2_SEEK    0x0100
#define R2_RDS     0x0008
#define R2_NEW     0x0004
#define R2_RESET   0x0002
#define R2_ENABLE  0x0001
/* register 0x0A bits */
#define S_RDSR     0x8000
#define S_STC      0x4000
#define S_SF       0x2000
#define S_ST       0x0400

static int present, on, vol = 8, mono, bass;
static u32 freq = 10000;                         /* in 10 kHz: 10440 = 104.40 MHz */
static u16 reg2;
static u16 st[6];                                /* the last 0x0A..0x0F */
static char ps[9], rt[65];
static char ps_cand[8], rt_cand[64];
static int rt_ab = -1;
static u64 next_poll;

/* ---------------- the chip ---------------- */

static int wr(u8 reg, u16 v)
{
    u8 b[3] = { reg, (u8)(v >> 8), (u8)v };
    return i2c_write(RA, b, 3);
}

static int read_status(void)
{
    u8 b[12];
    if (i2c_read(RS, b, 12)) return -1;
    for (int i = 0; i < 6; i++) st[i] = (u16)(b[2 * i] << 8 | b[2 * i + 1]);
    return 0;
}

static u32 chan_freq(void) { return 8700 + (u32)(st[0] & 0x3FF) * 10; }
static void clear_rds(void) { memset(ps, 0, sizeof(ps)); memset(rt, 0, sizeof(rt)); memset(ps_cand, 0, sizeof(ps_cand)); memset(rt_cand, 0, sizeof(rt_cand)); rt_ab = -1; }

static void set_reg2(void)
{
    reg2 = R2_DHIZ | R2_RDS | R2_NEW | R2_ENABLE | (vol ? R2_DMUTE : 0) | (mono ? R2_MONO : 0) | (bass ? R2_BASS : 0);
    wr(2, reg2);
}

static int wait_stc(u32 ms)
{
    u64 end = timer_us() + (u64)ms * 1000;
    while (timer_us() < end) {
        delay_us(20000);
        if (read_status() == 0 && (st[0] & S_STC)) return 0;
    }
    return -1;
}

static int tune(u32 f)
{
    if (f < 8700 || f > 10800) return -1;
    u32 ch = (f - 8700) / 10;
    clear_rds();
    if (wr(3, (u16)(ch << 6 | 0x10))) return -2;     /* TUNE, band 87-108, 100 kHz */
    wait_stc(500);
    freq = 8700 + ch * 10;
    return 0;
}

static int chip_on(void)
{
    if (wr(2, R2_RESET)) return -1;
    delay_us(50000);
    set_reg2();
    delay_us(100000);
    wr(4, 0x0800);                                   /* de-emphasis 50 us */
    wr(5, (u16)(0x8880 | (vol & 15)));               /* INT_MODE, seek threshold 8, LNA, volume */
    on = 1;
    return tune(freq);
}

static void chip_off(void) { wr(2, 0); on = 0; clear_rds(); }

static void set_volume(int v)
{
    vol = v < 0 ? 0 : v > 15 ? 15 : v;
    if (!on) return;
    wr(5, (u16)(0x8880 | (vol ? vol : 0)));
    set_reg2();                                      /* volume 0 = muted */
}

/* seek up/down; 0 = found, -1 = nothing (the whole band) */
static int seek(int up)
{
    clear_rds();
    wr(2, (u16)(reg2 | R2_SEEK | (up ? R2_SEEKUP : 0)));
    u64 end = timer_us() + 8000000;
    int r = -1;
    while (timer_us() < end) {
        delay_us(30000);
        if (read_status()) break;
        if (st[0] & S_STC) { r = (st[0] & S_SF) ? -1 : 0; break; }
#ifndef RADIO_HOST_TEST
        if (con_break()) break;
#endif
    }
    wr(2, reg2);                                     /* SEEK off */
    read_status();
    freq = chan_freq();
    return r;
}

/* ---------------- RDS ---------------- */

static char rds_char(u8 c) { return c >= 32 && c < 127 ? (char)c : c == 0 ? 0 : c == '\r' ? '\r' : ' '; }

static void rds_group(void)
{
    u16 b = st[3], c = st[4], d = st[5];
    int blerb = st[1] & 3;
    if (blerb > 1) return;                           /* block B (group type) not reliable */
    int type = b >> 12, ver = (b >> 11) & 1;
    if (type == 0) {                                 /* PS name, 2 chars per group */
        int seg = b & 3;
        char c1 = rds_char((u8)(d >> 8)), c2 = rds_char((u8)d);
        if (ps_cand[seg * 2] == c1 && ps_cand[seg * 2 + 1] == c2) { ps[seg * 2] = c1; ps[seg * 2 + 1] = c2; }
        ps_cand[seg * 2] = c1; ps_cand[seg * 2 + 1] = c2;
    } else if (type == 2) {                          /* radio text */
        int ab = (b >> 4) & 1, seg = b & 15;
        if (ab != rt_ab) { memset(rt, 0, sizeof(rt)); memset(rt_cand, 0, sizeof(rt_cand)); rt_ab = ab; }
        char ch[4];
        int n;
        if (!ver) { ch[0] = rds_char((u8)(c >> 8)); ch[1] = rds_char((u8)c); ch[2] = rds_char((u8)(d >> 8)); ch[3] = rds_char((u8)d); n = 4; }
        else { ch[0] = rds_char((u8)(d >> 8)); ch[1] = rds_char((u8)d); n = 2; }
        int at = seg * n, same = 1;
        for (int i = 0; i < n; i++) if (rt_cand[at + i] != ch[i]) same = 0;
        if (same) for (int i = 0; i < n; i++) rt[at + i] = ch[i] == '\r' ? 0 : ch[i];
        for (int i = 0; i < n; i++) rt_cand[at + i] = ch[i];
    }
}

/* ---------------- settings ---------------- */

static void ap2(char *b, int *n, const char *s) { while (*s) b[(*n)++] = *s++; }
static void freq_text(char *b, u32 f)            /* "104.4" */
{
    int n = 0; char t[8]; int k = 0; u32 mhz = f / 100;
    do t[k++] = (char)('0' + mhz % 10); while ((mhz /= 10));
    while (k) b[n++] = t[--k];
    b[n++] = '.'; b[n++] = (char)('0' + f / 10 % 10); b[n] = 0;
}

static void save(void)
{
#ifndef RADIO_HOST_TEST
    char b[96], f[12]; int n = 0;
    freq_text(f, freq);
    ap2(b, &n, "state "); ap2(b, &n, on ? "on" : "off");
    ap2(b, &n, "\nfreq "); ap2(b, &n, f);
    ap2(b, &n, "\nvol "); if (vol >= 10) b[n++] = '1'; b[n++] = (char)('0' + vol % 10);
    ap2(b, &n, "\nmono "); ap2(b, &n, mono ? "on" : "off");
    ap2(b, &n, "\nbass "); ap2(b, &n, bass ? "on" : "off");
    b[n++] = '\n';
    if (fat_mounted()) fat_write_file("/RADIO.TXT", b, (u32)n);
#endif
}

/* "104.4" / "104,4" / "104" / "1044" -> 10 kHz units, 0 = bad */
static u32 parse_freq(const char *s)
{
    u32 a = 0, frac = 0; int fd = 0, dot = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') { if (dot) { if (fd < 2) { frac = frac * 10 + (u32)(*s - '0'); fd++; } } else a = a * 10 + (u32)(*s - '0'); }
        else if ((*s == '.' || *s == ',') && !dot) dot = 1;
        else return 0;
    }
    if (!dot && a > 200 && a < 2000) return a * 10;  /* 1044 = 104.4 MHz */
    while (fd < 2) { frac *= 10; fd++; }
    u32 f = a * 100 + frac;
    return f >= 8700 && f <= 10800 ? f : 0;
}

#ifndef RADIO_HOST_TEST
static void load(void)
{
    struct fat_file fl;
    char b[160];
    if (!fat_mounted() || fat_open("/RADIO.TXT", &fl) || fl.is_dir) return;
    int n = fat_read(&fl, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    for (char *l = b; *l; ) {
        char *e = l; while (*e && *e != '\n' && *e != '\r') e++;
        char sv = *e; *e = 0;
        if (!strncmp(l, "freq ", 5)) { u32 f = parse_freq(l + 5); if (f) freq = f; }
        else if (!strncmp(l, "vol ", 4)) { u64 v; if (parse_num(l + 4, &v) && v <= 15) vol = (int)v; }
        else if (!strncmp(l, "mono ", 5)) mono = !strcmp(l + 5, "on");
        else if (!strncmp(l, "bass ", 5)) bass = !strcmp(l + 5, "on");
        else if (!strncmp(l, "state ", 6)) on = !strcmp(l + 6, "on");
        *e = sv; l = *e ? e + 1 : e; while (*l == '\n' || *l == '\r') l++;
    }
}
#endif

static int detect(void)
{
    u8 b[2];
    present = i2c_read(RS, b, 2) == 0 && i2c_probe(RA);
    return present;
}

/* ---------------- interface ---------------- */

#ifndef RADIO_HOST_TEST
void radio_boot(void)
{
    load();
    int want = on;
    on = 0;
    if (!detect()) return;
    kprintf("FM radio RDA5807M found");
    if (want && chip_on() == 0) { char f[12]; freq_text(f, freq); kprintf(", playing %s MHz", f); }
    kprintf("\n");
}
#endif

/* a command line from core 1 (JSON API, MQTT) - run by core 0 in radio_poll */
static volatile int qlock, qpend;
static char qcmd[48];
void radio_net(const char *cmd, int len)
{
    while (__atomic_test_and_set(&qlock, __ATOMIC_ACQUIRE)) ;
    int n = 0;
    for (int i = 0; i < len && n < (int)sizeof(qcmd) - 1; i++) if ((u8)cmd[i] >= 32) qcmd[n++] = cmd[i];
    qcmd[n] = 0;
    qpend = 1;
    __atomic_clear(&qlock, __ATOMIC_RELEASE);
}

void radio_cmd(int argc, char **argv);

void radio_poll(void)
{
    if (qpend) {
        char c[48], *av[4]; int ac = 1;
        while (__atomic_test_and_set(&qlock, __ATOMIC_ACQUIRE)) ;
        memcpy(c, qcmd, sizeof(c)); qpend = 0;
        __atomic_clear(&qlock, __ATOMIC_RELEASE);
        av[0] = "radio";
        for (char *p = c; *p && ac < 4; ) {
            while (*p == ' ') *p++ = 0;
            if (!*p) break;
            av[ac++] = p;
            while (*p && *p != ' ') p++;
        }
        radio_cmd(ac, av);
        return;
    }
    if (!on || timer_us() < next_poll) return;
    next_poll = timer_us() + 30000;
    if (read_status()) return;
    if (st[0] & S_RDSR) rds_group();
}

/* for the display, the API, MQTT (any core reads; a torn value is harmless) */
int radio_on(void) { return on; }
u32 radio_freq(void) { return freq; }
int radio_rssi(void) { return st[1] >> 9; }
int radio_stereo(void) { return (st[0] & S_ST) != 0; }
int radio_volume(void) { return vol; }
int radio_present(void) { return present; }
const char *radio_ps(void) { return ps; }
const char *radio_rt(void) { return rt; }

static void show(void)
{
    char f[12];
    freq_text(f, freq);
    if (!present) { kprintf("no FM radio found (RDA5807M module on I2C 0x10/0x11, 3.3 V)\n"); return; }
    if (!on) { kprintf("FM radio: off (last %s MHz, volume %d) - 'radio on'\n", f, vol); return; }
    read_status();
    kprintf("FM radio: %s MHz, %s, signal %d/127%s, volume %d%s%s\n", f, radio_stereo() ? "stereo" : "mono",
            radio_rssi(), (st[1] & 0x0100) ? "" : " (no station?)", vol, mono ? ", forced mono" : "", bass ? ", bass" : "");
    if (ps[0]) { char p[9]; int k = 0; for (int i = 0; i < 8; i++) p[k++] = ps[i] ? ps[i] : ' '; p[k] = 0; kprintf("  station: %s\n", p); }
    if (rt[0]) { char r[65]; int k = 0; for (int i = 0; i < 64 && rt[i]; i++) r[k++] = rt[i]; r[k] = 0; kprintf("  text:    %s\n", r); }
}

void radio_cmd(int argc, char **argv)
{
    const char *a = argc > 1 ? argv[1] : "";
    u32 f;
    if (!present && !detect()) { show(); return; }
    if (!a[0]) { show(); return; }
    if (!strcmp(a, "off")) { chip_off(); save(); kprintf("radio off\n"); return; }
    if (!strcmp(a, "on")) { if (chip_on()) kprintf("the radio does not answer\n"); save(); show(); return; }
    if (!strcmp(a, "vol") && argc > 2) {
        u64 v;
        if (!parse_num(argv[2], &v) || v > 15) { kprintf("volume 0-15\n"); return; }
        set_volume((int)v); save(); kprintf("volume %d\n", vol); return;
    }
    if (!strcmp(a, "mono") || !strcmp(a, "bass")) {
        int v = argc > 2 && !strcmp(argv[2], "on");
        if (a[0] == 'm') mono = v; else bass = v;
        if (on) set_reg2();
        save(); show(); return;
    }
    if (!strcmp(a, "up") || !strcmp(a, "down") || !strcmp(a, "seek")) {
        if (!on && chip_on()) { kprintf("the radio does not answer\n"); return; }
        kprintf("seeking %s ...\n", a[0] == 'd' ? "down" : "up");
        if (seek(a[0] != 'd')) kprintf("no station found\n");
        save(); show(); return;
    }
    if (!strcmp(a, "scan")) {                        /* list the stations of the whole band */
        if (!on && chip_on()) { kprintf("the radio does not answer\n"); return; }
        u32 back = freq, first = 0;
        int n = 0;
        tune(8700);
        kprintf("stations (Ctrl+C stops):\n");
        while (n < 40 && seek(1) == 0) {
            if (first && freq <= first) break;       /* wrapped around */
            if (!first) first = freq;
            char fs[12]; freq_text(fs, freq);
            delay_us(150000); read_status();
            kprintf("  %6s MHz  signal %3d%s\n", fs, radio_rssi(), radio_stereo() ? "  stereo" : "");
            n++;
        }
        tune(back);
        kprintf("%d station(s); back on the last one\n", n);
        return;
    }
    if ((f = parse_freq(a)) != 0) {
        if (!on && chip_on()) { kprintf("the radio does not answer\n"); return; }
        tune(f); save(); show(); return;
    }
    kprintf("usage: radio [on | off | <MHz e.g. 104.4> | up | down | scan | vol 0-15 | mono on|off | bass on|off]\n");
}
