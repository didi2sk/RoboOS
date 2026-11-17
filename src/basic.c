/*
 * RoboOS BASIC - a small line-numbered integer BASIC with GPIO statements.
 *
 *   10 PINMODE 17, OUT
 *   20 GPIO 17, HIGH : WAIT 500
 *   30 GPIO 17, LOW  : WAIT 500
 *   40 GOTO 20
 *
 * Numbers are 64-bit integers. Variables: names up to 8 letters/digits,
 * 1-D arrays with DIM. Programs are stored as plain text (*.BAS) on the SD card,
 * so they can be edited on a PC too. Keywords must be separated by spaces.
 */
#include "kernel.h"
#include "sd.h"

typedef long i64;

struct ctx { u64 regs[13]; };
int  ctx_save(struct ctx *c);
void ctx_restore(struct ctx *c, int v);

/* ---------------- program storage ---------------- */

#define MAX_LINES  2000
#define ARENA      65536
#define LINE_LEN   160

struct line { int num; char *text; };
static struct line lines[MAX_LINES];
static int  nlines;
static char arena[ARENA], arena_tmp[ARENA];
static int  arena_used;
static int  dirty;                     /* changed since LOAD/SAVE */
static char cur_name[64];

static void compact(void)
{
    int used = 0;
    for (int i = 0; i < nlines; i++) {
        int n = (int)strlen(lines[i].text) + 1;
        memcpy(arena_tmp + used, lines[i].text, n);
        lines[i].text = arena + used;
        used += n;
    }
    memcpy(arena, arena_tmp, used);
    arena_used = used;
}

static int find_line(int num, int *exact)       /* index of first line >= num */
{
    int lo = 0, hi = nlines;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (lines[m].num < num) lo = m + 1; else hi = m;
    }
    *exact = lo < nlines && lines[lo].num == num;
    return lo;
}

/* returns 0 ok, -1 out of memory */
static int store_line(int num, const char *text)
{
    int exact, i = find_line(num, &exact);
    while (*text == ' ') text++;
    if (!*text) {                                       /* delete */
        if (exact) { memmove(&lines[i], &lines[i + 1], (nlines - i - 1) * sizeof(lines[0])); nlines--; }
        dirty = 1;
        return 0;
    }
    int n = (int)strlen(text) + 1;
    if (arena_used + n > ARENA) compact();
    if (arena_used + n > ARENA || (!exact && nlines >= MAX_LINES)) return -1;
    char *p = arena + arena_used;
    memcpy(p, text, n);
    arena_used += n;
    if (!exact) {
        memmove(&lines[i + 1], &lines[i], (nlines - i) * sizeof(lines[0]));
        nlines++;
        lines[i].num = num;
    }
    lines[i].text = p;
    dirty = 1;
    return 0;
}

static void prog_new(void) { nlines = 0; arena_used = 0; dirty = 0; cur_name[0] = 0; }

/* ---------------- variables ---------------- */

#define MAX_VARS  128
#define POOL      16384

struct var { char name[9]; i64 val; i64 *arr; int dim; };
static struct var vars[MAX_VARS];
static int  nvars;
static i64  pool[POOL];
static int  pool_used;

static void clear_vars(void) { nvars = 0; pool_used = 0; }

/* ---------------- interpreter state ---------------- */

static const char *pc;                 /* parse position */
static int  cur;                       /* current line index, -1 = immediate */
static int  running, jumped, ended, open_line;   /* open_line: PRINT ended with ; */
static struct ctx err_ctx;
static char inbuf[LINE_LEN];

struct forent { struct var *v; i64 limit, step; int line; const char *pos; };
static struct forent fors[16];
static int nfor;
struct gosubent { int line; const char *pos; };
static struct gosubent gosubs[32];
static int ngosub;

static char keyq[16];
static int  kh, kt;
static u64  rng = 88172645463325252UL;

static void error(const char *msg)
{
    con_color(COL_RED);
    if (cur >= 0) kprintf("?%s in line %d\n", msg, lines[cur].num);
    else          kprintf("?%s\n", msg);
    con_color(COL_DEFAULT);
    running = 0;
    ctx_restore(&err_ctx, 1);
}

static void check_break(void)
{
    int c = con_trygetc();
    if (c < 0) return;
    if (c == 3) {
        con_color(COL_YELLOW);
        if (cur >= 0) kprintf("\nBreak in line %d\n", lines[cur].num); else kprintf("\nBreak\n");
        con_color(COL_DEFAULT);
        running = 0;
        ctx_restore(&err_ctx, 1);
    }
    int n = (kh + 1) % 16;
    if (n != kt) { keyq[kh] = (char)c; kh = n; }
}

/* ---------------- lexer helpers ---------------- */

