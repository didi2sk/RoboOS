/*
 * Optional users for the web page and the JSON API (HTTP Basic authentication).
 * No users = everything as before (open page, API with the Bearer token only).
 * With users: the page and the API ask for a login; role "admin" may do everything,
 * "view" may only read (GET). The Bearer token still works and counts as admin.
 *
 * /USERS.TXT, one line per user:  <name> <admin|view> <salt hex> <PBKDF2-HMAC-SHA256 hex>
 * (1000 rounds, 8-byte salt from the hardware RNG - the passwords are not stored).
 * Five wrong logins from one address lock that address out for a minute.
 * The table is read on core 0 (boot, 'users' command) and checked on core 1 (lwIP
 * callbacks) - changes are made under the io lock, which core 1 holds while it serves.
 */
#include "kernel.h"
#include "sd.h"
#include "bearssl.h"

#define MAXU   8
#define ROUNDS 1000
static struct user { char name[24]; int role; u8 salt[8], hash[32]; } users[MAXU];
static int nusers;

int users_count(void) { return nusers; }

static void pbkdf2(const char *pass, int pl, const u8 salt[8], u8 out[32])
{
    br_hmac_key_context kc;
    br_hmac_context hc;
    u8 u[32], one[4] = { 0, 0, 0, 1 };
    br_hmac_key_init(&kc, &br_sha256_vtable, pass, (size_t)pl);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, salt, 8);
    br_hmac_update(&hc, one, 4);
    br_hmac_out(&hc, u);
    memcpy(out, u, 32);
    for (int i = 1; i < ROUNDS; i++) {
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, u, 32);
        br_hmac_out(&hc, u);
        for (int k = 0; k < 32; k++) out[k] ^= u[k];
    }
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }
static int unhex(const char *s, u8 *o, int n)
{
    for (int i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = a < 0 ? -1 : hexval(s[2 * i + 1]);
        if (b < 0) return -1;
        o[i] = (u8)(a << 4 | b);
    }
    return 0;
}
static char *puthex(char *d, const u8 *b, int n)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { *d++ = hx[b[i] >> 4]; *d++ = hx[b[i] & 15]; }
    return d;
}

