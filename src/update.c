/*
 * Updates over the network: POST /api/update (a new kernel8.img) and POST /api/file?path=/X/Y (any file).
 *
 * Core 1 (ip.c) checks the login, then streams the body into UPLOAD_BUF; when it is all there, core 0
 * (upload_poll, from con_trygetc - only core 0 may touch the FAT) writes it to the card and leaves the
 * answer in upl.answer; core 1 sends it and, for a kernel, reboots a second later.
 *
 * A kernel is checked first: it must carry the RoboOS tag (kernel_tag in main.c), so a wrong file
 * (a picture, a Linux kernel, half a download) never replaces the running one. The old kernel8.img
 * is copied to KERNEL8.BAK; the new one is written into fresh clusters and only then does the
 * directory entry switch to it (fat_write_file), then it is read back and compared.
 * If a new kernel does not boot: take the card out and rename KERNEL8.BAK to kernel8.img
 * ('update restore' does the same from a running RoboOS).
 */
#include "kernel.h"
#include "sd.h"

struct upload upl;

#define CMP_BUF ((u8 *)0x11000000UL)          /* 16 MB: the old kernel / the read-back */

static const char TAG[] = "ROBOOS-KERNEL:";

/* the version in a RoboOS kernel image ("" = no tag found) */
static void kernel_version(const u8 *b, u32 n, char *v, int vmax)
{
    int tl = (int)sizeof(TAG) - 1;
    v[0] = 0;
    for (u32 i = 0; i + (u32)tl + 2 < n; i++) {
        if (b[i] != 'R' || memcmp_n(b + i, TAG, tl)) continue;
        int k = 0;
        const u8 *p = b + i + tl;
        while (k < vmax - 1 && i + (u32)tl + (u32)k < n && p[k] != ':' && p[k] >= 32 && p[k] < 127) { v[k] = (char)p[k]; k++; }
        v[k] = 0;
        if (k && i + (u32)tl + (u32)k < n && p[k] == ':') return;
        v[0] = 0;
    }
}

/* the whole file into buf; its size, or -1 */
static long read_all(const char *path, u8 *buf, u32 max)
{
    struct fat_file f;
    if (fat_open(path, &f) || f.is_dir || f.size > max) return -1;
    u32 got = 0;
    while (got < f.size) {
        u32 want = f.size - got > 65536 ? 65536 : f.size - got;
        int r = fat_read(&f, buf + got, want);
        if (r <= 0) return -1;
        got += (u32)r;
    }
    return (long)got;
}

static int alen;
static void ans(int code, const char *json1, const char *json2)
{
    upl.code = code;
    alen = 0;
    const char *parts[2] = { json1, json2 };
    for (int k = 0; k < 2; k++)
        for (const char *s = parts[k]; s && *s && alen < (int)sizeof(upl.answer) - 2; s++) upl.answer[alen++] = *s;
    upl.answer[alen] = 0;
}

static void add_num(char *o, u32 v) { char t[12]; int k = 0; do t[k++] = (char)('0' + v % 10); while (v /= 10); int j = 0; while (k) o[j++] = t[--k]; o[j] = 0; }

static void do_kernel_buf(const u8 *b, u32 n);
static void do_kernel(void) { do_kernel_buf(UPLOAD_BUF, upl.got); }

