/*
 * files.c - the files on the card over the web (0.117): the /files page (also /microscope = /CAM),
 * GET /api/files?path=/CAM (list), GET /api/file?path=/CAM/X.JPG[&dl=1] (download / view),
 * POST /api/files {"delete":"/CAM/X.JPG"} (admin).
 *
 * FAT belongs to core 0: core 1 (lwIP) puts ONE job into `fj` (files_request), core 0 does it from
 * files_poll() (con_trygetc - also while the microscope runs, its key loop calls con_trygetc) and leaves
 * the answer: a JSON text (list / delete) or the whole file in FILE_BUF; core 1 sends it and frees the
 * slot (files_release, also when the client went away early).
 */
#include "kernel.h"
#include "sd.h"

#define FILE_BUF 0x20000000UL                              /* up to 256 MB (a long microscope video) */
#define FILE_MAX (256u << 20)

static struct {
    volatile int state;                                    /* 0 free, 1 queued (core 0 works), 2 answer ready */
    volatile int abandon;                                  /* the connection is gone: free when done */
    int type, err;
    u32 len;
    char path[96];
} fj;
static int flk;
static char fjson[32768];
static int jn;

static void lock(void)   { while (__atomic_test_and_set(&flk, __ATOMIC_ACQUIRE)) ; }
static void unlock(void) { __atomic_clear(&flk, __ATOMIC_RELEASE); }

static void J(const char *s) { while (*s && jn < (int)sizeof(fjson) - 2) fjson[jn++] = *s++; fjson[jn] = 0; }
static void Jn(u64 v) { char t[24]; int k = 0; do t[k++] = (char)('0' + v % 10); while ((v /= 10)); while (k && jn < (int)sizeof(fjson) - 2) fjson[jn++] = t[--k]; fjson[jn] = 0; }
static void Js(const char *s)
{
    J("\"");
    for (; *s; s++) { char c[3] = { '\\', *s, 0 }; if (*s == '"' || *s == '\\') J(c); else if ((u8)*s >= 32 && (u8)*s < 127) { c[0] = *s; c[1] = 0; J(c); } }
    J("\"");
}

/* a safe absolute path: starts with '/', no "..", printable, short */
static int path_ok(const char *p)
{
    if (p[0] != '/') return 0;
    int n = 0;
    for (const char *s = p; *s; s++, n++) {
        if ((u8)*s < 32 || (u8)*s > 126 || *s == '\\') return 0;
        if (s[0] == '.' && s[1] == '.' && (s == p || s[-1] == '/')) return 0;
    }
    return n > 0 && n < 90;
}

/* core 1: queue a job (1 list, 2 read, 3 delete); 0 ok, -1 busy, -2 bad path */
int files_request(int type, const char *path)
{
    if (!path_ok(path)) return -2;
    lock();
    if (fj.state) { unlock(); return -1; }
    int i = 0; while (path[i] && i < (int)sizeof(fj.path) - 1) { fj.path[i] = path[i]; i++; }
    fj.path[i] = 0;
    fj.type = type; fj.err = 0; fj.len = 0; fj.abandon = 0;
    __atomic_store_n(&fj.state, 1, __ATOMIC_RELEASE);
    unlock();
    return 0;
}

/* core 1: 1 = the answer (data, len, type, err), 0 = not yet */
int files_take(const u8 **p, u32 *len, int *type, int *err)
{
    if (__atomic_load_n(&fj.state, __ATOMIC_ACQUIRE) != 2) return 0;
    *type = fj.type; *err = fj.err;
    if (fj.type == 2 && !fj.err) { *p = (const u8 *)FILE_BUF; *len = fj.len; }
    else { *p = (const u8 *)fjson; *len = (u32)jn; }
    return 1;
}

/* core 1: the answer was sent (or the client is gone) */
void files_release(void)
{
    lock();
    if (fj.state == 2) fj.state = 0;
    else if (fj.state == 1) fj.abandon = 1;
    unlock();
}

const char *files_path(void) { return fj.path; }

struct lctx { int n, more; };
static int list_cb(const struct fat_dirent *e, void *ctx)
{
    struct lctx *c = ctx;
    if (!strcmp(e->name, ".") || !strcmp(e->name, "..")) return 0;
    if (jn > (int)sizeof(fjson) - 200) { c->more = 1; return 1; }
    J(c->n ? ",{\"n\":" : "{\"n\":"); Js(e->name);
    J(",\"s\":"); Jn(e->size);
    J(",\"d\":"); J(e->is_dir ? "1}" : "0}");
    c->n++;
    return 0;
}

static void err_json(const char *msg) { jn = 0; J("{\"ok\":false,\"error\":"); Js(msg); J("}\n"); }

/* boot / system files in the root are never deleted from the web */
static int protected_path(const char *p)
{
    int slashes = 0;
    for (const char *s = p; *s; s++) if (*s == '/') slashes++;
    return slashes < 2;                                    /* "/X" = a file or folder in the root */
}

/* core 0, from con_trygetc */
void files_poll(void)
{
    if (__atomic_load_n(&fj.state, __ATOMIC_ACQUIRE) != 1 || io_held()) return;
    jn = 0;
    fj.err = 0;
    if (!fat_mounted()) { fj.err = 503; err_json("no SD card"); }
    else if (fj.type == 1) {
        struct lctx c = { 0, 0 };
        J("{\"ok\":true,\"path\":"); Js(fj.path); J(",\"files\":[");
        int r = fat_list(fj.path, list_cb, &c);
        if (r) { fj.err = 404; err_json("folder not found"); }
        else { J("]"); if (c.more) J(",\"more\":true"); J("}\n"); }
    } else if (fj.type == 2) {
        struct fat_file f;
        if (fat_open(fj.path, &f)) { fj.err = 404; err_json("file not found"); }
        else if (f.is_dir) { fj.err = 400; err_json("that is a folder"); }
        else if (f.size > FILE_MAX) { fj.err = 413; err_json("file too big for the web download (256 MB)"); }
        else {
            u32 got = 0;
            while (got < f.size) {
                u32 n = f.size - got > (1u << 20) ? (1u << 20) : f.size - got;
                int r = fat_read(&f, (u8 *)FILE_BUF + got, n);
                if (r <= 0) break;
                got += (u32)r;
            }
            if (got != f.size) { fj.err = 500; err_json("read error"); }
            else fj.len = got;
        }
    } else if (fj.type == 3) {
        if (protected_path(fj.path)) { fj.err = 403; err_json("files in the root are not deleted from the web (boot / settings files)"); }
        else {
            struct fat_file f;
            int r = fat_open(fj.path, &f) ? -100 : fat_remove(fj.path);
            if (r == -100) { fj.err = 404; err_json("not found"); r = 0; goto done; }
            if (r) { fj.err = 409; err_json("could not delete (a folder must be empty)"); }
            else { J("{\"ok\":true,\"deleted\":"); Js(fj.path); J("}\n"); kprintf("\n[WEB] deleted %s\n", fj.path); }
        }
    }
done:
    lock();
    if (fj.abandon) fj.state = 0;
    else __atomic_store_n(&fj.state, 2, __ATOMIC_RELEASE);
    unlock();
}
