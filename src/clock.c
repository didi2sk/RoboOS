/*
 * Wall clock: Unix time from NTP (ip.c asks the server, about once an hour) or
 * set by hand, kept with the free-running microsecond timer. Time zone with the
 * EU summer time rule (last Sunday of March .. last Sunday of October, 01:00 UTC).
 *
 * /TIME.TXT (read on core 0 at boot, written by the 'time' command):
 *   server pool.ntp.org      NTP server ("off" = no NTP)
 *   zone +1 eu               UTC offset in hours (+5:30 works too), eu = summer time, none = no
 */
#include "kernel.h"
#include "sd.h"

static volatile u64 base_unix, base_us;    /* the time at the moment base_us of the timer */
static volatile u64 synced_us;             /* when NTP last set it (0 = never) */
static volatile int set_by;                /* 0 not set, 1 NTP, 2 by hand */
static char server[48] = "pool.ntp.org";
static int zone_min = 60, zone_eu = 1;
static volatile int want_sync = 1;

void clock_set_time(u64 unix_s, int by)
{
    base_us = timer_us();
    base_unix = unix_s;
    set_by = by;
    if (by == 1) synced_us = base_us;
}

/* seconds since 1970-01-01 UTC, 0 = not known yet */
u64 clock_now(void)
{
    if (!set_by) return 0;
    return base_unix + (timer_us() - base_us) / 1000000;
}

const char *clock_server(void) { return server; }
int  clock_ntp_wanted(void) { int w = want_sync; want_sync = 0; return w && server[0]; }
void clock_ntp_again(void) { want_sync = 1; }
int  clock_how(void) { return set_by; }
u64  clock_synced_ago(void) { return synced_us ? (timer_us() - synced_us) / 1000000 : 0; }

/* ---------------- calendar ---------------- */

