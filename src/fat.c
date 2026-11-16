/*
 * Read-only FAT16 / FAT32 with long file names.
 * Supports an MBR partition table (first FAT partition) or a bare FAT volume.
 */
#include "kernel.h"
#include "sd.h"

static int  mounted, fat32;
static u32  part_lba, sec_per_clus, fat_lba, data_lba, root_lba, root_secs, root_cluster;
static u32  total_clusters, nfats_g, fatsz_g, fsinfo_lba, free_hint = 2;
static int  fsinfo_dirty;

static u8   secbuf[512] __attribute__((aligned(16)));
static u32  secbuf_lba = 0xFFFFFFFF;
static u8   fatbuf[512] __attribute__((aligned(16)));
static u32  fatbuf_lba = 0xFFFFFFFF;

static u16 rd16(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

static int read_sec(u32 lba, u8 *buf, u32 *cache)
{
    if (*cache == lba) return 0;
    if (sd_read(lba, buf, 1)) { *cache = 0xFFFFFFFF; return -1; }
    *cache = lba;
    return 0;
}

static int is_fat_bs(const u8 *b)
{
    return b[510] == 0x55 && b[511] == 0xAA && rd16(b + 11) == 512 &&
           b[13] && (b[13] & (b[13] - 1)) == 0 && b[16] >= 1;
}

int fat_mount(void)
{
    u8 *b = secbuf;
    mounted = 0;
    secbuf_lba = fatbuf_lba = 0xFFFFFFFF;
    if (!sd_ready()) return -1;
    if (read_sec(0, b, &secbuf_lba)) return -1;

    part_lba = 0;
    if (!is_fat_bs(b)) {                                 /* MBR: first FAT partition */
        if (b[510] != 0x55 || b[511] != 0xAA) return -2;
        for (int i = 0; i < 4; i++) {
            const u8 *e = b + 446 + i * 16;
            u8 type = e[4];
            if (type == 0x0B || type == 0x0C || type == 0x0E || type == 0x06 || type == 0x04) {
                part_lba = rd32(e + 8);
                break;
            }
        }
        if (!part_lba) return -3;
        if (read_sec(part_lba, b, &secbuf_lba) || !is_fat_bs(b)) return -4;
    }

    sec_per_clus   = b[13];
    u32 reserved   = rd16(b + 14);
    u32 nfats      = b[16];
    u32 root_ents  = rd16(b + 17);
    u32 tot        = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    u32 fatsz      = rd16(b + 22) ? rd16(b + 22) : rd32(b + 36);

    root_secs   = (root_ents * 32 + 511) / 512;
    fat_lba     = part_lba + reserved;
    root_lba    = fat_lba + nfats * fatsz;
    data_lba    = root_lba + root_secs;
    total_clusters = (tot - (data_lba - part_lba)) / sec_per_clus;
    fat32       = total_clusters >= 65525;
    if (total_clusters < 4085) return -5;                /* FAT12 not supported */
    root_cluster = fat32 ? rd32(b + 44) : 0;
    nfats_g = nfats; fatsz_g = fatsz;
    fsinfo_lba = fat32 && rd16(b + 48) ? part_lba + rd16(b + 48) : 0;
    free_hint = 2; fsinfo_dirty = 0;
    mounted = 1;
    return 0;
}

int fat_mounted(void) { return mounted; }
const char *fat_type(void) { return fat32 ? "FAT32" : "FAT16"; }
u32 fat_cluster_bytes(void) { return sec_per_clus * 512; }
u64 fat_volume_bytes(void) { return (u64)total_clusters * sec_per_clus * 512; }

static u32 next_cluster(u32 c)
{
    u32 off = fat32 ? c * 4 : c * 2;
    if (read_sec(fat_lba + off / 512, fatbuf, &fatbuf_lba)) return 0x0FFFFFFF;
    u32 v = fat32 ? (rd32(fatbuf + off % 512) & 0x0FFFFFFF) : rd16(fatbuf + off % 512);
    if (!fat32 && v >= 0xFFF8) v = 0x0FFFFFFF;
    return v;
}

static int end_of_chain(u32 c) { return c < 2 || c >= 0x0FFFFFF8; }
static u32 clus_lba(u32 c) { return data_lba + (c - 2) * sec_per_clus; }

/* sector number of the n-th sector of a file/dir, walking the chain as needed */
static int file_sector(struct fat_file *f, u32 index, u32 *lba)
{
    if (f->first_cluster == 0) {                         /* FAT16 root dir */
        if (index >= root_secs) return -1;
        *lba = root_lba + index;
        return 0;
    }
    u32 ci = index / sec_per_clus;
    if (ci < f->cur_index || !f->cur_cluster) { f->cur_cluster = f->first_cluster; f->cur_index = 0; }
    while (f->cur_index < ci) {
        u32 n = next_cluster(f->cur_cluster);
        if (end_of_chain(n)) return -1;
        f->cur_cluster = n;
        f->cur_index++;
    }
    *lba = clus_lba(f->cur_cluster) + index % sec_per_clus;
    return 0;
}

int fat_read(struct fat_file *f, void *buf, u32 len)
{
    u8 *out = buf;
    u32 done = 0;
    if (!f->is_dir && f->pos + len > f->size) len = f->size - f->pos;
    while (done < len) {
        u32 lba;
        if (file_sector(f, f->pos / 512, &lba)) break;
        u32 off = f->pos % 512, n = 512 - off;
        if (n > len - done) n = len - done;
        if (off == 0 && n == 512) {                      /* whole sector straight to the buffer */
            if (sd_read(lba, out + done, 1)) return -1;
        } else {
            if (read_sec(lba, secbuf, &secbuf_lba)) return -1;
            memcpy(out + done, secbuf + off, n);
        }
        done += n;
        f->pos += n;
    }
    return done;
}

/* ---------------- directories ---------------- */

static char lower_c(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int name_eq(const char *a, const char *b, int blen)
{
    int i;
    for (i = 0; i < blen && a[i]; i++)
        if (lower_c(a[i]) != lower_c(b[i])) return 0;
    return i == blen && !a[i];
}

/* iterate entries; cb gets dirent + raw 32-byte entry */
typedef int (*raw_cb)(const struct fat_dirent *e, const u8 *raw, void *ctx);

static int dir_iterate(struct fat_file *dir, raw_cb cb, void *ctx)
{
    u8 ent[32];
    char lfn[FAT_MAX_NAME];
    int have_lfn = 0;
    struct fat_dirent de;

    dir->pos = 0; dir->cur_cluster = 0; dir->cur_index = 0;
    memset(lfn, 0, sizeof(lfn));
    for (;;) {
        if (fat_read(dir, ent, 32) != 32) return 0;
        if (ent[0] == 0x00) return 0;                    /* end of directory */
        if (ent[0] == 0xE5) { have_lfn = 0; continue; }  /* deleted */
        if (ent[11] == 0x0F) {                           /* long name part */
            int seq = (ent[0] & 0x1F) - 1;
            static const u8 pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            if (ent[0] & 0x40) { memset(lfn, 0, sizeof(lfn)); have_lfn = 1; }
            for (int i = 0; i < 13; i++) {
                int idx = seq * 13 + i;
                u16 ch = rd16(ent + pos[i]);
                if (idx >= FAT_MAX_NAME - 1 || ch == 0 || ch == 0xFFFF) continue;
                lfn[idx] = ch < 128 ? (char)ch : '?';
            }
            continue;
        }
        if (ent[11] & 0x08) { have_lfn = 0; continue; } /* volume label */

        if (have_lfn && lfn[0]) {
            memcpy(de.name, lfn, FAT_MAX_NAME);
        } else {                                         /* 8.3 name */
            int n = 0;                                   /* NT case flags: 0x08 name, 0x10 ext lowercase */
            for (int i = 0; i < 8 && ent[i] != ' '; i++)
                de.name[n++] = (ent[12] & 0x08) ? lower_c(ent[i]) : ent[i];
            if (ent[8] != ' ') {
                de.name[n++] = '.';
                for (int i = 8; i < 11 && ent[i] != ' '; i++)
                    de.name[n++] = (ent[12] & 0x10) ? lower_c(ent[i]) : ent[i];
            }
            de.name[n] = 0;
        }
        have_lfn = 0;
        de.is_dir = (ent[11] & 0x10) != 0;
        de.size = rd32(ent + 28);
        int r = cb(&de, ent, ctx);
        if (r) return r;
    }
}

static void root_dir(struct fat_file *f)
{
    memset(f, 0, sizeof(*f));
    f->first_cluster = fat32 ? root_cluster : 0;
    f->is_dir = 1;
}

struct find_ctx { const char *name; int len; struct fat_file *out; };

static int find_cb(const struct fat_dirent *e, const u8 *raw, void *c)
{
    struct find_ctx *fc = c;
    if (!name_eq(e->name, fc->name, fc->len)) {
        /* also accept the 8.3 alias */
        char sn[13]; int n = 0;
        for (int i = 0; i < 8 && raw[i] != ' '; i++) sn[n++] = raw[i];
        if (raw[8] != ' ') { sn[n++] = '.'; for (int i = 8; i < 11 && raw[i] != ' '; i++) sn[n++] = raw[i]; }
        sn[n] = 0;
        if (!name_eq(sn, fc->name, fc->len)) return 0;
    }
    memset(fc->out, 0, sizeof(*fc->out));
    fc->out->first_cluster = rd16(raw + 26) | ((u32)rd16(raw + 20) << 16);
    fc->out->size = e->size;
    fc->out->is_dir = e->is_dir;
    if (e->is_dir && fc->out->first_cluster == 0) root_dir(fc->out);   /* ".." to root */
    return 1;
}

int fat_open(const char *path, struct fat_file *f)
{
    if (!mounted) return -1;
    root_dir(f);
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        const char *s = path;
        while (*path && *path != '/') path++;
        if (!f->is_dir) return -2;
        struct fat_file next;
        struct find_ctx fc = { s, (int)(path - s), &next };
        if (dir_iterate(f, find_cb, &fc) != 1) return -2;
        *f = next;
    }
    f->pos = 0; f->cur_cluster = 0; f->cur_index = 0;
    return 0;
}

struct list_ctx { int (*cb)(const struct fat_dirent *, void *); void *ctx; };

static int list_cb(const struct fat_dirent *e, const u8 *raw, void *c)
{
    (void)raw;
    struct list_ctx *lc = c;
    if (e->name[0] == '.' && (e->name[1] == 0 || (e->name[1] == '.' && e->name[2] == 0))) return 0;
    return lc->cb(e, lc->ctx);
}

int fat_list(const char *path, int (*cb)(const struct fat_dirent *e, void *ctx), void *ctx)
{
    struct fat_file d;
    if (fat_open(path, &d)) return -1;
    if (!d.is_dir) return -2;
    struct list_ctx lc = { cb, ctx };
    dir_iterate(&d, list_cb, &lc);
    return 0;
}

/* ======================= writing ======================= */

static void wr16(u8 *p, u16 v) { p[0] = v; p[1] = v >> 8; }
static void wr32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static int write_sec(u32 lba, const u8 *buf)
{
    if (buf != secbuf && lba == secbuf_lba) secbuf_lba = 0xFFFFFFFF;
    if (buf != fatbuf && lba == fatbuf_lba) fatbuf_lba = 0xFFFFFFFF;
    return sd_write(lba, buf, 1) ? -1 : 0;
}

/* set a FAT entry in every FAT copy */
static int set_fat(u32 c, u32 v)
{
    u32 off = fat32 ? c * 4 : c * 2;
    for (u32 n = 0; n < nfats_g; n++) {
        u32 lba = fat_lba + n * fatsz_g + off / 512;
        if (read_sec(lba, fatbuf, &fatbuf_lba)) return -1;
        if (fat32) wr32(fatbuf + off % 512, (rd32(fatbuf + off % 512) & 0xF0000000) | (v & 0x0FFFFFFF));
        else       wr16(fatbuf + off % 512, (u16)v);
        if (write_sec(lba, fatbuf)) return -1;
    }
    return 0;
}

/* FSInfo free-cluster count becomes "unknown" - the OS that reads it recounts */
static void fsinfo_invalidate(void)
{
    if (!fsinfo_lba || fsinfo_dirty) return;
    if (read_sec(fsinfo_lba, secbuf, &secbuf_lba)) return;
    if (rd32(secbuf) != 0x41615252) return;
    wr32(secbuf + 488, 0xFFFFFFFF);
    wr32(secbuf + 492, 0xFFFFFFFF);
    if (!write_sec(fsinfo_lba, secbuf)) fsinfo_dirty = 1;
}

static u32 alloc_cluster(u32 prev)
{
    u32 last = total_clusters + 2;
    for (u32 i = 0; i < total_clusters; i++) {
        u32 c = free_hint + i;
        if (c >= last) c -= total_clusters;
        if (next_cluster(c) == 0) {
            if (set_fat(c, 0x0FFFFFFF)) return 0;
            if (prev && set_fat(prev, c)) return 0;
            free_hint = c + 1 < last ? c + 1 : 2;
            return c;
        }
    }
    return 0;                                            /* disk full */
}

static void free_chain(u32 c)
{
    while (!end_of_chain(c)) {
        u32 n = next_cluster(c);
        set_fat(c, 0);
        c = n;
    }
}

/* "name.ext" -> 11-byte 8.3 name + NT case flags; -1 if it is not a valid 8.3 name */
static int make_83(const char *s, u8 out[11], u8 *flags)
{
    static const char ok[] = "_-~!#$%&'(){}^@`";
    int i = 0, n = 0, up = 0, lo[2] = { 0, 0 }, upc[2] = { 0, 0 }, part = 0;
    memset(out, ' ', 11);
    for (; s[i]; i++) {
        char c = s[i];
        if (c == '.') { if (part || !n) return -1; part = 1; n = 0; continue; }
        if ((part == 0 && n >= 8) || (part == 1 && n >= 3)) return -1;
        int valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        for (const char *k = ok; *k && !valid; k++) valid = (*k == c);
        if (!valid) return -1;
        if (c >= 'a' && c <= 'z') { lo[part] = 1; c -= 32; }
        else if (c >= 'A' && c <= 'Z') upc[part] = 1;
        out[part ? 8 + n : n] = c;
        n++;
    }
    (void)up;
    if (out[0] == ' ') return -1;
    if ((lo[0] && upc[0]) || (lo[1] && upc[1])) return -1;   /* mixed case needs a long name */
    *flags = (lo[0] ? 0x08 : 0) | (lo[1] ? 0x10 : 0);
    return 0;
}

const char *fat_write_error(int e)
{
    switch (e) {
    case -1: return "no filesystem";
    case -2: return "directory not found";
    case -3: return "use an 8.3 name (max 8 chars + 3 char extension)";
    case -4: return "directory full";
    case -5: return "disk full";
    case -6: return "is a directory";
    default: return "write error";
    }
}

int fat_write_file(const char *path, const void *data, u32 len)
{
    char dirpath[96];
    const char *name = path;
    u8 n83[11], flags = 0;
    if (!mounted) return -1;

    for (const char *p = path; *p; p++) if (*p == '/') name = p + 1;
    int dl = (int)(name - path);
    if (dl >= (int)sizeof(dirpath)) return -2;
    memcpy(dirpath, path, dl);
    dirpath[dl] = 0;
    if (make_83(name, n83, &flags)) return -3;

    struct fat_file dir;
    if (fat_open(dirpath, &dir) || !dir.is_dir) return -2;

    /* find the existing entry or a free slot */
    u32 slot_lba = 0, slot_off = 0, old_cluster = 0;
    int found = 0, have_free = 0;
    u32 idx;
    for (idx = 0; ; idx++) {
        u32 lba;
        if (file_sector(&dir, idx, &lba)) break;
        if (read_sec(lba, secbuf, &secbuf_lba)) return -7;
        int end = 0;
        for (u32 o = 0; o < 512; o += 32) {
            u8 *e = secbuf + o;
            if (e[0] == 0x00 || e[0] == 0xE5) {
                if (!have_free) { have_free = 1; slot_lba = lba; slot_off = o; }
                if (e[0] == 0x00) { end = 1; break; }
                continue;
            }
            if (e[11] == 0x0F || (e[11] & 0x08)) continue;
            if (!memcmp_n(e, n83, 11)) {
                if (e[11] & 0x10) return -6;
                found = 1; slot_lba = lba; slot_off = o;
                old_cluster = rd16(e + 26) | ((u32)rd16(e + 20) << 16);
                end = 1;
                break;
            }
        }
        if (end) break;
    }
    if (!found && !have_free) {                          /* grow the directory by a cluster */
        if (dir.first_cluster == 0) return -4;           /* FAT16 root is fixed size */
        u32 lastc = dir.first_cluster;
        while (!end_of_chain(next_cluster(lastc))) lastc = next_cluster(lastc);
        u32 nc = alloc_cluster(lastc);
        if (!nc) return -5;
        memset(secbuf, 0, 512);
        secbuf_lba = 0xFFFFFFFF;
        for (u32 i = 0; i < sec_per_clus; i++)
            if (write_sec(clus_lba(nc) + i, secbuf)) return -7;
        slot_lba = clus_lba(nc); slot_off = 0;
    }

    fsinfo_invalidate();

    /* write the data into a fresh cluster chain */
    const u8 *src = data;
    u32 first = 0, prev = 0, done = 0;
    while (done < len) {
        u32 c = alloc_cluster(prev);
        if (!c) { if (first) free_chain(first); return -5; }
        if (!first) first = c;
        prev = c;
        for (u32 i = 0; i < sec_per_clus && done < len; i++) {
            u32 n = len - done < 512 ? len - done : 512;
            if (n < 512) {
                memset(secbuf, 0, 512);
                memcpy(secbuf, src + done, n);
                secbuf_lba = 0xFFFFFFFF;
                if (write_sec(clus_lba(c) + i, secbuf)) return -7;
            } else if (write_sec(clus_lba(c) + i, src + done)) {
                return -7;
            }
            done += n;
        }
    }

    /* directory entry */
    if (read_sec(slot_lba, secbuf, &secbuf_lba)) return -7;
    u8 *e = secbuf + slot_off;
    if (!found) {
        memset(e, 0, 32);
        memcpy(e, n83, 11);
        e[12] = flags;
        u16 date = ((2026 - 1980) << 9) | (1 << 5) | 1;  /* no RTC: 2026-01-01 */
        wr16(e + 16, date);                               /* created */
        wr16(e + 18, date);                               /* accessed */
    }
    e[11] = 0x20;                                         /* archive */
    wr16(e + 22, 12 << 11);                               /* modified 12:00 */
    wr16(e + 24, ((2026 - 1980) << 9) | (1 << 5) | 1);
    wr16(e + 20, first >> 16);
    wr16(e + 26, first & 0xFFFF);
    wr32(e + 28, len);
    if (write_sec(slot_lba, secbuf)) return -7;

    if (found && old_cluster) free_chain(old_cluster);
    return 0;
}
