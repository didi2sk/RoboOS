/*
 * Full-screen text editor ("edit <file>").
 *
 * The whole file sits in one flat buffer at EDIT_BASE (max 1 MB); inserting
 * and deleting is a memmove. The screen is redrawn through the console
 * (con_goto / con_clreol), so it works on HDMI and on a UART terminal alike.
 * Only what changed is redrawn - the UART at 115200 baud is slow.
 */
#include "kernel.h"
#include "sd.h"

#define EDIT_BASE 0x02000000UL           /* text: right after the program area */
#define EDIT_MAX  0x00100000UL           /* 1 MB */
#define SCRATCH   (EDIT_BASE + EDIT_MAX) /* save buffer (LF -> CR LF), up to 2 MB */
#define TABW      4

enum { R_NONE, R_LINE, R_DOWN, R_FULL };

static char *const buf = (char *)EDIT_BASE;
static u32  len, pos, top;               /* text length, cursor, first visible line */
static int  left, want;                  /* horizontal scroll, wanted cursor column */
static int  width, trows, srow;          /* text area size, status row */
static int  modified, crlf;
static char name[96], msg[100];
static char clip[512];
static int  clip_len = -1;

/* ---------------- text helpers ---------------- */

static u32 line_start(u32 p) { while (p > 0 && buf[p - 1] != '\n') p--; return p; }
static u32 line_end(u32 p)   { while (p < len && buf[p] != '\n') p++; return p; }
static int advance(int col, char c) { return c == '\t' ? (col / TABW + 1) * TABW : col + 1; }

static int col_of(u32 p)
{
    int c = 0;
    for (u32 i = line_start(p); i < p; i++) c = advance(c, buf[i]);
    return c;
}

/* position in the line starting at s that is closest to visual column col */
static u32 at_col(u32 s, int col)
{
    int c = 0;
    while (s < len && buf[s] != '\n') {
        int n = advance(c, buf[s]);
        if (n > col) break;
        c = n; s++;
    }
    return s;
}

static u32 count_nl(u32 a, u32 b)
{
    u32 n = 0;
    for (; a < b; a++) if (buf[a] == '\n') n++;
    return n;
}

static void add(char *d, int *n, const char *s) { while (*s && *n < 98) d[(*n)++] = *s++; d[*n] = 0; }
static void add_num(char *d, int *n, u32 v)
{
    char t[12]; int k = 0;
    do { t[k++] = '0' + v % 10; v /= 10; } while (v);
    while (k && *n < 98) d[(*n)++] = t[--k];
    d[*n] = 0;
}

static int insert(const char *s, u32 n)
{
    if (len + n > EDIT_MAX) { int k = 0; add(msg, &k, "file is full (1 MB)"); return 0; }
    memmove(buf + pos + n, buf + pos, len - pos);
    memcpy(buf + pos, s, n);
    len += n; pos += n;
    modified = 1;
    return 1;
}

static void erase(u32 p, u32 n)
{
    memmove(buf + p, buf + p + n, len - p - n);
    len -= n;
    if (p < top) top = line_start(p);
    modified = 1;
}

/* ---------------- drawing ---------------- */

static void draw_row(int row, u32 p, int valid)
{
    con_goto(0, row);
    if (valid) {
        int c = 0;
        for (; p < len && buf[p] != '\n' && c < left + width; p++) {
            char ch = buf[p];
            int n = advance(c, ch);
            for (; c < n; c++)
                if (c >= left && c < left + width)
                    con_putc(ch == '\t' ? ' ' : (ch >= 32 && ch < 127) ? ch : '.');
        }
    }
    con_clreol();
}

static void draw_from(int first)
{
    u32 p = top;
    int past = 0;
    for (int row = 0; row < trows; row++) {
        if (row >= first) draw_row(row, p, !past);
        if (!past) {
            u32 e = line_end(p);
            if (e < len) p = e + 1; else past = 1;
        }
    }
}

static void status(void)
{
    char s[100];
    int n = 0;
    if (msg[0]) {
        add(s, &n, msg);
    } else {
        add(s, &n, " "); add(s, &n, name); add(s, &n, modified ? " *" : "");
        add(s, &n, "   Ln "); add_num(s, &n, count_nl(0, pos) + 1);
        add(s, &n, ", Col "); add_num(s, &n, (u32)col_of(pos) + 1);
        add(s, &n, "   |  ^S save  ^Q quit  ^K cut line  ^U paste");
    }
    if (n > width) n = width;
    s[n] = 0;
    con_goto(0, srow);
    con_color(msg[0] ? COL_YELLOW : COL_CYAN);
    con_puts(s);
    con_color(COL_DEFAULT);
    con_clreol();
}

/* keep the cursor on screen; returns 1 if the view moved */
static int scroll(void)
{
    int moved = 0;
    if (pos < top) { top = line_start(pos); moved = 1; }
    while ((int)count_nl(top, pos) >= trows) { top = line_end(top) + 1; moved = 1; }
    int c = col_of(pos);
    while (c < left)           { left -= 16; moved = 1; }
    while (c >= left + width)  { left += 16; moved = 1; }
    return moved;
}

/* ---------------- files ---------------- */