static int is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static char up(char c)      { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static void skip(void)      { while (*pc == ' ' || *pc == '\t') pc++; }

/* keyword at pc (case-insensitive, must not continue as a name) */
static int kw(const char *k)
{
    skip();
    const char *p = pc;
    for (; *k; k++, p++) if (up(*p) != *k) return 0;
    if (is_alpha(k[-1]) && (is_alpha(*p) || is_digit(*p) || *p == '$')) return 0;
    pc = p;
    return 1;
}

static int accept(char c) { skip(); if (*pc == c) { pc++; return 1; } return 0; }
static void expect(char c) { if (!accept(c)) error("SYNTAX ERROR"); }

static int read_name(char *out)
{
    skip();
    if (!is_alpha(*pc)) return 0;
    int n = 0;
    while (is_alpha(*pc) || is_digit(*pc)) {
        if (n < 8) out[n++] = up(*pc);
        pc++;
    }
    out[n] = 0;
    return 1;
}

static struct var *get_var(const char *name)
{
    for (int i = 0; i < nvars; i++) if (!strcmp(vars[i].name, name)) return &vars[i];
    if (nvars >= MAX_VARS) error("TOO MANY VARIABLES");
    struct var *v = &vars[nvars++];
    memset(v, 0, sizeof(*v));
    memcpy(v->name, name, strlen(name) + 1);
    return v;
}

/* ---------------- expressions ---------------- */

static i64 expr(void);
static void check_break(void);
static int  b_idle(void) { check_break(); return 0; }   /* waiting: network runs, Ctrl+C breaks */

/* "text" at pc -> out */
static void str_lit(char *out, int max)
{
    int n = 0;
    skip();
    if (*pc != '"') error("SYNTAX ERROR");
    pc++;
    while (*pc && *pc != '"') { if (n < max - 1) out[n++] = *pc; pc++; }
    if (*pc == '"') pc++;
    out[n] = 0;
}

/* SOCKNUM(h): the next integer from a connection. Waits up to 2 s for it to start;
 * a number ends at a non-digit, or when nothing more arrives for 50 ms. 0 if none. */
static i64 sock_number(int h)
{
    i64 v = 0;
    int neg = 0, digits = 0;
    u64 end = timer_us() + 2000000, quiet = 0;
    for (;;) {
        int c = sock_getc(h);
        if (c == -1) {                                      /* nothing yet */
            u64 now = timer_us();
            if (now > end) break;
            if (digits) { if (!quiet) quiet = now + 50000; else if (now > quiet) break; }
            check_break();
            continue;
        }
        quiet = 0;
        if (c < 0) break;                                   /* closed */
        if (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); digits = 1; continue; }
        if (digits) break;                                  /* the number ended */
        neg = c == '-';
    }
    return neg ? -v : v;
}
static i64 net_ok;                     /* NETOK: last NETSEND went through (-1 / 0) */

static i64 element(struct var *v, i64 idx, int create)
{
    (void)create;
    if (!v->arr) {                                      /* auto-DIM to 10 like classic BASIC */
        if (pool_used + 11 > POOL) error("OUT OF MEMORY");
        v->arr = pool + pool_used; v->dim = 10; pool_used += 11;
    }
    if (idx < 0 || idx > v->dim) error("BAD SUBSCRIPT");
    return idx;
}

static i64 temp_c(void)
{
    u32 t[2] = { 0, 0 };
    return mbox_prop(0x00030006, t, 2) ? (i64)t[1] / 1000 : -1;
}

static i64 opt_parens(void) { if (accept('(')) expect(')'); return 0; }

