/*
 * Pictures on the HDMI screen: JPEG, PNG, BMP, GIF (first frame), decoded by
 * stb_image (src/stbimg.c) into RGBA in the heap and drawn scaled down to fit
 * (box filter, aspect kept, never enlarged); transparent parts let the screen
 * behind them show through.
 *
 *   view [file|dir]       full-screen viewer, arrows = previous / next picture
 *   /SPLASH.PNG|JPG|BMP   shown while RoboOS boots
 *   BASIC IMAGE, C drawImage()
 */
#include "kernel.h"
#include "sd.h"

unsigned char *stbi_load_from_memory(const unsigned char *buf, int len, int *x, int *y, int *comp, int req);
const char *stbi_failure_reason(void);

#define MAX_FILE (64u << 20)

static const char *err = "";
const char *img_error(void) { return err; }

/* file -> RGBA pixels in the heap (the heap is reset first) */
static u8 *load(const char *path, int *w, int *h)
{
    struct fat_file f;
    heap_reset();
    if (!fat_mounted() || fat_open(path, &f) || f.is_dir) { err = "file not found"; return 0; }
    if (f.size > MAX_FILE) { err = "file too big (max 64 MB)"; return 0; }
    u8 *buf = heap_alloc(f.size);
    if (!buf) { err = "out of memory"; return 0; }
    if (fat_read(&f, buf, f.size) != (int)f.size) { err = "read error"; return 0; }
    int n;
    u8 *px = stbi_load_from_memory(buf, (int)f.size, w, h, &n, 4);
    if (!px) { err = stbi_failure_reason(); if (!err) err = "cannot decode"; return 0; }
    return px;
}

/* draw scaled down to fit the box (bx, by, bw, bh); center = in the middle of it */
static void draw_fit(const u8 *px, int w, int h, int bx, int by, int bw, int bh, int center)
{
    int sw = (int)fb_width(), sh = (int)fb_height();
    long ow = w, oh = h;
    if (ow > bw) { oh = oh * bw / ow; ow = bw; }
    if (oh > bh) { ow = ow * bh / oh; oh = bh; }
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    int ox = bx + (center ? (int)(bw - ow) / 2 : 0), oy = by + (center ? (int)(bh - oh) / 2 : 0);
    for (int y = 0; y < oh; y++) {
        int py = oy + y;
        if (py < 0 || py >= sh) continue;
        int y0 = (int)((long)y * h / oh), y1 = (int)((long)(y + 1) * h / oh);
        if (y1 <= y0) y1 = y0 + 1;
        int sty = (y1 - y0 + 3) / 4;                       /* at most 4 x 4 samples per pixel */
        u32 *row = fb_row(py);
        for (int x = 0; x < ow; x++) {
            int pxx = ox + x;
            if (pxx < 0 || pxx >= sw) continue;
            int x0 = (int)((long)x * w / ow), x1 = (int)((long)(x + 1) * w / ow);
            if (x1 <= x0) x1 = x0 + 1;
            int stx = (x1 - x0 + 3) / 4;
            u32 r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = y0; yy < y1; yy += sty)
                for (int xx = x0; xx < x1; xx += stx) {
                    const u8 *p = px + ((long)yy * w + xx) * 4;
                    r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; n++;
                }
            if (!a) continue;                                /* fully transparent */
            u32 al = a / n, d = row[pxx];                    /* blend over what is there */
            r = (r / n + ((d >> 16) & 255) * (255 - al)) / 255;
            g = (g / n + ((d >> 8) & 255) * (255 - al)) / 255;
            b = (b / n + (d & 255) * (255 - al)) / 255;
            row[pxx] = (r << 16) | (g << 8) | b;
        }
    }
}

static void fill(int x, int y, int w, int h, u32 c)
{
    for (int yy = y; yy < y + h && yy < (int)fb_height(); yy++) {
        u32 *row = fb_row(yy);
        for (int xx = x; xx < x + w && xx < (int)fb_width(); xx++) row[xx] = c;
    }
}

/* a picture into the box (w or h 0 = up to the screen edge); 0 = ok */
int img_draw(const char *path, int x, int y, int w, int h, int center)
{
    int iw, ih;
    if (!fb_ready()) { err = "no HDMI screen"; return -1; }
    if (w <= 0) w = (int)fb_width() - x;
    if (h <= 0) h = (int)fb_height() - y;
    u8 *px = load(path, &iw, &ih);
    if (!px) return -1;
    draw_fit(px, iw, ih, x, y, w, h, center);
    heap_reset();
    return 0;
}

/* ---------------- boot picture ---------------- */

static int splash_on;
static u64 splash_t0;

void img_splash_start(void)
{
    static const char *names[] = { "/SPLASH.PNG", "/SPLASH.JPG", "/SPLASH.BMP" };
    struct fat_file f;
    if (!fb_ready() || !fat_mounted()) return;
    for (u32 i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (fat_open(names[i], &f)) continue;
        int w, h;
        u8 *px = load(names[i], &w, &h);
        if (!px) return;
        fb_hold(1);                                         /* boot messages wait in memory */
        fill(0, 0, (int)fb_width(), (int)fb_height(), 0);
        draw_fit(px, w, h, 0, 0, (int)fb_width(), (int)fb_height(), 1);
        heap_reset();
        splash_on = 1;
        splash_t0 = timer_us();
        return;
    }
}

/* keep the picture at least 2.5 s (a key ends it sooner), then the text */
void img_splash_end(void)
{
    if (!splash_on) return;
    while (timer_us() - splash_t0 < 2500000)
        if (con_trygetc() >= 0) break;
    splash_on = 0;
    fb_hold(0);
}