static void do_kernel_buf(const u8 *b, u32 n)
{
    char nv[24], ov[24], num[16], msg[200];
    kernel_version(b, n, nv, sizeof(nv));
    if (!nv[0]) { ans(400, "{\"ok\":false,\"error\":\"this is not a RoboOS kernel (no RoboOS tag inside) - nothing changed\"}", 0); return; }
    if (n < 4096) { ans(400, "{\"ok\":false,\"error\":\"too small for a kernel\"}", 0); return; }
    kprintf("\n[UPDATE] kernel %s received (%u bytes), writing...\n", nv, n);
    /* the running one to KERNEL8.BAK */
    long on = read_all("/kernel8.img", CMP_BUF, UPLOAD_MAX);
    ov[0] = 0;
    if (on > 0) {
        kernel_version(CMP_BUF, (u32)on, ov, sizeof(ov));
        int e = fat_write_file("/KERNEL8.BAK", CMP_BUF, (u32)on);
        if (e) { ans(500, "{\"ok\":false,\"error\":\"could not write the backup KERNEL8.BAK: ", fat_write_error(e)); upl.answer[alen++] = '"'; upl.answer[alen++] = '}'; upl.answer[alen] = 0; return; }
    }
    int e = fat_write_file("/kernel8.img", b, n);
    if (e) {
        kprintf("[UPDATE] write failed: %s - the old kernel stays\n", fat_write_error(e));
        ans(500, "{\"ok\":false,\"error\":\"writing kernel8.img failed - the old kernel stays\"}", 0);
        return;
    }
    long rn = read_all("/kernel8.img", CMP_BUF, UPLOAD_MAX);
    if (rn != (long)n || memcmp_n(CMP_BUF, b, (int)n)) {
        kprintf("[UPDATE] read-back differs! use 'update restore' before a reboot\n");
        ans(500, "{\"ok\":false,\"error\":\"the written kernel8.img reads back different - run 'update restore' on the board, do not reboot\"}", 0);
        upl.reboot = 0;
        return;
    }
    int k = 0;
    const char *parts[] = { "{\"ok\":true,\"old\":\"", ov[0] ? ov : "?", "\",\"new\":\"", nv, "\",\"bytes\":", 0, ",\"backup\":\"/KERNEL8.BAK\",\"reboot\":", upl.reboot ? "true" : "false", "}" };
    add_num(num, n);
    parts[5] = num;
    for (int i = 0; i < 9; i++) for (const char *s = parts[i]; *s && k < (int)sizeof(msg) - 2; s++) msg[k++] = *s;
    msg[k] = 0;
    ans(200, msg, 0);
    con_color(COL_CYAN);
    kprintf("[UPDATE] kernel %s -> %s written and checked (backup: KERNEL8.BAK)%s\n", ov[0] ? ov : "?", nv,
            upl.reboot ? " - rebooting" : "");
    con_color(COL_DEFAULT);
}

static void do_file(void)
{
    const char *p = upl.path;
    if (p[0] != '/' || !fat_name_ok(p)) { ans(400, "{\"ok\":false,\"error\":\"path=/DIR/NAME.EXT with an 8.3 name, e.g. /APPS/ALARM.BIN\"}", 0); return; }
    int e = fat_write_file(p, UPLOAD_BUF, upl.got);
    if (e) {
        ans(500, "{\"ok\":false,\"error\":\"", fat_write_error(e));
        for (const char *s = "\"}"; *s; s++) upl.answer[alen++] = *s;
        upl.answer[alen] = 0;
        return;
    }
    char num[16], msg[160];
    add_num(num, upl.got);
    int k = 0;
    const char *parts[] = { "{\"ok\":true,\"path\":\"", p, "\",\"bytes\":", num, "}" };
    for (int i = 0; i < 5; i++) for (const char *s = parts[i]; *s && k < (int)sizeof(msg) - 2; s++) msg[k++] = *s;
    msg[k] = 0;
    ans(200, msg, 0);
    kprintf("\n[UPDATE] %s written (%u bytes)\n", p, upl.got);
}

/* "0.48" -> 48, "1.2" -> 1002 */
static int ver_num(const char *v)
{
    int a = 0, b = 0;
    while (*v >= '0' && *v <= '9') a = a * 10 + (*v++ - '0');
    if (*v == '.') v++;
    while (*v >= '0' && *v <= '9') b = b * 10 + (*v++ - '0');
    return a * 1000 + b;
}