static i64 primary(void)
{
    char name[9];
    skip();
    if (is_digit(*pc)) {
        i64 v = 0;
        while (is_digit(*pc)) v = v * 10 + (*pc++ - '0');
        return v;
    }
    if (*pc == '&' && up(pc[1]) == 'H') {                /* &H1F hex */
        i64 v = 0; pc += 2;
        for (;;) {
            char c = up(*pc);
            if (is_digit(c)) v = v * 16 + c - '0';
            else if (c >= 'A' && c <= 'F') v = v * 16 + c - 'A' + 10;
            else break;
            pc++;
        }
        return v;
    }
    if (accept('(')) { i64 v = expr(); expect(')'); return v; }
    if (!read_name(name)) error("SYNTAX ERROR");

    /* constants */
    if (!strcmp(name, "HIGH") || !strcmp(name, "OUT") || !strcmp(name, "ON")) return 1;
    if (!strcmp(name, "LOW") || !strcmp(name, "IN") || !strcmp(name, "OFF") || !strcmp(name, "FALSE")) return 0;
    if (!strcmp(name, "PULLUP")) return 2;
    if (!strcmp(name, "PULLDOWN")) return 3;
    if (!strcmp(name, "TRUE")) return -1;

    /* functions */
    if (!strcmp(name, "MILLIS")) { opt_parens(); return (i64)(timer_us() / 1000); }
    if (!strcmp(name, "MICROS")) { opt_parens(); return (i64)timer_us(); }
    if (!strcmp(name, "TEMP"))   { opt_parens(); return temp_c(); }
    if (!strcmp(name, "INKEY"))  {
        opt_parens(); check_break();
        if (kh == kt) return 0;
        char c = keyq[kt]; kt = (kt + 1) % 16; return (u8)c;
    }
    /* TCP / UDP (names are cut to 8 letters by read_name) */
    if (!strcmp(name, "TCPCONNE")) {                    /* TCPCONNECT("host", port) */
        char host[64];
        expect('('); str_lit(host, sizeof(host)); expect(','); i64 port = expr(); expect(')');
        return sock_tcp_connect(host, (int)port, b_idle);
    }
    if (!strcmp(name, "TCPLISTE")) { expect('('); i64 p = expr(); expect(')'); return sock_tcp_listen((int)p); }
    if (!strcmp(name, "TCPACCEP")) { expect('('); i64 h = expr(); expect(')'); check_break(); return sock_tcp_accept((int)h); }
    if (!strcmp(name, "UDPOPEN")) {                     /* UDPOPEN(port [, "host", port]) */
        char host[64];
        i64 rp = 0;
        host[0] = 0;
        expect('('); i64 lp = expr();
        if (accept(',')) { str_lit(host, sizeof(host)); expect(','); rp = expr(); }
        expect(')');
        return sock_udp_open((int)lp, host, (int)rp, b_idle);
    }
    if (!strcmp(name, "SOCKAVAI")) { expect('('); i64 h = expr(); expect(')'); check_break(); return sock_avail((int)h); }
    if (!strcmp(name, "SOCKGET"))  { expect('('); i64 h = expr(); expect(')'); check_break(); return sock_getc((int)h); }
    if (!strcmp(name, "SOCKNUM"))  { expect('('); i64 h = expr(); expect(')'); return sock_number((int)h); }
    if (!strcmp(name, "NETAVAIL")) { opt_parens(); check_break(); return net_avail(); }
    if (!strcmp(name, "NETOK"))    { opt_parens(); return net_ok; }
    if (!strcmp(name, "NETRECV")) {                      /* oldest message as a number */
        char t[NET_TEXT_MAX + 1];
        opt_parens(); check_break();
        if (net_recv(0, t, sizeof(t)) < 0) return 0;
        const char *s = t;
        int neg = 0; i64 v = 0;
        while (*s == ' ') s++;
        if (*s == '-') { neg = 1; s++; }
        while (is_digit(*s)) v = v * 10 + (*s++ - '0');
        return neg ? -v : v;
    }
    if (!strcmp(name, "ABS")) { expect('('); i64 v = expr(); expect(')'); return v < 0 ? -v : v; }
    if (!strcmp(name, "RND")) {
        expect('('); i64 n = expr(); expect(')');
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        return n > 0 ? (i64)(rng % (u64)n) : 0;
    }
    if (!strcmp(name, "GPIO")) {
        expect('('); i64 p = expr(); expect(')');
        if (p < 0 || p > 53) error("BAD PIN");
        return gpio_get((int)p);
    }
    if (!strcmp(name, "MIN") || !strcmp(name, "MAX")) {
        int mx = name[1] == 'A';
        expect('('); i64 a = expr(); expect(','); i64 b = expr(); expect(')');
        return mx ? (a > b ? a : b) : (a < b ? a : b);
    }

    struct var *v = get_var(name);
    if (accept('(')) {
        i64 i = expr(); expect(')');
        i = element(v, i, 0);                           /* may allocate v->arr */
        return v->arr[i];
    }
    return v->val;
}

static i64 unary(void)
{
    if (accept('-')) return -unary();
    if (accept('+')) return unary();
    return primary();
}

static i64 mul(void)
{
    i64 v = unary();
    for (;;) {
        if (accept('*')) v *= unary();
        else if (accept('/')) { i64 d = unary(); if (!d) error("DIVISION BY ZERO"); v /= d; }
        else if (kw("MOD"))   { i64 d = unary(); if (!d) error("DIVISION BY ZERO"); v %= d; }
        else return v;
    }
}

static i64 add(void)
{
    i64 v = mul();
    for (;;) {
        if (accept('+')) v += mul();
        else if (accept('-')) v -= mul();
        else return v;
    }
}