/* days since 1970-01-01 -> year, month, day (H. Hinnant's civil_from_days) */
static void civil(long z, int *y, int *m, int *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

/* the last Sunday of a month, 01:00 UTC, as Unix time */
static u64 last_sunday(int y, int m)
{
    int nm = m == 12 ? 1 : m + 1, ny = m == 12 ? y + 1 : y;
    long last = days_from_civil(ny, nm, 1) - 1;          /* the last day of month m */
    long wd = (last + 4) % 7;                            /* 1970-01-01 was a Thursday; 0 = Sunday */
    return (u64)(last - wd) * 86400 + 3600;
}

/* minutes to add to UTC at that moment */
int clock_offset(u64 t)
{
    int off = zone_min;
    if (zone_eu && t) {
        int y, m, d;
        civil((long)(t / 86400), &y, &m, &d);
        if (t >= last_sunday(y, 3) && t < last_sunday(y, 10)) off += 60;
    }
    return off;
}

/* "2026-11-18 21:05:07" (local), or ISO 8601 "2026-11-18T21:05:07+01:00" */
int clock_format(char *b, u64 t, int iso)
{
    static const char dig[] = "0123456789";
    int off = clock_offset(t);
    u64 l = t + (u64)((long)off * 60);
    int y, mo, d;
    civil((long)(l / 86400), &y, &mo, &d);
    int s = (int)(l % 86400), n = 0;
    int v[6] = { y, mo, d, s / 3600, s / 60 % 60, s % 60 };
    for (int i = 0; i < 6; i++) {
        if (i == 0) { b[n++] = dig[y / 1000 % 10]; b[n++] = dig[y / 100 % 10]; b[n++] = dig[y / 10 % 10]; b[n++] = dig[y % 10]; }
        else { b[n++] = dig[v[i] / 10]; b[n++] = dig[v[i] % 10]; }
        if (i < 2) b[n++] = '-';
        else if (i == 2) b[n++] = iso ? 'T' : ' ';
        else if (i < 5) b[n++] = ':';
    }
    if (iso) {
        int a = off < 0 ? -off : off;
        b[n++] = off < 0 ? '-' : '+';
        b[n++] = dig[a / 600]; b[n++] = dig[a / 60 % 10]; b[n++] = ':';
        b[n++] = dig[a % 60 / 10]; b[n++] = dig[a % 10];
    }
    b[n] = 0;
    return n;
}

/* ---------------- /TIME.TXT ---------------- */

static int parse_zone(const char *s, int *min)
{
    int sign = 1, h = 0, m = 0;
    if (*s == '+') s++; else if (*s == '-') { sign = -1; s++; }
    if (*s < '0' || *s > '9') return 0;
    while (*s >= '0' && *s <= '9') h = h * 10 + (*s++ - '0');
    if (*s == ':') { s++; while (*s >= '0' && *s <= '9') m = m * 10 + (*s++ - '0'); }
    if (*s || h > 14 || m > 59) return 0;
    *min = sign * (h * 60 + m);
    return 1;
}

static void words(char *b, char **w, int *k, int max)
{
    *k = 0;
    while (*b && *k < max) {
        while (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n') *b++ = 0;
        if (!*b) break;
        w[(*k)++] = b;
        while (*b && *b != ' ' && *b != '\t' && *b != '\r' && *b != '\n') b++;
    }
}

/* core 0, at boot */
void clock_prepare(void)
{
    struct fat_file f;
    char b[160];
    if (!fat_mounted() || fat_open("/TIME.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    char *line = b;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *w[4]; int k;
        words(line, w, &k, 4);
        if (k >= 2 && !strcmp(w[0], "server")) {
            int i = 0;
            if (strcmp(w[1], "off")) for (; w[1][i] && i < (int)sizeof(server) - 1; i++) server[i] = w[1][i];
            server[i] = 0;
        } else if (k >= 2 && !strcmp(w[0], "zone")) {
            int m;
            if (parse_zone(w[1], &m)) zone_min = m;
            zone_eu = k >= 3 && !strcmp(w[2], "eu");
        }
        line = nl ? nl + 1 : 0;
    }
}

static void save(void)
{
    char b[120];
    int n = 0;
    const char *s = "server ";
    while (*s) b[n++] = *s++;
    s = server[0] ? server : "off";
    while (*s) b[n++] = *s++;
    s = "\r\nzone ";
    while (*s) b[n++] = *s++;
    int a = zone_min < 0 ? -zone_min : zone_min;
    b[n++] = zone_min < 0 ? '-' : '+';
    if (a / 60 >= 10) b[n++] = (char)('0' + a / 600);
    b[n++] = (char)('0' + a / 60 % 10);
    if (a % 60) { b[n++] = ':'; b[n++] = (char)('0' + a % 60 / 10); b[n++] = (char)('0' + a % 10); }
    s = zone_eu ? " eu\r\n" : " none\r\n";
    while (*s) b[n++] = *s++;
    if (!fat_mounted() || fat_write_file("/TIME.TXT", b, (u32)n)) kprintf("(not saved: no SD card or write error)\n");
}

/* ---------------- command: time ---------------- */

static int num(const char *s, int len)
{
    int v = 0;
    for (int i = 0; i < len; i++) { if (s[i] < '0' || s[i] > '9') return -1; v = v * 10 + (s[i] - '0'); }
    return v;
}

void time_cmd(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "server")) {
        int i = 0;
        if (strcmp(argv[2], "off")) for (; argv[2][i] && i < (int)sizeof(server) - 1; i++) server[i] = argv[2][i];
        server[i] = 0;
        save();
        if (server[0]) want_sync = 1;
    } else if (argc > 2 && !strcmp(argv[1], "zone")) {
        int m;
        if (!parse_zone(argv[2], &m)) { kprintf("zone: an offset from UTC like +1, -5, +5:30\n"); return; }
        zone_min = m;
        zone_eu = argc > 3 && !strcmp(argv[3], "eu");
        save();
    } else if (argc > 1 && !strcmp(argv[1], "sync")) {
        if (!server[0]) { kprintf("no NTP server ('time server pool.ntp.org')\n"); return; }
        want_sync = 1;
        u64 was = synced_us, end = timer_us() + 5000000;
        while (synced_us == was && timer_us() < end)
            if (con_trygetc() == 3) break;
        if (synced_us == was) kprintf("no answer from %s (network up? 'ip')\n", server);
    } else if (argc > 3 && !strcmp(argv[1], "set")) {
        /* time set 2026-11-18 21:05[:07]  (local time) */
        const char *d = argv[2], *t = argv[3];
        int y = num(d, 4), mo = num(d + 5, 2), da = num(d + 8, 2), h = num(t, 2), mi = num(t + 3, 2);
        int s = strlen(t) >= 8 ? num(t + 6, 2) : 0;
        if (strlen(d) != 10 || d[4] != '-' || d[7] != '-' || y < 2000 || mo < 1 || mo > 12 || da < 1 || da > 31 ||
            h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) {
            kprintf("usage: time set YYYY-MM-DD HH:MM[:SS]   (local time)\n");
            return;
        }
        u64 local = (u64)days_from_civil(y, mo, da) * 86400 + (u64)(h * 3600 + mi * 60 + s);
        u64 utc = local - (u64)((long)zone_min * 60);
        utc -= (u64)((long)(clock_offset(utc) - zone_min) * 60);   /* summer time */
        clock_set_time(utc, 2);
    } else if (argc > 1) {
        kprintf("usage: time [sync | server <name>|off | zone <+h[:mm]> [eu] | set YYYY-MM-DD HH:MM[:SS]]\n");
        return;
    }
    char b[32];
    u64 now = clock_now();
    int off = clock_offset(now);
    if (now) {
        clock_format(b, now, 0);
        kprintf("%s  (UTC%c%d", b, off < 0 ? '-' : '+', (off < 0 ? -off : off) / 60);
        if (off % 60) kprintf(":%02d", (off < 0 ? -off : off) % 60);
        kprintf("%s)\n", zone_eu && off != zone_min ? ", summer time" : "");
    } else kprintf("time not known yet\n");
    if (set_by == 1) kprintf("from NTP %s, %u s ago\n", server, (u32)clock_synced_ago());
    else if (set_by == 2) kprintf("set by hand\n");
    int az = zone_min < 0 ? -zone_min : zone_min;
    kprintf("ntp: %s, zone: UTC%c%d", server[0] ? server : "off", zone_min < 0 ? '-' : '+', az / 60);
    if (az % 60) kprintf(":%02d", az % 60);
    kprintf("%s  (saved in /TIME.TXT)\n", zone_eu ? " + EU summer time" : "");
}