/* /UPDATE.TXT: the URL (+ the word "insecure" = no certificate check); none = the RoboOS repository */
#define DEFAULT_URL "https://raw.githubusercontent.com/didi2sk/RoboOS/main/sd/kernel8.img"
static int update_url_quiet(char *url, int max, int *insecure)
{
    struct fat_file f;
    char b[320];
    int n = 0;
    if (fat_mounted() && !fat_open("/UPDATE.TXT", &f) && !f.is_dir) n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) { int k = 0; for (const char *s = DEFAULT_URL; *s && k < max - 1; s++) url[k++] = *s; url[k] = 0; return 0; }
    b[n] = 0;
    char *p = b;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!strncmp(p, "url=", 4)) p += 4;
    int k = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && k < max - 1) url[k++] = *p++;
    url[k] = 0;
    if (strstr(p, "insecure")) *insecure = 1;
    return k ? 0 : -1;
}
static int update_url(char *url, int max, int *insecure)
{
    if (update_url_quiet(url, max, insecure)) { kprintf("/UPDATE.TXT has no address ('update url https://...')\n"); return -1; }
    return 0;
}

/* core 0, from con_trygetc */
void upload_poll(void)
{
    if (upl.state != 2) return;
    u64 tw = timer_us();
    kprintf("\n[UPDATE] %u bytes received in %u ms\n", upl.got, (u32)((upl.t_rx - upl.t0) / 1000));
    if (!fat_mounted()) ans(500, "{\"ok\":false,\"error\":\"no SD card\"}", 0);
    else if (upl.kind == 1) do_kernel();
    else do_file();
    kprintf("[UPDATE] card work took %u ms\n", (u32)((timer_us() - tw) / 1000));
    __atomic_store_n(&upl.state, 3, __ATOMIC_RELEASE);
}

/* shell: update | update restore */
void cmd_update(int argc, char **argv)
{
    char v[24];
    if (argc > 1 && !strcmp(argv[1], "restore")) {
        long n = read_all("/KERNEL8.BAK", CMP_BUF, UPLOAD_MAX);
        if (n <= 0) { kprintf("no /KERNEL8.BAK on the card\n"); return; }
        kernel_version(CMP_BUF, (u32)n, v, sizeof(v));
        if (!v[0]) { kprintf("KERNEL8.BAK is not a RoboOS kernel - not restored\n"); return; }
        int e = fat_write_file("/kernel8.img", CMP_BUF, (u32)n);
        if (e) { kprintf("write failed: %s\n", fat_write_error(e)); return; }
        kprintf("kernel8.img is version %s again - reboot to use it ('reboot')\n", v);
        return;
    }
    if (argc > 2 && !strcmp(argv[1], "url")) {
        char t[300]; int k = 0;
        for (const char *s2 = argv[2]; *s2 && k < 280; s2++) t[k++] = *s2;
        if (argc > 3 && !strcmp(argv[3], "insecure")) { const char *w = " insecure"; while (*w) t[k++] = *w++; }
        t[k++] = '\r'; t[k++] = '\n';
        if (!fat_mounted() || fat_write_file("/UPDATE.TXT", t, (u32)k)) { kprintf("not saved (no SD card?)\n"); return; }
        kprintf("saved to /UPDATE.TXT - 'update check' / 'update get'\n");
        return;
    }
    if (argc > 1 && (!strcmp(argv[1], "check") || !strcmp(argv[1], "get"))) {
        int get = !strcmp(argv[1], "get"), force = 0, insecure = 0;
        for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "-k") || !strcmp(argv[i], "insecure")) insecure = 1; if (!strcmp(argv[i], "force")) force = 1; }
        char url[300];
        if (update_url(url, sizeof(url), &insecure)) return;
        kprintf("downloading%s:\n", insecure ? " (certificate NOT checked)" : "");
        long n = fetch_url(url, UPLOAD_BUF, UPLOAD_MAX, insecure);
        if (n < 0) { kprintf("download failed: %s\n", fetch_err); return; }
        kernel_version(UPLOAD_BUF, (u32)n, v, sizeof(v));
        if (!v[0]) { kprintf("%ld bytes, but that is not a RoboOS kernel - nothing changed\n", n); return; }
        int same = !strcmp(v, os_version());
        kprintf("%ld bytes: RoboOS %s (running: %s)%s\n", n, v, os_version(), same ? " - the same" : "");
        if (!get) { if (!same) kprintf("'update get' installs it\n"); return; }
        if (same && !force) { kprintf("nothing to do ('update get force' installs it anyway)\n"); return; }
        if (!same && !force && ver_num(v) < ver_num(os_version())) {
            kprintf("that is OLDER than the running %s ('update get force' installs it anyway)\n", os_version());
            return;
        }
        upl.reboot = 1;
        do_kernel_buf(UPLOAD_BUF, (u32)n);
        if (upl.code != 200) { kprintf("%s\n", upl.answer); return; }
        kprintf("rebooting in 2 s...\n");
        delay_us(2000000);
        sys_reboot();
        return;
    }
    if (argc > 1) { kprintf("usage: update [check | get [-k] [force] | url <https://...> [insecure] | restore]\n"); return; }
    kprintf("running RoboOS %s\n", os_version());
    long n = fat_mounted() ? read_all("/kernel8.img", CMP_BUF, UPLOAD_MAX) : -1;
    if (n > 0) { kernel_version(CMP_BUF, (u32)n, v, sizeof(v)); kprintf("  kernel8.img on the card: %s (%ld bytes)\n", v[0] ? v : "not a RoboOS kernel?", n); }
    n = fat_mounted() ? read_all("/KERNEL8.BAK", CMP_BUF, UPLOAD_MAX) : -1;
    if (n > 0) { kernel_version(CMP_BUF, (u32)n, v, sizeof(v)); kprintf("  KERNEL8.BAK (the one before): %s ('update restore' puts it back)\n", v[0] ? v : "?"); }
    {
        char url[300]; int ins = 0;
        if (!update_url_quiet(url, sizeof(url), &ins)) kprintf("  downloads from %s%s ('update check', 'update get')\n", url, ins ? " (insecure)" : "");
    }
    kprintf("Sent from a PC (needs 'http on' and a token or an admin login):\n");
    kprintf("  curl --data-binary @kernel8.img -H \"Authorization: Bearer TOKEN\" http://%s/api/update\n", ip_address_str());
    kprintf("  curl --data-binary @ALARM.BIN -H \"Authorization: Bearer TOKEN\" \"http://%s/api/file?path=/APPS/ALARM.BIN\"\n", ip_address_str());
    kprintf("  (https:// and curl -k when HTTPS is on; ?reboot=0 keeps the board running after a kernel update)\n");
}

