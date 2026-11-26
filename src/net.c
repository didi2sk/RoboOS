/*
 * RoboOS messages: a tiny protocol for RoboOS boards on the same Ethernet.
 * Raw frames with the "local experimental" EtherType 0x88B5 - no IP, no UDP.
 *
 *   +14 "ROBO"   +18 version (1)   +19 kind   +20 id (16 bit, little endian)
 *   +22 sender name[16]   +38 receiver name[16] (empty = everybody)
 *   +54 text length (16 bit)   +56 text
 *
 *   WHO   broadcast "who is there?"     -> every board answers HERE
 *   HERE  "I am <name>"
 *   MSG   text for one board (answered with ACK, retried 3 times) or for all
 *   ACK   message <id> arrived
 *
 * Frames are polled by the I/O core (core 1, smp.c), or from the console idle
 * loop when there is none (net_poll). All of it runs under the io lock. Messages wait in a
 * small inbox; the shell prints them at its prompt, programs fetch them.
 */
#include "usb.h"
#include "sd.h"

#define K_WHO  1
#define K_HERE 2
#define K_MSG  3
#define K_ACK  4
#define HDR    56

#define MAX_PEERS 8
#define INBOX     8

struct peer { char name[NET_NAME_MAX]; u8 mac[6]; u8 used, has_id; u16 last_id; u64 seen; };
struct msg  { char from[NET_NAME_MAX]; char text[NET_TEXT_MAX + 1]; };

static struct peer peers[MAX_PEERS];
static struct msg  inbox[INBOX];
static int  in_head, in_count;
static char myname[NET_NAME_MAX];
static u8   mymac[6];
static int  inited, name_from_file, notify, loop;
static u16  next_id, pending_id;
static volatile int acked;           /* set on the I/O core */

/* loopback queue ("net loop on"): frames we send come back to us */
static u8  lq[4][HDR + NET_TEXT_MAX + 4];
static int lq_len[4], lqh, lqt;