static i64 cmp(void)
{
    i64 a = add();
    skip();
    if (pc[0] == '<' && pc[1] == '>') { pc += 2; return a != add() ? -1 : 0; }
    if (pc[0] == '<' && pc[1] == '=') { pc += 2; return a <= add() ? -1 : 0; }
    if (pc[0] == '>' && pc[1] == '=') { pc += 2; return a >= add() ? -1 : 0; }
    if (accept('<')) return a <  add() ? -1 : 0;
    if (accept('>')) return a >  add() ? -1 : 0;
    if (accept('=')) return a == add() ? -1 : 0;
    return a;
}

static i64 notx(void) { if (kw("NOT")) return ~notx(); return cmp(); }

static i64 andx(void)
{
    i64 v = notx();
    while (kw("AND")) { i64 b = notx(); v = v & b; }
    return v;
}

static i64 expr(void)
{
    i64 v = andx();
    while (kw("OR")) { i64 b = andx(); v = v | b; }
    return v;
}

/* ---------------- statements ---------------- */

static void jump_to(int num)
{
    int exact, i = find_line(num, &exact);
    if (!exact) error("UNDEFINED LINE");
    cur = i;
    pc = lines[i].text;
    jumped = 1;
    running = 1;
}

static int at_end(void) { skip(); return *pc == 0 || *pc == ':'; }

static void st_print(void)
{
    int newline = 1;
    while (!at_end()) {
        newline = 1;
        skip();
        if (*pc == '"') {
            pc++;
            while (*pc && *pc != '"') con_putc(*pc++);
            if (*pc == '"') pc++;
        } else if (kw("CHR$")) {
            expect('('); i64 c = expr(); expect(')');
            con_putc((char)c);
        } else {
            kprintf("%ld", expr());
        }
        if (accept(';')) { newline = 0; continue; }
        if (accept(',')) { con_putc('\t'); newline = 0; continue; }
        break;
    }
    if (newline) con_putc('\n');
    open_line = !newline;
}

static void read_input_line(char *buf, int max)
{
    if (line_edit(buf, max, 0, 0, 0) < 0) { running = 0; ctx_restore(&err_ctx, 1); }
}

static void assign(struct var *v, int has_index, i64 idx, i64 val)
{
    if (has_index) { idx = element(v, idx, 1); v->arr[idx] = val; }
    else v->val = val;
}

static void st_input(void)
{
    skip();
    if (*pc == '"') {
        pc++;
        while (*pc && *pc != '"') con_putc(*pc++);
        if (*pc == '"') pc++;
        if (accept(';')) con_puts("? "); else expect(',');
    } else {
        con_puts("? ");
    }
    char name[9];
    if (!read_name(name)) error("SYNTAX ERROR");
    struct var *v = get_var(name);
    int hi = 0; i64 idx = 0;
    if (accept('(')) { idx = expr(); expect(')'); hi = 1; }
    for (;;) {
        char buf[32];
        read_input_line(buf, sizeof(buf));
        const char *s = buf;
        int neg = 0;
        while (*s == ' ') s++;
        if (*s == '-') { neg = 1; s++; }
        if (!is_digit(*s)) { con_puts("?REDO\n? "); continue; }
        i64 val = 0;
        while (is_digit(*s)) val = val * 10 + (*s++ - '0');
        assign(v, hi, idx, neg ? -val : val);
        break;
    }
}

static void st_let(void)
{
    char name[9];
    if (!read_name(name)) error("SYNTAX ERROR");
    struct var *v = get_var(name);
    int hi = 0; i64 idx = 0;
    if (accept('(')) { idx = expr(); expect(')'); hi = 1; }
    expect('=');
    assign(v, hi, idx, expr());
}

static void st_for(void)
{
    char name[9];
    if (!read_name(name)) error("SYNTAX ERROR");
    struct var *v = get_var(name);
    expect('=');
    v->val = expr();
    if (!kw("TO")) error("SYNTAX ERROR");
    i64 limit = expr(), step = 1;
    if (kw("STEP")) step = expr();
    if (!step) error("STEP 0");
    for (int i = 0; i < nfor; i++)                      /* re-entering the same loop */
        if (fors[i].v == v) { nfor = i; break; }
    if (nfor >= 16) error("FOR NESTING TOO DEEP");
    fors[nfor++] = (struct forent){ v, limit, step, cur, pc };
}

static void st_next(void)
{
    char name[9];
    struct var *v = 0;
    if (read_name(name)) v = get_var(name);
    while (nfor && v && fors[nfor - 1].v != v) nfor--;
    if (!nfor) error("NEXT WITHOUT FOR");
    struct forent *f = &fors[nfor - 1];
    f->v->val += f->step;
    if (f->step > 0 ? f->v->val <= f->limit : f->v->val >= f->limit) {
        cur = f->line;
        pc = f->pos;
        jumped = 1;
    } else {
        nfor--;
    }
}