/* ---------------- viewer ---------------- */

#define MAX_PICS 256
static char pics[MAX_PICS][FAT_MAX_NAME];
static int npics;

static int is_picture(const char *n)
{
    static const char *ext[] = { ".jpg", ".jpeg", ".png", ".bmp", ".gif" };
    int len = (int)strlen(n);
    for (u32 i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
        int el = (int)strlen(ext[i]), k = 0;
        if (len <= el) continue;
        while (k < el && (n[len - el + k] | 32) == ext[i][k]) k++;
        if (k == el) return 1;
    }
    return 0;
}

static int collect(const struct fat_dirent *e, void *ctx)
{
    (void)ctx;
    if (!e->is_dir && is_picture(e->name) && npics < MAX_PICS) memcpy(pics[npics++], e->name, FAT_MAX_NAME);
    return 0;
}

static int same_name(const char *a, const char *b)
{
    while (*a && *b && (*a | 32) == (*b | 32)) { a++; b++; }
    return !*a && !*b;
}

static void put_num(char *s, int *n, u32 v)
{
    char t[12]; int k = 0;
    do { t[k++] = '0' + v % 10; v /= 10; } while (v);
    while (k) s[(*n)++] = t[--k];
}

static void show(const char *dir, int i)
{
    char path[PATH_MAX_LEN + FAT_MAX_NAME], bar[160];
    int n = 0, w = 0, h = 0, sw = (int)fb_width(), sh = (int)fb_height();
    for (const char *s = dir; *s; s++) path[n++] = *s;
    if (n > 1) path[n++] = '/';
    for (const char *s = pics[i]; *s; s++) path[n++] = *s;
    path[n] = 0;

    fill(0, 0, sw, sh, 0);
    fb_text_px(8, sh - 16, "loading ", 0x808080, 0);
    fb_text_px(72, sh - 16, pics[i], 0x808080, 0);
    u8 *px = load(path, &w, &h);
    fill(0, sh - 16, sw, 16, 0);
    if (px) draw_fit(px, w, h, 0, 0, sw, sh - 16, 1);
    heap_reset();

    n = 0;                                                  /* the bar at the bottom */
    for (const char *s = pics[i]; *s && n < 60; s++) bar[n++] = *s;
    bar[n++] = ' '; bar[n++] = ' ';
    if (px) { put_num(bar, &n, (u32)w); bar[n++] = 'x'; put_num(bar, &n, (u32)h); }
    else for (const char *s = err; *s && n < 120; s++) bar[n++] = *s;
    bar[n++] = ' '; bar[n++] = ' ';
    put_num(bar, &n, (u32)i + 1); bar[n++] = '/'; put_num(bar, &n, (u32)npics);
    for (const char *s = "   <- -> next, Q back"; *s; s++) bar[n++] = *s;
    bar[n] = 0;
    fb_text_px(8, sh - 16, bar, px ? 0xC0C0C0 : 0xFF5555, 0);
}

void view_cmd(int argc, char **argv)
{
    char p[PATH_MAX_LEN], dir[PATH_MAX_LEN];
    const char *file = 0;
    struct fat_file f;
    if (!fb_ready()) { kprintf("view needs the HDMI screen\n"); return; }
    if (!fat_mounted()) { kprintf("no SD card filesystem\n"); return; }
    if (path_abs(argc > 1 ? argv[1] : ".", p, sizeof(p)) || fat_open(p, &f)) {
        kprintf("not found: %s\n", argv[1]);
        return;
    }
    memcpy(dir, p, strlen(p) + 1);
    if (!f.is_dir) {                                        /* a file: its directory, start there */
        int s = (int)strlen(dir);
        while (s > 0 && dir[s] != '/') s--;
        file = p + s + 1;
        dir[s ? s : 1] = 0;
    }
    npics = 0;
    fat_list(dir, collect, 0);
    for (int a = 1; a < npics; a++)                         /* sort by name */
        for (int b = a; b > 0 && strcmp(pics[b - 1], pics[b]) > 0; b--) {
            char t[FAT_MAX_NAME];
            memcpy(t, pics[b], FAT_MAX_NAME); memcpy(pics[b], pics[b - 1], FAT_MAX_NAME); memcpy(pics[b - 1], t, FAT_MAX_NAME);
        }
    int i = 0;
    if (file) {
        if (!is_picture(file)) { kprintf("%s: not a picture (JPEG, PNG, BMP, GIF)\n", argv[1]); return; }
        while (i < npics && !same_name(pics[i], file)) i++;
        if (i == npics) { kprintf("not found: %s\n", argv[1]); return; }
    }
    if (!npics) { kprintf("no pictures in %s\n", dir); return; }

    fb_hold(1);
    show(dir, i);
    for (;;) {
        int k = con_getkey();
        if (k == K_RIGHT || k == K_DOWN || k == K_PGDN || k == ' ' || k == 'n') {
            if (i + 1 < npics) show(dir, ++i);
        } else if (k == K_LEFT || k == K_UP || k == K_PGUP || k == 'p' || k == 0x7F || k == '\b') {
            if (i > 0) show(dir, --i);
        } else if (k == K_HOME) {
            show(dir, i = 0);
        } else if (k == K_END) {
            show(dir, i = npics - 1);
        } else if (k == 'q' || k == 'Q' || k == '\r' || k == '\n' || k == 3) {
            break;
        }
    }
    fb_hold(0);
}
