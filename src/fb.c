/*
 * Framebuffer - text console on HDMI (8x16 font).
 * Text is kept in a shadow buffer; scrolling redraws only changed cells.
 */
#include "kernel.h"

extern const unsigned char font8x16[95][16];

#define MAX_COLS 256                       /* text up to 2048 x 1280 pixels */
#define MAX_ROWS 80

static u8  *fb;
static u32  width, height, pitch;
static int  cols, rows, cx, cy;
static u32  cur_fg = 0xC0C0C0, bg = 0x101418;

static u8   text[MAX_ROWS][MAX_COLS];      /* what should be on screen */
static u32  tcol[MAX_ROWS][MAX_COLS];
static u8   shown[MAX_ROWS][MAX_COLS];     /* what is actually on screen */
static u32  scol[MAX_ROWS][MAX_COLS];

static u32 native_w, native_h;

/* the monitor's own resolution: what the firmware set up before we asked for ours */
void fb_native(u32 *w, u32 *h) { *w = native_w; *h = native_h; }

static int pages, page_shown;

/* ask the firmware for a w x h screen with `np` pages stacked vertically */
static int fb_alloc(u32 w, u32 h, int np)
{
    mbox_lock();
    mbox[0] = 35 * 4;
    mbox[1] = 0;
    mbox[2] = 0x48003; mbox[3] = 8; mbox[4] = 8;  mbox[5] = w;  mbox[6] = h;   /* physical size */
    mbox[7] = 0x48004; mbox[8] = 8; mbox[9] = 8;  mbox[10] = w; mbox[11] = h * np; /* virtual size */
    mbox[12] = 0x48009; mbox[13] = 8; mbox[14] = 8; mbox[15] = 0; mbox[16] = 0; /* offset */
    mbox[17] = 0x48005; mbox[18] = 4; mbox[19] = 4; mbox[20] = 32;             /* 32 bpp */
    mbox[21] = 0x48006; mbox[22] = 4; mbox[23] = 4; mbox[24] = 0;              /* BGR order */
    mbox[25] = 0x40001; mbox[26] = 8; mbox[27] = 8; mbox[28] = 4096; mbox[29] = 0; /* allocate */
    mbox[30] = 0x40008; mbox[31] = 4; mbox[32] = 4; mbox[33] = 0;              /* pitch */
    mbox[34] = 0;

    if (!mbox_call(MBOX_CH_PROP) || mbox[20] != 32 || mbox[28] == 0 ||
        (np > 1 && mbox[11] < mbox[6] * (u32)np)) {
        mbox_unlock();
        return 0;
    }
    width  = mbox[5];
    height = mbox[6];
    pitch  = mbox[33];
    fb     = (u8 *)(uintptr_t)(mbox[28] & 0x3FFFFFFF);   /* bus -> ARM address */
    mbox_unlock();
    if (pitch == 0) pitch = width * 4;
    pages = np;
    page_shown = 0;
    return 1;
}

int fb_init(u32 w, u32 h)
{
    if (!native_w) {
        u32 v[2] = { 0, 0 };
        if (mbox_prop(0x00040003, v, 2) && v[0] && v[1]) { native_w = v[0]; native_h = v[1]; }
        else { native_w = 1024; native_h = 768; }
    }
    if (!w || !h) { w = native_w; h = native_h; }
    /* two pages (virtual height 2 x h) for tear-free video; one if the firmware says no */
    if (!fb_alloc(w, h, 2) && !fb_alloc(w, h, 1)) return 0;
    cols = width / 8;  if (cols > MAX_COLS) cols = MAX_COLS;
    rows = height / 16; if (rows > MAX_ROWS) rows = MAX_ROWS;
    fb_clear();
    return 1;
}

int fb_ready(void)  { return fb != 0; }
u32 fb_width(void)  { return width; }
u32 fb_height(void) { return height; }
void fb_setcolor(u32 fg) { cur_fg = fg; }
int fb_cols(void)   { return cols; }
int fb_rows(void)   { return rows; }

static int hold;                           /* 1 = a picture is on screen: text only in memory */

static void draw_cell(int x, int y)
{
    if (hold) return;
    u8 ch = text[y][x];
    u32 fg = tcol[y][x];
    const u8 *glyph = (ch >= 32 && ch < 127) ? font8x16[ch - 32] : font8x16[0];
    u8 *row = fb + (y * 16) * pitch + x * 8 * 4;
    for (int gy = 0; gy < 16; gy++, row += pitch) {
        u32 *px = (u32 *)row;
        u8 bits = glyph[gy];
        for (int gx = 0; gx < 8; gx++)
            px[gx] = (bits & (0x80 >> gx)) ? fg : bg;
    }
    shown[y][x] = ch;
    scol[y][x] = fg;
}

static void sync_screen(void)
{
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++)
            if (shown[y][x] != text[y][x] ||
                (text[y][x] != ' ' && scol[y][x] != tcol[y][x]))
                draw_cell(x, y);
}