static void st_dim(void)
{
    do {
        char name[9];
        if (!read_name(name)) error("SYNTAX ERROR");
        struct var *v = get_var(name);
        expect('('); i64 n = expr(); expect(')');
        if (n < 0 || pool_used + n + 1 > POOL) error("OUT OF MEMORY");
        v->arr = pool + pool_used; v->dim = (int)n; pool_used += (int)n + 1;
        memset(v->arr, 0, (n + 1) * sizeof(i64));
    } while (accept(','));
}

static void st_pinmode(void)
{
    i64 p = expr(); expect(','); i64 m = expr();
    if (p < 0 || p > 53) error("BAD PIN");
    gpio_func((int)p, m == 1 ? 1 : 0);
    gpio_pull((int)p, m == 2 ? 2 : m == 3 ? 1 : 0);
}

static void st_gpio(void)
{
    i64 p = expr(); expect(','); i64 v = expr();
    if (p < 0 || p > 53) error("BAD PIN");
    gpio_set((int)p, v != 0);
}

/* NETSEND "name" | "ALL", "text" | number */
static void st_netsend(void)
{
    char to[NET_NAME_MAX], text[NET_TEXT_MAX + 1];
    int n = 0;
    skip();
    if (*pc != '"') error("SYNTAX ERROR");
    pc++;
    while (*pc && *pc != '"') { if (n < NET_NAME_MAX - 1) to[n++] = *pc; pc++; }
    if (*pc == '"') pc++;
    to[n] = 0;
    expect(',');
    skip();
    n = 0;
    if (*pc == '"') {
        pc++;
        while (*pc && *pc != '"') { if (n < NET_TEXT_MAX) text[n++] = *pc; pc++; }
        if (*pc == '"') pc++;
    } else {
        i64 v = expr();
        char d[24]; int k = 0;
        u64 a = v < 0 ? (u64)-v : (u64)v;
        do { d[k++] = '0' + a % 10; a /= 10; } while (a);
        if (v < 0) text[n++] = '-';
        while (k) text[n++] = d[--k];
    }
    text[n] = 0;
    net_ok = net_send(to, text) >= 0 ? -1 : 0;
}

/* SOCKPRINT h, items   /   UDPSEND h, "host", port, items
 * items like PRINT: "text", numbers, CHR$(n); ends with CR LF unless it ends with ; */
static void sock_items(char *b, int *n, int max)
{
    int newline = 1;
    while (!at_end()) {
        newline = 1;
        skip();
        if (*pc == '"') {
            pc++;
            while (*pc && *pc != '"') { if (*n < max) b[(*n)++] = *pc; pc++; }
            if (*pc == '"') pc++;
        } else if (kw("CHR$")) {
            expect('('); i64 c = expr(); expect(')');
            if (*n < max) b[(*n)++] = (char)c;
        } else {
            i64 v = expr();
            char d[24]; int k = 0;
            u64 a = v < 0 ? (u64)-v : (u64)v;
            do { d[k++] = '0' + a % 10; a /= 10; } while (a);
            if (v < 0 && *n < max) b[(*n)++] = '-';
            while (k && *n < max) b[(*n)++] = d[--k];
        }
        if (accept(';')) { newline = 0; continue; }
        if (accept(',')) { if (*n < max) b[(*n)++] = '\t'; newline = 0; continue; }
        break;
    }
    if (newline && *n + 2 <= max) { b[(*n)++] = '\r'; b[(*n)++] = '\n'; }
}

static void st_sockprint(void)
{
    char b[512];
    int n = 0;
    i64 h = expr();
    if (!at_end()) expect(',');
    sock_items(b, &n, sizeof(b));
    net_ok = sock_write((int)h, b, n, b_idle) >= 0 ? -1 : 0;
}

static void st_udpsend(void)
{
    char b[512], host[64];
    int n = 0;
    i64 h = expr(); expect(',');
    str_lit(host, sizeof(host)); expect(',');
    i64 port = expr();
    if (!at_end()) expect(',');
    sock_items(b, &n, sizeof(b));
    net_ok = sock_sendto((int)h, host, (int)port, b, n, b_idle) >= 0 ? -1 : 0;
}

static void st_wait(void)
{
    i64 ms = expr();
    u64 end = timer_us() + (u64)(ms > 0 ? ms : 0) * 1000;
    while (timer_us() < end) check_break();
}

static void statement(void);

