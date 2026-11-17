/*
 * Current directory and path handling for the SD card: "cd", relative paths,
 * "." and "..", used by the shell, BASIC (LOAD / SAVE / DIR) and the loader.
 * FAT names are case-insensitive; cd stores the spelling found on the card.
 */
#include "kernel.h"
#include "sd.h"

static char cwd[PATH_MAX_LEN] = "/";

const char *cwd_get(void) { return cwd; }

/* in (absolute, or relative to the current directory) -> clean absolute path.
 * 0 = ok, -1 = too long */
int path_abs(const char *in, char *out, int max)
{
    char tmp[PATH_MAX_LEN * 2];
    int n = 0;
    if (in[0] != '/')                                   /* relative: start in cwd */
        for (const char *s = cwd; *s && n < (int)sizeof(tmp) - 2; s++) tmp[n++] = *s;
    tmp[n++] = '/';
    for (const char *s = in; *s && n < (int)sizeof(tmp) - 1; s++) tmp[n++] = *s;
    tmp[n] = 0;

    int o = 0;                                          /* out without the trailing '/' */
    for (char *p = tmp; *p; ) {
        while (*p == '/') p++;
        if (!*p) break;
        char *c = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - c);
        if (len == 1 && c[0] == '.') continue;
        if (len == 2 && c[0] == '.' && c[1] == '.') {  /* one level up */
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            continue;
        }
        if (o + 1 + len >= max) return -1;
        out[o++] = '/';
        memcpy(out + o, c, len);
        o += len;
    }
    if (!o) out[o++] = '/';
    out[o] = 0;
    return 0;
}

/* ---- spelling as stored on the card ---- */
struct cn { const char *name; int len; char found[FAT_MAX_NAME]; };

static int cn_cb(const struct fat_dirent *e, void *ctx)
{
    struct cn *c = ctx;
    int i = 0;
    for (; i < c->len && e->name[i]; i++) {
        char a = e->name[i], b = c->name[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    if (i != c->len || e->name[i]) return 0;
    memcpy(c->found, e->name, FAT_MAX_NAME);
    return 1;
}

static void canon(char *p)
{
    char parent[PATH_MAX_LEN];
    int i = 1;
    while (p[i]) {
        int s = i;
        while (p[i] && p[i] != '/') i++;
        struct cn c;
        c.name = p + s; c.len = i - s; c.found[0] = 0;
        memcpy(parent, p, s > 1 ? s - 1 : 1);
        parent[s > 1 ? s - 1 : 1] = 0;
        fat_list(parent, cn_cb, &c);
        if (c.found[0] && (int)strlen(c.found) == c.len) memcpy(p + s, c.found, c.len);
        if (p[i]) i++;
    }
}

/* change directory: 0 = ok, -1 not found, -2 not a directory, -3 too long */
int cwd_set(const char *path)
{
    char p[PATH_MAX_LEN];
    struct fat_file f;
    if (path_abs(path, p, sizeof(p))) return -3;
    if (fat_open(p, &f)) return -1;
    if (!f.is_dir) return -2;
    canon(p);
    memcpy(cwd, p, strlen(p) + 1);
    return 0;
}