void fb_clear(void)
{
    if (!fb) return;
    for (u32 y = 0; y < height && !hold; y++) {
        u32 *p = (u32 *)(fb + y * pitch);
        for (u32 x = 0; x < width; x++) p[x] = bg;
    }
    for (int y = 0; y < MAX_ROWS; y++)
        for (int x = 0; x < MAX_COLS; x++) {
            text[y][x] = shown[y][x] = ' ';
            tcol[y][x] = scol[y][x] = cur_fg;
        }
    cx = cy = 0;
}

static void scroll(void)
{
    memmove(text[0], text[1], sizeof(text[0]) * (rows - 1));
    memmove(tcol[0], tcol[1], sizeof(tcol[0]) * (rows - 1));
    for (int x = 0; x < cols; x++) { text[rows - 1][x] = ' '; tcol[rows - 1][x] = cur_fg; }
    sync_screen();
    cy = rows - 1;
}

static void draw_cursor(int on)
{
    if (hold) return;
    if (!on) { draw_cell(cx, cy); return; }            /* the cursor may sit on a character */
    u8 *row = fb + (cy * 16 + 14) * pitch + cx * 8 * 4;
    for (int gy = 0; gy < 2; gy++, row += pitch)
        for (int gx = 0; gx < 8; gx++)
            ((u32 *)row)[gx] = cur_fg;
}

void fb_putc(char c)
{
    if (!fb) return;
    draw_cursor(0);
    if (c == '\n') {
        cx = 0;
        if (++cy >= rows) scroll();
    } else if (c == '\r') {
        cx = 0;
    } else if (c == '\b') {
        if (cx > 0) cx--;
    } else if (c == '\t') {
        do fb_putc(' '); while (cx % 4);
    } else {
        text[cy][cx] = (u8)c;
        tcol[cy][cx] = cur_fg;
        if (shown[cy][cx] != (u8)c || (c != ' ' && scol[cy][cx] != cur_fg))
            draw_cell(cx, cy);                     /* unchanged cells cost nothing */
        if (++cx >= cols) {
            cx = 0;
            if (++cy >= rows) scroll();
        }
    }
    draw_cursor(1);
}

void fb_goto(int x, int y)
{
    if (!fb) return;
    draw_cursor(0);
    cx = x < 0 ? 0 : x >= cols ? cols - 1 : x;
    cy = y < 0 ? 0 : y >= rows ? rows - 1 : y;
    draw_cursor(1);
}

void fb_clreol(void)
{
    if (!fb) return;
    for (int x = cx; x < cols; x++)
        if (text[cy][x] != ' ' || shown[cy][x] != ' ') {
            text[cy][x] = ' ';
            draw_cell(x, cy);
        }
    draw_cursor(1);
}

/* ---------------- pictures ---------------- */

/* draw the whole text screen again (after a picture) */
void fb_redraw(void)
{
    if (!fb) return;
    for (u32 y = 0; y < height; y++) {
        u32 *p = (u32 *)(fb + y * pitch);
        for (u32 x = 0; x < width; x++) p[x] = bg;
    }
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) shown[y][x] = ' ';
    sync_screen();
    draw_cursor(1);
}

/* 1: a picture owns the screen, text goes only to memory; 0: show the text again */
void fb_hold(int on)
{
    if (!fb || on == hold) return;
    if (on) { draw_cursor(0); hold = 1; }
    else    { hold = 0; fb_redraw(); }
}

u32 *fb_row(int y) { return (u32 *)(fb + (u32)y * pitch); }

/* text straight onto the picture (not part of the console), 8x16 glyphs */
void fb_text_px(int x, int y, const char *s, u32 fg, u32 back)
{
    if (!fb) return;
    for (; *s; s++, x += 8) {
        if (x < 0 || x + 8 > (int)width || y < 0 || y + 16 > (int)height) continue;
        u8 ch = (u8)*s;
        const u8 *glyph = (ch >= 32 && ch < 127) ? font8x16[ch - 32] : font8x16[0];
        for (int gy = 0; gy < 16; gy++) {
            u32 *px = fb_row(y + gy) + x;
            for (int gx = 0; gx < 8; gx++) px[gx] = (glyph[gy] & (0x80 >> gx)) ? fg : back;
        }
    }
}

/* ---------------- pages (video) ---------------- */

int fb_pages(void) { return pages; }
u32 fb_pitch(void) { return pitch; }
u32 *fb_page_row(int page, int y) { return (u32 *)(fb + ((u32)page * height + (u32)y) * pitch); }

/* show page 0 or 1 (the console always draws on page 0) */
void fb_show_page(int page)
{
    if (!fb || page >= pages || page == page_shown) return;
    u32 v[2] = { 0, (u32)page * height };
    mbox_prop(0x00048009, v, 2);
    page_shown = page;
}
