/*
 * Framebuffer - text console on HDMI (8x16 font).
 * Text is kept in a shadow buffer; scrolling redraws only changed cells.
 */
#include "kernel.h"

extern const unsigned char font8x16[95][16];

#define MAX_COLS 240
#define MAX_ROWS 68

static u8  *fb;
static u32  width, height, pitch;
static int  cols, rows, cx, cy;
static u32  cur_fg = 0xC0C0C0, bg = 0x101418;

static u8   text[MAX_ROWS][MAX_COLS];      /* what should be on screen */
static u32  tcol[MAX_ROWS][MAX_COLS];
static u8   shown[MAX_ROWS][MAX_COLS];     /* what is actually on screen */
static u32  scol[MAX_ROWS][MAX_COLS];

int fb_init(u32 w, u32 h)
{
    mbox[0] = 35 * 4;
    mbox[1] = 0;
    mbox[2] = 0x48003; mbox[3] = 8; mbox[4] = 8;  mbox[5] = w;  mbox[6] = h;   /* physical size */
    mbox[7] = 0x48004; mbox[8] = 8; mbox[9] = 8;  mbox[10] = w; mbox[11] = h;  /* virtual size */
    mbox[12] = 0x48009; mbox[13] = 8; mbox[14] = 8; mbox[15] = 0; mbox[16] = 0; /* offset */
    mbox[17] = 0x48005; mbox[18] = 4; mbox[19] = 4; mbox[20] = 32;             /* 32 bpp */
    mbox[21] = 0x48006; mbox[22] = 4; mbox[23] = 4; mbox[24] = 0;              /* BGR order */
    mbox[25] = 0x40001; mbox[26] = 8; mbox[27] = 8; mbox[28] = 4096; mbox[29] = 0; /* allocate */
    mbox[30] = 0x40008; mbox[31] = 4; mbox[32] = 4; mbox[33] = 0;              /* pitch */
    mbox[34] = 0;

    if (!mbox_call(MBOX_CH_PROP) || mbox[20] != 32 || mbox[28] == 0)
        return 0;

    width  = mbox[5];
    height = mbox[6];
    pitch  = mbox[33];
    fb     = (u8 *)(uintptr_t)(mbox[28] & 0x3FFFFFFF);   /* bus -> ARM address */
    if (pitch == 0) pitch = width * 4;

    cols = width / 8;  if (cols > MAX_COLS) cols = MAX_COLS;
    rows = height / 16; if (rows > MAX_ROWS) rows = MAX_ROWS;
    fb_clear();
    return 1;
}

int fb_ready(void)  { return fb != 0; }
u32 fb_width(void)  { return width; }
u32 fb_height(void) { return height; }
void fb_setcolor(u32 fg) { cur_fg = fg; }

static void draw_cell(int x, int y)
{
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
    for (u32 y = 0; y < height; y++) {
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
    u8 *row = fb + (cy * 16 + 14) * pitch + cx * 8 * 4;
    for (int gy = 0; gy < 2; gy++, row += pitch)
        for (int gx = 0; gx < 8; gx++)
            ((u32 *)row)[gx] = on ? cur_fg : bg;
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
        draw_cell(cx, cy);
        if (++cx >= cols) {
            cx = 0;
            if (++cy >= rows) scroll();
        }
    }
    draw_cursor(1);
}
