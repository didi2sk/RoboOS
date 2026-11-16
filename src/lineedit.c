/*
 * Line editor shared by the shell and BASIC: cursor movement (Left/Right,
 * Home/End, Ctrl+A/Ctrl+E), insert and delete in the middle of the line,
 * history (Up/Down) and an optional Tab completion hook.
 * Only '\b' and plain characters are used to redraw, so it works the same
 * on the UART terminal and on the HDMI console.
 */
#include "kernel.h"

static void back(int n) { while (n-- > 0) con_putc('\b'); }
static void put_n(const char *s, int n) { for (int i = 0; i < n; i++) con_putc(s[i]); }

/* remove the character at position cur and redraw the tail */
static void del_at(char *line, int *len, int cur)
{
    memmove(line + cur, line + cur + 1, *len - cur - 1);
    (*len)--;
    put_n(line + cur, *len - cur);
    con_putc(' ');
    back(*len - cur + 1);
}

int line_edit(char *line, int max, struct le_hist *h,
              int (*complete)(char *line, int len), void (*prompt)(void))
{
    int len = 0, cur = 0, hpos = h ? h->n : 0;
    if (max > LE_MAX) max = LE_MAX;

    for (;;) {
        int c = con_getkey(), key = c >= 256 ? c : 0;
        if (key == K_PGUP || key == K_PGDN) continue;
        if (c == 1) key = K_HOME;                   /* Ctrl+A */
        else if (c == 5) key = K_END;                    /* Ctrl+E */

        if (key == K_LEFT)       { if (cur) { back(1); cur--; } }
        else if (key == K_RIGHT) { if (cur < len) con_putc(line[cur++]); }
        else if (key == K_HOME)  { back(cur); cur = 0; }
        else if (key == K_END)   { put_n(line + cur, len - cur); cur = len; }
        else if (key == K_DEL)   { if (cur < len) del_at(line, &len, cur); }
        else if (key == K_UP || key == K_DOWN) {
            if (!h) continue;
            int oldest = h->n > LE_HIST ? h->n - LE_HIST : 0, target = hpos;
            if (key == K_UP && hpos > oldest) target = hpos - 1;
            if (key == K_DOWN && hpos < h->n) target = hpos + 1;
            if (target == hpos) continue;
            hpos = target;
            back(cur);                                   /* wipe the old line */
            for (int i = 0; i < len; i++) con_putc(' ');
            back(len);
            len = 0;
            if (hpos < h->n)
                for (const char *s = h->line[hpos % LE_HIST]; s[len] && len < max - 1; len++)
                    line[len] = s[len];
            put_n(line, len);
            cur = len;
        }
        else if (c == '\r' || c == '\n') {
            con_putc('\n');
            line[len] = 0;
            if (h && len && (!h->n || strcmp(h->line[(h->n - 1) % LE_HIST], line))) {
                memcpy(h->line[h->n % LE_HIST], line, len + 1);
                h->n++;
            }
            return len;
        }
        else if (c == 0x7F || c == '\b') { if (cur) { back(1); cur--; del_at(line, &len, cur); } }
        else if (c == 3) {                               /* Ctrl+C */
            con_puts("^C\n");
            line[0] = 0;
            return -1;
        }
        else if (c == 12) {                              /* Ctrl+L */
            if (!prompt) continue;
            con_clear(); prompt();
            put_n(line, len); back(len - cur);
        }
        else if (c == '\t') {
            if (complete && cur == len) { line[len] = 0; len = cur = complete(line, len); }
        }
        else if (c >= 32 && c < 127 && len < max - 1) {
            memmove(line + cur + 1, line + cur, len - cur);
            line[cur] = (char)c;
            len++;
            put_n(line + cur, len - cur);
            cur++;
            back(len - cur);
        }
    }
}