static void st_if(void)
{
    i64 c = expr();
    if (!kw("THEN") && !kw("GOTO")) error("SYNTAX ERROR");
    if (!c) {                                           /* jump to ELSE or skip the line */
        int q = 0;
        for (; *pc; pc++) {
            if (*pc == '"') q = !q;
            if (!q && up(pc[0]) == 'E' && up(pc[1]) == 'L' && up(pc[2]) == 'S' && up(pc[3]) == 'E' &&
                (pc[-1] == ' ' || pc[-1] == ':') && !is_alpha(pc[4]) && !is_digit(pc[4])) {
                pc += 4;
                skip();
                if (is_digit(*pc)) { jump_to((int)expr()); return; }
                statement();
                return;
            }
        }
        return;
    }
    skip();
    if (is_digit(*pc)) { jump_to((int)expr()); return; }
    statement();
}

static void statement(void)
{
    skip();
    if (!*pc || *pc == ':') return;
    if (*pc == '\'' || kw("REM")) { while (*pc) pc++; return; }
    if (*pc == '?') { pc++; st_print(); return; }
    if (kw("PRINT"))   { st_print(); return; }
    if (kw("LET"))     { st_let(); return; }
    if (kw("INPUT"))   { st_input(); return; }
    if (kw("IF"))      { st_if(); return; }
    if (kw("GOTO"))    { jump_to((int)expr()); return; }
    if (kw("GOSUB"))   {
        int target = (int)expr();
        if (ngosub >= 32) error("GOSUB NESTING TOO DEEP");
        gosubs[ngosub++] = (struct gosubent){ cur, pc };
        jump_to(target);
        return;
    }
    if (kw("RETURN"))  {
        if (!ngosub) error("RETURN WITHOUT GOSUB");
        ngosub--;
        cur = gosubs[ngosub].line; pc = gosubs[ngosub].pos; jumped = 1;
        return;
    }
    if (kw("FOR"))     { st_for(); return; }
    if (kw("NEXT"))    { st_next(); return; }
    if (kw("END") || kw("STOP")) { running = 0; ended = 1; while (*pc) pc++; cur = -1; return; }
    if (kw("DIM"))     { st_dim(); return; }
    if (kw("CLS"))     { con_clear(); return; }
    if (kw("PINMODE")) { st_pinmode(); return; }
    if (kw("GPIO"))    { st_gpio(); return; }
    if (kw("LED"))     { board_led(expr() != 0); return; }
    if (kw("WAIT") || kw("DELAY")) { st_wait(); return; }
    if (kw("NETSEND")) { st_netsend(); return; }
    if (kw("SOCKPRINT")) { st_sockprint(); return; }
    if (kw("UDPSEND"))   { st_udpsend(); return; }
    if (kw("SOCKCLOSE")) { sock_close((int)expr()); return; }
    if (kw("RANDOMIZE")) { rng ^= timer_us() * 2654435761UL; if (!at_end()) rng ^= (u64)expr(); return; }
    if (is_alpha(*pc)) { st_let(); return; }
    error("SYNTAX ERROR");
}

/* execute from the current pc; follows jumps into the program */
static void execute(void)
{
    for (;;) {
        skip();
        if (!*pc) {
            if (cur < 0 || !running) return;
            if (++cur >= nlines) { running = 0; return; }
            pc = lines[cur].text;
            continue;
        }
        check_break();
        jumped = 0;
        statement();
        if (jumped) continue;
        if (ended) return;                              /* END */
        skip();
        if (kw("ELSE")) { while (*pc) pc++; continue; } /* THEN branch done */
        if (*pc == ':') { pc++; continue; }
        if (*pc) error("SYNTAX ERROR");
    }
}

static void run_program(void)
{
    sock_close_all();                                   /* connections of an earlier run */
    clear_vars();
    nfor = ngosub = 0;
    kh = kt = 0;
    rng ^= timer_us();
    if (!nlines) return;
    cur = 0;
    pc = lines[0].text;
    running = 1;
    ended = 0;
    execute();
    running = 0;
    cur = -1;
}

/* ---------------- files ---------------- */

static void basic_path(const char *name, char *out, int max)
{
    int n = 0, dot = 0, lower = 0;
    for (const char *s = name; *s && n < max - 5; s++) {
        if (*s == '.') dot = 1;
        if (*s == '/') dot = 0;
        if (*s >= 'a' && *s <= 'z') lower = 1;
        out[n++] = *s;
    }
    if (!dot) {                                         /* .bas / .BAS matching the name's case */
        const char *e = lower ? ".bas" : ".BAS";
        while (*e) out[n++] = *e++;
    }
    out[n] = 0;
}