static int load(const char *path)
{
    struct fat_file f;
    int n = 0;
    for (; path[n] && n < (int)sizeof(name) - 1; n++) name[n] = path[n];
    name[n] = 0;
    len = pos = top = 0; left = want = 0; modified = 0; crlf = 1; msg[0] = 0;
    n = 0;

    if (fat_open(path, &f)) {                            /* new file */
        if (!fat_name_ok(path)) { kprintf("%s: %s\n", path, fat_write_error(-3)); return -1; }
        add(msg, &n, "new file");
        return 0;
    }
    if (f.is_dir) { kprintf("%s is a directory\n", path); return -1; }
    if (f.size > EDIT_MAX) { kprintf("%s is too big for the editor (max 1 MB)\n", path); return -1; }
    if (fat_read(&f, buf, f.size) != (int)f.size) { kprintf("%s: read error\n", path); return -1; }

    crlf = 0;                                            /* CR LF -> LF in memory */
    for (u32 i = 0; i < f.size; i++) {
        if (buf[i] == '\r' && i + 1 < f.size && buf[i + 1] == '\n') { crlf = 1; continue; }
        buf[len++] = buf[i];
    }
    if (!fat_name_ok(path)) add(msg, &n, "long file name: saving is not possible (8.3 names only)");
    return 0;
}

static int save(void)
{
    char *out = (char *)SCRATCH;
    u32 n = 0;
    for (u32 i = 0; i < len; i++) {
        if (buf[i] == '\n' && crlf) out[n++] = '\r';
        out[n++] = buf[i];
    }
    int r = fat_write_file(name, out, n), k = 0;
    if (r) {
        add(msg, &k, "SAVE FAILED: "); add(msg, &k, fat_write_error(r));
        return -1;
    }
    add(msg, &k, "saved "); add(msg, &k, name); add(msg, &k, " (");
    add_num(msg, &k, n); add(msg, &k, " bytes)");
    modified = 0;
    return 0;
}

/* 1 = leave the editor */
static int quit(void)
{
    if (!modified) return 1;
    int k = 0;
    add(msg, &k, "Save changes? (y = save, n = discard, c = continue editing)");
    status();
    msg[0] = 0;
    for (;;) {
        int c = con_getkey() | 32;
        if (c == 'y') return save() == 0;
        if (c == 'n') return 1;
        if (c == 'c') return 0;
    }
}

/* ---------------- main loop ---------------- */

void editor_main(const char *path)
{
    int cols, rows, redraw = R_FULL, from = 0;

    if (!fat_mounted()) { kprintf("no SD card filesystem (try 'sd init')\n"); return; }
    if (load(path)) return;
    con_size(&cols, &rows);
    width = cols - 1;                    /* the last column stays free (no auto-wrap) */
    trows = rows - 1;
    srow = rows - 1;
    con_clear();

    for (;;) {
        if (scroll()) redraw = R_FULL;
        int row = (int)count_nl(top, pos), col = col_of(pos);
        if (redraw == R_FULL)      draw_from(0);
        else if (redraw == R_DOWN) draw_from(row < from ? row : from);
        else if (redraw == R_LINE) draw_row(row, line_start(pos), 1);
        status();
        con_goto(col - left, row);
        redraw = R_NONE;
        msg[0] = 0;
        from = row;

        int k = con_getkey(), vertical = 0;
        switch (k) {
        case K_LEFT:  if (pos > 0) pos--; break;
        case K_RIGHT: if (pos < len) pos++; break;
        case K_HOME:  pos = line_start(pos); break;
        case K_END:   pos = line_end(pos); break;
        case K_UP: case K_PGUP:
            for (int i = (k == K_UP ? 1 : trows - 1); i > 0; i--) {
                u32 s = line_start(pos);
                if (!s) break;
                pos = at_col(line_start(s - 1), want);
            }
            vertical = 1;
            break;
        case K_DOWN: case K_PGDN:
            for (int i = (k == K_DOWN ? 1 : trows - 1); i > 0; i--) {
                u32 e = line_end(pos);
                if (e >= len) break;
                pos = at_col(e + 1, want);
            }
            vertical = 1;
            break;
        case K_DEL:
            if (pos < len) { redraw = buf[pos] == '\n' ? R_DOWN : R_LINE; erase(pos, 1); }
            break;
        case 0x7F: case '\b':
            if (pos > 0) { pos--; redraw = buf[pos] == '\n' ? R_DOWN : R_LINE; erase(pos, 1); }
            break;
        case '\r': case '\n':
            if (insert("\n", 1)) redraw = R_DOWN;
            break;
        case '\t':
            if (insert("    ", TABW - col % TABW)) redraw = R_LINE;
            break;
        case 11: {                                       /* Ctrl+K: cut line */
            u32 s = line_start(pos), e = line_end(pos);
            if (e - s > sizeof(clip)) { int n = 0; add(msg, &n, "line too long to cut"); break; }
            if (s == e && e == len) break;               /* empty last line */
            clip_len = (int)(e - s);
            memcpy(clip, buf + s, clip_len);
            pos = s;
            erase(s, e - s + (e < len ? 1 : 0));
            redraw = R_DOWN;
            break;
        }
        case 21:                                         /* Ctrl+U: paste line */
            if (clip_len < 0) break;
            pos = line_start(pos);
            if (insert(clip, clip_len) && insert("\n", 1)) redraw = R_DOWN;
            break;
        case 19: save(); break;                          /* Ctrl+S */
        case 17:                                         /* Ctrl+Q */
            if (quit()) { con_clear(); return; }
            break;
        case 12: con_clear(); redraw = R_FULL; break;    /* Ctrl+L */
        default:
            if (k >= 32 && k < 127) { char c = (char)k; if (insert(&c, 1)) redraw = R_LINE; }
            break;
        }
        if (!vertical) want = col_of(pos);
    }
}