static const u8 bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int name_eq(const char *a, const char *b)
{
    for (int i = 0; i < NET_NAME_MAX; i++) {
        if (lc(a[i]) != lc(b[i])) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

static int name_ok(const char *s)
{
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return n >= 1 && n < NET_NAME_MAX && !name_eq(s, "all");
}

static void init(void)
{
    if (!inited) {
        static const char hex[] = "0123456789abcdef";
        u32 m[2] = { 0, 0 };
        mbox_prop(0x00010003, m, 2);
        memcpy(mymac, m, 6);
        int n = 0;                                       /* default: host-xxxx (end of the MAC) */
        for (const char *s = board_host(); *s && n < 8; s++)
            if (*s != '+') myname[n++] = *s;
        myname[n++] = '-';
        for (int i = 4; i < 6; i++) { myname[n++] = hex[mymac[i] >> 4]; myname[n++] = hex[mymac[i] & 15]; }
        myname[n] = 0;
        inited = 1;
    }
    if (!name_from_file && fat_mounted() && smp_core() == 0) {   /* a saved name wins (FAT: core 0 only) */
        struct fat_file f;
        char b[NET_NAME_MAX + 2];
        name_from_file = 1;
        if (!fat_open("/NAME.TXT", &f) && !f.is_dir) {
            int n = fat_read(&f, b, NET_NAME_MAX);
            if (n < 0) n = 0;
            b[n] = 0;
            for (int i = 0; i < n; i++) if (b[i] == '\r' || b[i] == '\n' || b[i] == ' ') { b[i] = 0; break; }
            if (name_ok(b)) memcpy(myname, b, strlen(b) + 1);
        }
    }
}

const char *net_name(void) { init(); return myname; }

int net_set_name(const char *name)
{
    init();
    if (!name_ok(name)) return -1;
    io_lock();                                           /* the I/O core sends the name */
    memset(myname, 0, sizeof(myname));
    memcpy(myname, name, strlen(name));
    name_from_file = 1;
    io_unlock();
    if (fat_mounted()) return fat_write_file("/NAME.TXT", myname, (u32)strlen(myname)) ? 1 : 0;
    return 1;                                            /* set, but not saved */
}

void net_notify(int on) { notify = on; }
void net_loop(int on)   { io_lock(); loop = on; lqh = lqt = 0; io_unlock(); }
int  net_looping(void)  { return loop; }
int  net_avail(void)    { io_lock(); int n = in_count; io_unlock(); return n; }

static int usable(void) { return loop || link_present(); }

static void tx(const u8 *f, int len)
{
    if (loop) {
        int n = (lqh + 1) % 4;
        if (n == lqt || len > (int)sizeof(lq[0])) return;
        memcpy(lq[lqh], f, len);
        lq_len[lqh] = len;
        lqh = n;
    } else {
        link_send(f, len);
    }
}

static int build(u8 *f, const u8 *dst, u8 kind, u16 id, const char *to, const char *text)
{
    int len = text ? (int)strlen(text) : 0;
    if (len > NET_TEXT_MAX) len = NET_TEXT_MAX;
    memset(f, 0, HDR + 4);
    memcpy(f, dst, 6);
    memcpy(f + 6, link_mac(), 6);
    f[12] = 0x88; f[13] = 0xB5;
    memcpy(f + 14, "ROBO", 4);
    f[18] = 1; f[19] = kind;
    f[20] = (u8)id; f[21] = (u8)(id >> 8);
    for (int i = 0; i < NET_NAME_MAX - 1 && myname[i]; i++) f[22 + i] = (u8)myname[i];
    for (int i = 0; to && i < NET_NAME_MAX - 1 && to[i]; i++) f[38 + i] = (u8)to[i];
    f[54] = (u8)len; f[55] = (u8)(len >> 8);
    if (len) memcpy(f + HDR, text, len);
    return HDR + len < 60 ? 60 : HDR + len;
}

static void send_simple(const u8 *dst, u8 kind, u16 id, const char *to)
{
    u8 f[64];
    tx(f, build(f, dst, kind, id, to, 0));
}

static struct peer *find_peer(const char *name)
{
    for (int i = 0; i < MAX_PEERS; i++)
        if (peers[i].used && name_eq(peers[i].name, name)) return &peers[i];
    return 0;
}

static struct peer *note_peer(const char *name, const u8 *mac)
{
    struct peer *p = find_peer(name), *oldest = &peers[0];
    if (!p) {
        for (int i = 0; i < MAX_PEERS && !p; i++) {
            if (!peers[i].used) p = &peers[i];
            else if (peers[i].seen < oldest->seen) oldest = &peers[i];
        }
        if (!p) p = oldest;
        memset(p, 0, sizeof(*p));
        memcpy(p->name, name, NET_NAME_MAX);
        p->used = 1;
    }
    memcpy(p->mac, mac, 6);
    p->seen = timer_us();
    return p;
}

static void input(const u8 *f, int n)
{
    char from[NET_NAME_MAX], to[NET_NAME_MAX];
    if (n >= 18 && f[12] == 0x81 && f[13] == 0x00) {     /* VLAN tag: step over it */
        static u8 t[HDR + NET_TEXT_MAX + 16];
        if (n - 4 > (int)sizeof(t)) return;
        memcpy(t, f, 12);
        memcpy(t + 12, f + 16, n - 16);
        f = t; n -= 4;
    }
    if (n < HDR || f[12] != 0x88 || f[13] != 0xB5 || memcmp_n(f + 14, "ROBO", 4) || f[18] != 1) return;
    if (!loop && !memcmp_n(f + 6, link_mac(), 6)) return;     /* our own broadcast */

    memcpy(from, f + 22, NET_NAME_MAX); from[NET_NAME_MAX - 1] = 0;
    memcpy(to, f + 38, NET_NAME_MAX);   to[NET_NAME_MAX - 1] = 0;
    if (!name_ok(from)) return;
    u16 id = f[20] | (f[21] << 8);
    struct peer *p = note_peer(from, f + 6);

    switch (f[19]) {
    case K_WHO:
        send_simple(f + 6, K_HERE, 0, from);
        break;
    case K_ACK:
        if (id == pending_id && name_eq(to, myname)) acked = 1;
        break;
    case K_MSG: {
        int len = f[54] | (f[55] << 8);
        if (len > NET_TEXT_MAX || HDR + len > n) return;
        if (to[0]) {
            if (!name_eq(to, myname)) return;
            send_simple(f + 6, K_ACK, id, from);
            if (p->has_id && p->last_id == id) return;   /* a repeat: our ACK got lost */
        }
        p->has_id = 1; p->last_id = id;
        if (len >= 5 && !memcmp_n(f + HDR, "ALM ", 4)) {   /* 0.134: alarm terminal traffic - not for the inbox */
            static char t[NET_TEXT_MAX + 1];
            for (int i = 0; i < len; i++) { char c = (char)f[HDR + i]; t[i] = (c >= 32 && c < 127) ? c : '.'; }
            t[len] = 0;
            alarm_net_in(from, t, len);
            break;
        }
        if (in_count == INBOX) { in_head = (in_head + 1) % INBOX; in_count--; }   /* drop the oldest */
        struct msg *m = &inbox[(in_head + in_count++) % INBOX];
        memcpy(m->from, from, NET_NAME_MAX);
        for (int i = 0; i < len; i++) {
            char c = (char)f[HDR + i];
            m->text[i] = (c >= 32 && c < 127) ? c : '.';
        }
        m->text[len] = 0;
        break;
    }
    }
}

static void pump(void)
{
    static u8 f[1600];
    if (loop) {
        for (int i = 0; i < 8 && lqt != lqh; i++) {
            int n = lq_len[lqt];
            memcpy(f, lq[lqt], n);
            lqt = (lqt + 1) % 4;
            input(f, n);
        }
        return;
    }
    if (!link_present()) return;
    for (int i = 0; i < 8; i++) {
        int n = link_recv(f, sizeof(f));
        if (n <= 0) break;
        int robo = n >= 14 && ((f[12] == 0x88 && f[13] == 0xB5) ||
                   (n >= 18 && f[12] == 0x81 && f[13] == 0x00 && f[16] == 0x88 && f[17] == 0xB5));
        if (robo) input(f, n);
        else ipstack_input(f, n);                             /* everything else: TCP/IP */
    }
}

static int recv_locked(char *from, char *text, int max);

int net_recv(char *from, char *text, int max)
{
    io_lock();
    int r = recv_locked(from, text, max);
    io_unlock();
    return r;
}

static int recv_locked(char *from, char *text, int max)
{
    if (!in_count) return -1;
    struct msg *m = &inbox[in_head];
    int n = (int)strlen(m->text);
    if (max > 0) {
        if (n > max - 1) n = max - 1;
        memcpy(text, m->text, n);
        text[n] = 0;
    }
    if (from) memcpy(from, m->from, NET_NAME_MAX);
    in_head = (in_head + 1) % INBOX;
    in_count--;
    return n;
}

/* called from the console idle loop */
void net_poll(void)
{
    static u64 next;
    u64 now = timer_us();
    if (now < next) return;
    next = now + (io_core_active() ? 1000 : 5000);     /* the I/O core can look more often */
    if (!usable()) return;
    init();
    pump();
    ip_poll(notify);
    if (notify) {
        char from[NET_NAME_MAX], text[NET_TEXT_MAX + 1];
        while (recv_locked(from, text, sizeof(text)) >= 0) {
            con_color(COL_CYAN); kprintf("\n[NET] %s: ", from);
            con_color(COL_DEFAULT); kprintf("%s\n", text);
        }
    }
}

/* wait for an answer: the I/O core receives it (let go of the lock meanwhile),
 * or without one, receive here */
static void wait_ms(u32 ms, const volatile int *flag)
{
    u64 end = timer_us() + (u64)ms * 1000;
    if (io_core_active() && smp_core() == 0) {
        int d = io_release();
        while (timer_us() < end && !(flag && *flag)) delay_us(100);
        io_reacquire(d);
    } else {
        while (timer_us() < end && !(flag && *flag)) pump();
    }
}

const char *net_error(int e)
{
    switch (e) {
    case 0:  return "delivered";
    case 1:  return "sent to everybody";
    case -1: return "no network (Ethernet chip missing or not initialized)";
    case -2: return "unknown name (try 'net who')";
    case -3: return "no answer";
    default: return "error";
    }
}

static int send_locked(const char *to, const char *text);

int net_send(const char *to, const char *text)
{
    io_lock();
    int r = send_locked(to, text);
    io_unlock();
    return r;
}

static int send_locked(const char *to, const char *text)
{
    static u8 f[HDR + NET_TEXT_MAX + 4];
    if (!usable()) return -1;
    init();
    u16 id = ++next_id;
    if (name_eq(to, "all") || !strcmp(to, "*")) {
        tx(f, build(f, bcast, K_MSG, id, 0, text));
        if (loop) pump();
        return 1;
    }
    struct peer *p = find_peer(to);
    if (!p) {                                            /* not seen yet: ask around */
        send_simple(bcast, K_WHO, 0, 0);
        wait_ms(500, 0);
        if (!(p = find_peer(to))) return -2;
    }
    int len = build(f, p->mac, K_MSG, id, p->name, text);
    for (int attempt = 0; attempt < 3; attempt++) {
        acked = 0;
        pending_id = id;
        tx(f, len);
        wait_ms(300, &acked);
        if (acked) return 0;
    }
    return -3;
}

static void who_locked(void);

void net_who(void)
{
    io_lock();
    who_locked();
    io_unlock();
}

static void who_locked(void)
{
    if (!usable()) { kprintf("%s\n", net_error(-1)); return; }
    init();
    send_simple(bcast, K_WHO, 0, 0);
    wait_ms(1000, 0);
    u64 now = timer_us();
    int n = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        struct peer *p = &peers[i];
        if (!p->used) continue;
        const u8 *m = p->mac;
        kprintf("  %-16s %02x:%02x:%02x:%02x:%02x:%02x  seen %u s ago\n", p->name,
                m[0], m[1], m[2], m[3], m[4], m[5], (u32)((now - p->seen) / 1000000));
        n++;
    }
    if (!n) kprintf("  nobody answered\n");
}