/* ---------------- 0.122: update from the web page (/update, GET|POST /api/kernel) ----------------
 * Core 1 (webapi.c) only queues a job; core 0 (update_web_poll from con_trygetc) downloads from GitHub /
 * /UPDATE.TXT like 'update check' / 'update get' and leaves a message; the page polls GET /api/kernel. */
static volatile int uw_state;                 /* 0 idle, 1 check queued, 2 install queued, 3 working, 4 done */
static int uw_force, uw_ok, uw_newer, uw_reboot, uw_install;
static char uw_msg[200], uw_avail[24];
static u64 uw_since;

static void uw_set(const char *m) { int k = 0; for (; m[k] && k < (int)sizeof(uw_msg) - 1; k++) uw_msg[k] = m[k]; uw_msg[k] = 0; }

/* core 1: 0 queued, -1 busy */
int update_web_request(int install, int force)
{
    int s = __atomic_load_n(&uw_state, __ATOMIC_ACQUIRE);
    if ((s == 1 || s == 2 || s == 3) && timer_us() - uw_since < 180000000ULL) return -1;   /* (a stuck job frees itself) */
    if (uw_reboot) return -1;
    uw_force = force; uw_install = install; uw_ok = 0;
    uw_set(install ? "queued: download + install" : "queued: checking");
    uw_since = timer_us();
    __atomic_store_n(&uw_state, install ? 2 : 1, __ATOMIC_RELEASE);
    return 0;
}

