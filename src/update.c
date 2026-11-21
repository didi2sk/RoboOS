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

static void do_kernel(void)
{
    u8 *b = UPLOAD_BUF;
    u32 n = upl.got;
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
    if (argc > 1) { kprintf("usage: update [restore]\n"); return; }
    kprintf("running RoboOS %s\n", os_version());
    long n = fat_mounted() ? read_all("/kernel8.img", CMP_BUF, UPLOAD_MAX) : -1;
    if (n > 0) { kernel_version(CMP_BUF, (u32)n, v, sizeof(v)); kprintf("  kernel8.img on the card: %s (%ld bytes)\n", v[0] ? v : "not a RoboOS kernel?", n); }
    n = fat_mounted() ? read_all("/KERNEL8.BAK", CMP_BUF, UPLOAD_MAX) : -1;
    if (n > 0) { kernel_version(CMP_BUF, (u32)n, v, sizeof(v)); kprintf("  KERNEL8.BAK (the one before): %s ('update restore' puts it back)\n", v[0] ? v : "?"); }
    kprintf("Over the network (needs 'http on' and a token or an admin login):\n");
    kprintf("  curl --data-binary @kernel8.img -H \"Authorization: Bearer TOKEN\" http://%s/api/update\n", ip_address_str());
    kprintf("  curl --data-binary @ALARM.BIN -H \"Authorization: Bearer TOKEN\" \"http://%s/api/file?path=/APPS/ALARM.BIN\"\n", ip_address_str());
    kprintf("  (https:// and curl -k when HTTPS is on; ?reboot=0 keeps the board running after a kernel update)\n");
}