int basic_load(const char *path)
{
    static char buf[ARENA];
    struct fat_file f;
    if (fat_open(path, &f) || f.is_dir) return -1;
    u32 len = f.size < sizeof(buf) - 1 ? f.size : sizeof(buf) - 1;
    if (fat_read(&f, buf, len) != (int)len) return -2;
    buf[len] = 0;
    prog_new();
    char *p = buf;
    while (*p) {
        char *eol = p;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        char save = *eol; *eol = 0;
        char *s = p;
        while (*s == ' ') s++;
        if (is_digit(*s)) {
            int num = 0;
            while (is_digit(*s)) num = num * 10 + (*s++ - '0');
            if (*s == ' ') s++;
            if (store_line(num, s)) return -3;
        }
        *eol = save;
        p = eol;
        while (*p == '\n' || *p == '\r') p++;
    }
    dirty = 0;
    int n = 0;
    for (const char *s = path; *s && n < (int)sizeof(cur_name) - 1; s++) cur_name[n++] = *s;
    cur_name[n] = 0;
    return 0;
}

static int basic_save(const char *path)
{
    char *b = arena_tmp;                                /* reuse the scratch arena */
    int n = 0;
    for (int i = 0; i < nlines; i++) {
        char num[12]; int k = 0, v = lines[i].num;
        do { num[k++] = '0' + v % 10; v /= 10; } while (v);
        if (n + k + (int)strlen(lines[i].text) + 4 > ARENA) return -8;
        while (k) b[n++] = num[--k];
        b[n++] = ' ';
        for (const char *s = lines[i].text; *s; s++) b[n++] = *s;
        b[n++] = '\r'; b[n++] = '\n';
    }
    int r = fat_write_file(path, b, n);
    if (!r) {
        dirty = 0;
        int k = 0;
        for (const char *s = path; *s && k < (int)sizeof(cur_name) - 1; s++) cur_name[k++] = *s;
        cur_name[k] = 0;
    }
    return r;
}

/* LOAD looks in /, /RUN and /APPS */
static int find_bas(const char *name, char *out, int max)
{
    static const char *dirs[3] = { "", "/RUN/", "/APPS/" };
    char tmp[80];
    struct fat_file f;
    basic_path(name, tmp, sizeof(tmp));
    for (int d = 0; d < 3; d++) {
        int n = 0;
        if (tmp[0] != '/') for (const char *s = dirs[d]; *s; s++) out[n++] = *s;
        for (const char *s = tmp; *s && n < max - 1; s++) out[n++] = *s;
        out[n] = 0;
        if (!fat_open(out, &f) && !f.is_dir) return 0;
        if (tmp[0] == '/') break;
    }
    return -1;
}

/* ---------------- immediate mode ---------------- */

static int str_arg(char *out, int max)
{
    skip();
    int n = 0;
    if (*pc == '"') {
        pc++;
        while (*pc && *pc != '"' && n < max - 1) out[n++] = *pc++;
        if (*pc == '"') pc++;
    } else {
        while (*pc && *pc != ' ' && n < max - 1) out[n++] = *pc++;
    }
    out[n] = 0;
    return n;
}

static void cmd_list(void)
{
    int from = 0, to = 0x7FFFFFFF;
    skip();
    if (is_digit(*pc)) { from = to = (int)primary(); }
    if (accept('-')) { to = 0x7FFFFFFF; skip(); if (is_digit(*pc)) to = (int)primary(); }
    for (int i = 0; i < nlines; i++) {
        if (lines[i].num < from || lines[i].num > to) continue;
        con_color(COL_YELLOW); kprintf("%d ", lines[i].num); con_color(COL_DEFAULT);
        kprintf("%s\n", lines[i].text);
        if (con_trygetc() == 3) break;
    }
}

static int dir_cb(const struct fat_dirent *e, void *ctx)
{
    (void)ctx;
    if (e->is_dir) { con_color(COL_CYAN); kprintf("  %s/\n", e->name); con_color(COL_DEFAULT); }
    else kprintf("  %-24s %8u\n", e->name, e->size);
    return 0;
}

static void help(void)
{
    con_color(COL_CYAN); con_puts("RoboOS BASIC\n"); con_color(COL_DEFAULT);
    con_puts("  Lines:    10 PRINT \"HI\"   (type a number alone to delete a line)\n"
             "  Commands: RUN  LIST [a-b]  NEW  SAVE \"name\"  LOAD \"name\"  DIR [path]  BYE\n"
             "  Program:  PRINT ?  INPUT  LET  IF..THEN..ELSE  GOTO  GOSUB  RETURN  FOR..TO..STEP  NEXT\n"
             "            DIM  END  REM '  CLS  RANDOMIZE   (separate statements with :)\n"
             "  GPIO:     PINMODE pin, OUT|IN|PULLUP|PULLDOWN   GPIO pin, HIGH|LOW\n"
             "            LED ON|OFF   WAIT ms   GPIO(pin)\n"
             "  Functions: ABS() RND(n) MIN(,) MAX(,) MILLIS MICROS INKEY TEMP CHR$() (in PRINT)\n"
             "  Network:  NETSEND \"name\"|\"ALL\", \"text\"|number   NETAVAIL   NETRECV   NETOK\n"
             "  TCP/UDP:  H = TCPCONNECT(\"host\", port)  TCPLISTEN(port)  TCPACCEPT(H)  UDPOPEN(port[,\"host\",port])\n"
             "            SOCKPRINT H, items   UDPSEND H, \"host\", port, items   SOCKCLOSE H\n"
             "            SOCKAVAIL(H)  SOCKGET(H) (byte)  SOCKNUM(H) (number)   errors are < 0\n"
             "  Operators: + - * / MOD  = <> < > <= >=  AND OR NOT   (integers only)\n"
             "  SAVE \"/RUN/NAME\" = start automatically after boot. Ctrl+C stops a program.\n");
}