/* core 0, from con_trygetc */
void update_web_poll(void)
{
    int s = __atomic_load_n(&uw_state, __ATOMIC_ACQUIRE);
    if ((s != 1 && s != 2) || io_held()) return;
    __atomic_store_n(&uw_state, 3, __ATOMIC_RELEASE);
    int get = s == 2, insecure = 0;
    char url[300], v[24], m[200];
    uw_avail[0] = 0; uw_newer = 0;
    if (upl.state) { uw_set("an upload is running - try again in a moment"); goto done; }
    if (update_url_quiet(url, sizeof(url), &insecure)) { uw_set("/UPDATE.TXT has no address"); goto done; }
    uw_set("downloading...");
    kprintf("\n[UPDATE] web: %s from %s\n", get ? "install" : "check", url);
    long n = fetch_url(url, UPLOAD_BUF, UPLOAD_MAX, insecure);
    if (n < 0) { int k = 0; const char *a = "download failed: "; while (*a) m[k++] = *a++; for (const char *e = fetch_err; e && *e && k < 190; e++) m[k++] = *e; m[k] = 0; uw_set(m); goto done; }
    kernel_version(UPLOAD_BUF, (u32)n, v, sizeof(v));
    if (!v[0]) { uw_set("the download is not a RoboOS kernel - nothing changed"); goto done; }
    { int k = 0; for (; v[k] && k < (int)sizeof(uw_avail) - 1; k++) uw_avail[k] = v[k]; uw_avail[k] = 0; }
    int same = !strcmp(v, os_version()), older = !same && ver_num(v) < ver_num(os_version());
    uw_newer = !same && !older;
    if (!get) {
        uw_set(same ? "you have the newest version" : older ? "GitHub has an OLDER version (install with 'force' only)" : "a new version is ready to install");
        uw_ok = 1;
        goto done;
    }
    if ((same || older) && !uw_force) { uw_set(same ? "the same version - nothing to do (tick 'force' to reinstall)" : "that is OLDER than the running one (tick 'force')"); goto done; }
    upl.reboot = 1;
    do_kernel_buf(UPLOAD_BUF, (u32)n);
    if (upl.code != 200) { uw_set("installing failed - the old kernel stays (see the board's screen)"); goto done; }
    {
        int k = 0; const char *a = "installed ";
        while (*a) m[k++] = *a++;
        for (const char *b = v; *b; b++) m[k++] = *b;
        a = " - rebooting, the page reconnects by itself"; while (*a) m[k++] = *a++;
        m[k] = 0;
        uw_set(m);
    }
    uw_ok = 1;
    uw_reboot = 1;
    api_reboot_later(3000);
done:
    __atomic_store_n(&uw_state, 4, __ATOMIC_RELEASE);
}

static void js_add(char *b, int *n, int max, const char *s, int quote)
{
    if (quote && *n < max - 1) b[(*n)++] = '"';
    for (; *s && *n < max - 2; s++) { if (quote && (*s == '"' || *s == '\\')) b[(*n)++] = '\\'; if ((u8)*s >= 32) b[(*n)++] = *s; }
    if (quote && *n < max - 1) b[(*n)++] = '"';
    b[*n] = 0;
}

/* any core: GET /api/kernel */
int update_web_json(char *b, int max)
{
    int n = 0, s = __atomic_load_n(&uw_state, __ATOMIC_ACQUIRE);
    static const char *const st[5] = { "idle", "checking", "installing", "working", "done" };
    js_add(b, &n, max, "{\"ok\":true,\"running\":", 0); js_add(b, &n, max, os_version(), 1);
    js_add(b, &n, max, ",\"state\":", 0); js_add(b, &n, max, s == 3 ? (uw_install ? "installing" : "checking") : st[s], 1);
    js_add(b, &n, max, ",\"busy\":", 0); js_add(b, &n, max, s >= 1 && s <= 3 ? "true" : "false", 0);
    js_add(b, &n, max, ",\"message\":", 0); js_add(b, &n, max, uw_msg, 1);
    js_add(b, &n, max, ",\"available\":", 0); js_add(b, &n, max, uw_avail, 1);
    js_add(b, &n, max, ",\"newer\":", 0); js_add(b, &n, max, uw_newer ? "true" : "false", 0);
    js_add(b, &n, max, ",\"rebooting\":", 0); js_add(b, &n, max, uw_reboot ? "true" : "false", 0);
    js_add(b, &n, max, "}\n", 0);
    return n;
}