static int name_ok(const char *n)
{
    int l = (int)strlen(n);
    if (l < 1 || l > 23) return 0;
    for (int i = 0; i < l; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

/* core 0, at boot */
void users_load(void)
{
    struct fat_file f;
    static char b[MAXU * 120 + 8];
    nusers = 0;
    if (!fat_mounted() || fat_open("/USERS.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    for (char *l = b; l && *l && nusers < MAXU; ) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        for (int k = (int)strlen(l); k && (l[k - 1] == '\r' || l[k - 1] == ' '); ) l[--k] = 0;
        char *w[4];
        int k = 0;
        for (char *p = l; *p && k < 4; ) {
            while (*p == ' ' || *p == '\r') *p++ = 0;
            if (!*p) break;
            w[k++] = p;
            while (*p && *p != ' ' && *p != '\r') p++;
        }
        if (k == 4 && name_ok(w[0]) && strlen(w[2]) == 16 && strlen(w[3]) == 64) {
            struct user *u = &users[nusers];
            int i = 0;
            for (; w[0][i]; i++) u->name[i] = w[0][i];
            u->name[i] = 0;
            u->role = !strcmp(w[1], "admin") ? 2 : 1;
            if (!unhex(w[2], u->salt, 8) && !unhex(w[3], u->hash, 32)) nusers++;
        }
        l = e ? e + 1 : 0;
    }
}

static int save(void)
{
    static char b[MAXU * 120 + 8];
    char *d = b;
    for (int i = 0; i < nusers; i++) {
        for (const char *s = users[i].name; *s; s++) *d++ = *s;
        *d++ = ' ';
        for (const char *s = users[i].role == 2 ? "admin" : "view"; *s; s++) *d++ = *s;
        *d++ = ' ';
        d = puthex(d, users[i].salt, 8);
        *d++ = ' ';
        d = puthex(d, users[i].hash, 32);
        *d++ = '\r'; *d++ = '\n';
    }
    if (!fat_mounted()) return -1;
    if (!nusers) { fat_remove("/USERS.TXT"); return 0; }
    return fat_write_file("/USERS.TXT", b, (u32)(d - b));
}

/* ---------------- login check (core 1, from the HTTP server) ---------------- */

static struct { char ip[16]; int fails; u64 until; } lock[8];

static int find_lock(const char *ip, int make)
{
    int old = 0;
    for (int i = 0; i < 8; i++) {
        if (!strcmp(lock[i].ip, ip)) return i;
        if (lock[i].until < lock[old].until) old = i;
    }
    if (!make) return -1;
    int i = 0;
    for (; i < 8 && lock[i].ip[0]; i++) ;
    if (i == 8) i = old;
    memset(&lock[i], 0, sizeof(lock[i]));
    for (int k = 0; ip[k] && k < 15; k++) lock[i].ip[k] = ip[k];
    return i;
}

static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

static void failed(const char *from, const char *what)
{
    int li = find_lock(from, 1);
    if (++lock[li].fails >= 5) {
        lock[li].fails = 0;
        lock[li].until = timer_us() + 60000000ULL;
        kprintf("\n[WEB] 5 wrong logins from %s - locked out for 60 s\n", from);
    } else kprintf("\n[WEB] wrong %s from %s\n", what, from);
}

/* also for the Bearer token (webapi.c): 1 = this address is locked out now */
int users_locked(const char *from)
{
    int li = find_lock(from, 0);
    return li >= 0 && lock[li].until > timer_us();
}
void users_failed(const char *from) { failed(from, "API token"); }
void users_ok(const char *from) { int li = find_lock(from, 0); if (li >= 0) lock[li].fails = 0; }

/*
 * auth = the Authorization header value. Returns 2 admin, 1 view, 0 no / wrong login,
 * -1 = this address is locked out (too many wrong logins). who gets the user name.
 */
int users_auth(const char *auth, const char *from, char *who, int wmax)
{
    char up[96];
    int n = 0, bits = 0;
    u32 acc = 0;
    who[0] = 0;
    int li = find_lock(from, 0);
    u64 now = timer_us();
    if (li >= 0 && lock[li].until > now) return -1;
    if (!auth) return 0;
    while (*auth == ' ') auth++;
    if (strncmp(auth, "Basic ", 6)) return 0;
    for (const char *p = auth + 6; *p && *p != '\r' && *p != '='; p++) {
        int v = b64v(*p);
        if (v < 0) { if (*p == ' ') continue; return 0; }
        acc = acc << 6 | (u32)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; if (n < (int)sizeof(up) - 1) up[n++] = (char)(acc >> bits); }
    }
    up[n] = 0;
    char *colon = strchr(up, ':');
    if (!colon) return 0;
    *colon = 0;
    const char *pass = colon + 1;
    struct user *u = 0;
    for (int i = 0; i < nusers; i++) if (!strcmp(users[i].name, up)) u = &users[i];
    u8 h[32], salt[8] = { 0 };
    pbkdf2(pass, (int)strlen(pass), u ? u->salt : salt, h);   /* the same work for an unknown name */
    u8 d = u ? 0 : 1;
    for (int i = 0; i < 32; i++) d |= h[i] ^ (u ? u->hash[i] : 0);
    if (d == 0) {
        if (li >= 0) lock[li].fails = 0;
        int i = 0;
        for (; u->name[i] && i < wmax - 1; i++) who[i] = u->name[i];
        who[i] = 0;
        return u->role;
    }
    char what[48] = "login '";
    int k = 7;
    for (const char *s = up; *s && k < 44; s++) what[k++] = *s;
    what[k++] = '\'';
    what[k] = 0;
    failed(from, what);
    return 0;
}

/* ---------------- command (core 0) ---------------- */

void users_cmd(int argc, char **argv)
{
    if (argc > 3 && (!strcmp(argv[1], "add") || !strcmp(argv[1], "passwd"))) {
        int add = argv[1][0] == 'a';
        const char *name = argv[2], *pass = argv[3];
        int role = argc > 4 ? (!strcmp(argv[4], "admin") ? 2 : !strcmp(argv[4], "view") ? 1 : 0) : 2;
        if (!role) { kprintf("role: admin or view\n"); return; }
        if (!name_ok(name)) { kprintf("name: 1-23 letters, digits, _ - .\n"); return; }
        int pl = (int)strlen(pass);
        if (pl < 6 || pl > 63) { kprintf("password: 6-63 characters\n"); return; }
        int idx = -1;
        for (int i = 0; i < nusers; i++) if (!strcmp(users[i].name, name)) idx = i;
        if (!add && idx < 0) { kprintf("no user %s\n", name); return; }
        if (add && idx < 0 && nusers >= MAXU) { kprintf("at most %d users\n", MAXU); return; }
        if (add && idx < 0 && nusers == 0 && role != 2) { kprintf("the first user must be an admin\n"); return; }
        struct user nu;
        memset(&nu, 0, sizeof(nu));
        for (int i = 0; name[i] && i < 23; i++) nu.name[i] = name[i];
        nu.role = add ? role : users[idx].role;
        rng_bytes(nu.salt, 8);
        u64 t = timer_us();
        for (int i = 0; i < 8; i++) nu.salt[i] ^= (u8)(t >> (8 * i));
        pbkdf2(pass, pl, nu.salt, nu.hash);
        io_lock();
        if (idx < 0) idx = nusers++;
        users[idx] = nu;
        io_unlock();
        if (save()) kprintf("(not saved: no SD card or write error)\n");
        kprintf("%s %s (%s)\n", add ? "user" : "password changed:", name, nu.role == 2 ? "admin" : "view");
    } else if (argc > 2 && (!strcmp(argv[1], "del") || !strcmp(argv[1], "rm"))) {
        int idx = -1;
        for (int i = 0; i < nusers; i++) if (!strcmp(users[i].name, argv[2])) idx = i;
        if (idx < 0) { kprintf("no user %s\n", argv[2]); return; }
        int admins = 0;
        for (int i = 0; i < nusers; i++) if (users[i].role == 2) admins++;
        if (users[idx].role == 2 && admins == 1 && nusers > 1) { kprintf("%s is the last admin - delete the others first\n", argv[2]); return; }
        io_lock();
        for (int i = idx; i < nusers - 1; i++) users[i] = users[i + 1];
        nusers--;
        io_unlock();
        if (save()) kprintf("(not saved: no SD card or write error)\n");
        kprintf("user %s deleted%s\n", argv[2], nusers ? "" : " - no users left: the web page is open again");
        return;
    } else if (argc > 1) {
        kprintf("usage: users [add <name> <password> [admin|view] | passwd <name> <password> | del <name>]\n");
        return;
    }
    if (!nusers) {
        kprintf("no users: the web page is open, the API needs the Bearer token ('http token')\n"
                "'users add <name> <password>' turns the login on (the first user is an admin)\n");
        return;
    }
    kprintf("%d user(s) - the web page and the API ask for a login (HTTP Basic):\n", nusers);
    for (int i = 0; i < nusers; i++) kprintf("  %-23s %s\n", users[i].name, users[i].role == 2 ? "admin (everything)" : "view (only reads)");
    if (!https_on()) kprintf("note: without HTTPS the password crosses the network readable - 'http tls on'\n");
}