static int immediate(void)                            /* returns 1 on BYE */
{
    pc = inbuf;
    cur = -1;
    skip();
    if (is_digit(*pc)) {                                /* program line */
        int num = 0;
        while (is_digit(*pc)) num = num * 10 + (*pc++ - '0');
        if (*pc == ' ') pc++;
        if (store_line(num, pc)) kprintf("?OUT OF MEMORY\n");
        return 0;
    }
    if (kw("BYE") || kw("EXIT") || kw("QUIT")) return 1;
    if (kw("RUN"))  { run_program(); return 0; }
    if (kw("LIST")) { cmd_list(); return 0; }
    if (kw("NEW"))  { prog_new(); clear_vars(); return 0; }
    if (kw("HELP")) { help(); return 0; }
    if (kw("DIR") || kw("FILES")) {
        char p[64];
        if (!str_arg(p, sizeof(p))) { p[0] = '/'; p[1] = 0; }
        if (!fat_mounted()) kprintf("?NO SD CARD\n");
        else if (fat_list(p, dir_cb, 0)) kprintf("?NOT FOUND\n");
        return 0;
    }
    if (kw("SAVE")) {
        char name[64], path[80];
        if (!str_arg(name, sizeof(name))) {
            if (!cur_name[0]) { kprintf("?NAME NEEDED: SAVE \"NAME\"\n"); return 0; }
            memcpy(name, cur_name, sizeof(name));
        }
        basic_path(name, path, sizeof(path));
        if (path[0] != '/') { memmove(path + 1, path, strlen(path) + 1); path[0] = '/'; }
        int r = basic_save(path);
        if (r) kprintf("?SAVE FAILED: %s\n", fat_write_error(r));
        else   kprintf("saved %s (%d lines)\n", path, nlines);
        return 0;
    }
    if (kw("LOAD")) {
        char name[64], path[96];
        if (!str_arg(name, sizeof(name))) { kprintf("?NAME NEEDED\n"); return 0; }
        if (find_bas(name, path, sizeof(path))) { kprintf("?FILE NOT FOUND\n"); return 0; }
        int r = basic_load(path);
        if (r) kprintf("?LOAD FAILED (%d)\n", r);
        else   kprintf("loaded %s (%d lines)\n", path, nlines);
        return 0;
    }
    running = 0;
    ended = 0;
    execute();                                         /* direct statement, e.g. GPIO 17,1 */
    return 0;
}

/* interactive BASIC; returns to the shell on BYE */
void basic_main(const char *file)
{
    con_color(COL_CYAN);
    kprintf("RoboOS BASIC - %u bytes free. HELP for help, BYE to exit.\n", ARENA - arena_used);
    con_color(COL_DEFAULT);
    if (file && *file) {
        char path[96];
        if (find_bas(file, path, sizeof(path)) == 0 && basic_load(path) == 0)
            kprintf("loaded %s (%d lines)\n", path, nlines);
        else
            kprintf("?FILE NOT FOUND: %s\n", file);
    }
    for (;;) {
        if (ctx_save(&err_ctx)) { running = 0; open_line = 0; continue; }
        con_color(COL_GREEN); con_puts("] "); con_color(COL_DEFAULT);
        static struct le_hist hist;
        if (line_edit(inbuf, LINE_LEN, &hist, 0, 0) <= 0) continue;
        open_line = 0;
        int bye = immediate();
        if (open_line) { con_putc('\n'); open_line = 0; }
        if (bye) break;
    }
    sock_close_all();
    if (dirty && nlines) kprintf("(program kept in memory; SAVE it to keep it after reboot)\n");
}

/* run a .BAS file non-interactively (autostart, "run x.bas") */
int basic_run_file(const char *path)
{
    if (basic_load(path)) { kprintf("cannot load %s\n", path); return -1; }
    open_line = 0;
    if (ctx_save(&err_ctx) == 0) run_program();
    sock_close_all();
    if (open_line) con_putc('\n');
    running = 0;
    cur = -1;
    return 0;
}
