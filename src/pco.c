/*
 * pco.c - 0.146: alarm reports to a monitoring centre (PCO / ARC) with SIA DC-09 over IP.
 *
 * Wire format (ANSI/SIA DC-09):  LF <crc 4 hex><len 4 hex>"<id>"<seq 4 dec>[R<rcvr>]L<line>#<acct>[<data>]<time> CR
 *   crc  = CRC-16/ARC (poly 0xA001 reflected, init 0) over everything from the first quote up to the CR
 *   len  = the number of those characters in hex ("0" + 3 hex digits)
 *   id   = ADM-CID (Contact ID: "#ACCT|1130 01 015") or SIA-DCS ("#ACCT|Nri1/BA015"), NULL = supervision (seq 0000);
 *          "*" before the id = AES-CBC encrypted (IV 0): the part after '[' is hex of
 *          AES(pad + "|" + data + "]" + time) (NULL: pad + "]" + time), pad = 1..16 random characters (not | [ ]) to a 16-byte multiple
 *   time = _HH:MM:SS,MM-DD-YYYY in UTC (needed when encrypted)
 * The receiver answers "ACK" (or "*ACK") with the same sequence number, "NAK" with its own time (our time is
 * off: we take its time and send again), or "DUH" (it does not understand the message: dropped).
 *
 * Events come from alarm.c on any core (pco_event, own spinlock); the transmitter runs on core 1 from ip_poll
 * with lwIP raw TCP / UDP (like mqtt.c); the shell (core 0) takes the io lock. One message at a time, the same
 * sequence number on every retry; primary receiver 3 tries, then the backup, then 60 s pauses - nothing is
 * dropped until the queue (32) is full. Settings in /PCO.TXT (read at boot on core 0).
 */
#include "kernel.h"
#include "sd.h"
#include "bearssl.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

static int enabled, loaded, use_udp, fmt_sia, heartbeat, test_h = 24;
static char host[2][64];
static int port[2] = { 7777, 7777 };
static char acct[17] = "1234", rcvr[7], line[7] = "0";
static u8 key[32];
static int keylen;                                        /* 0 = no encryption, else 16 / 24 / 32 */
static char keytxt[66];                                   /* as written in /PCO.TXT */

/* ---------------- the event queue (any core) ---------------- */

struct ev { u8 q; u16 code, zone; };
#define NQ 32
static struct ev q[NQ];
static volatile int qh, qt, qdrop;
static int qk;
static void qlock(void)   { while (__atomic_test_and_set(&qk, __ATOMIC_ACQUIRE)) ; }
static void qunlock(void) { __atomic_clear(&qk, __ATOMIC_RELEASE); }

/* q: 1 new event, 3 restore / closing; code: Contact ID event code; zone: zone or user number */
void pco_event(int qual, int code, int zone)
{
    if (!enabled) return;
    qlock();
    int n = (qh + 1) % NQ;
    if (n == qt) { qt = (qt + 1) % NQ; qdrop++; }         /* full: the oldest goes */
    q[qh].q = (u8)qual; q[qh].code = (u16)code; q[qh].zone = (u16)zone;
    qh = n;
    qunlock();
}

/* Contact ID -> SIA code */
static const char *sia_code(int qual, int code)
{
    static const struct { u16 c; char e[3], r[3]; } t[] = {
        { 130, "BA", "BR" }, { 133, "BA", "BR" }, { 134, "BA", "BR" }, { 120, "PA", "PR" },
        { 401, "OP", "CL" }, { 441, "OP", "NL" }, { 406, "BC", "BC" }, { 461, "JA", "JA" },
        { 602, "RP", "RP" }, { 305, "RR", "RR" }, { 301, "AT", "AR" },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (t[i].c == code) return qual == 3 ? t[i].r : t[i].e;
    return "UX";
}

/* ---------------- building a message ---------------- */

static u16 crc16(const char *s, int n)
{
    u16 c = 0;
    for (int i = 0; i < n; i++) {
        u8 t = (u8)s[i];
        for (int b = 0; b < 8; b++) { t ^= (u8)(c & 1); c >>= 1; if (t & 1) c ^= 0xA001; t >>= 1; }
    }
    return c;
}

static const char HX[] = "0123456789ABCDEF";
static int skew;                                          /* seconds to add to our clock (from a NAK) */
static int put(char *b, int n, int max, const char *s) { while (*s && n < max - 1) b[n++] = *s++; b[n] = 0; return n; }
static int putn(char *b, int n, int max, unsigned v, int digits)
{
    char d[12]; for (int i = digits - 1; i >= 0; i--) { d[i] = (char)('0' + v % 10); v /= 10; } d[digits] = 0;
    return put(b, n, max, d);
}

/* _HH:MM:SS,MM-DD-YYYY (UTC); 0 = no clock */
static int stamp(char *b, int max)
{
    u64 now = clock_now();
    if (!now) { b[0] = 0; return 0; }
    long long t = (long long)now + skew;
    long long days = t / 86400; int s = (int)(t % 86400);
    /* civil date from days (H. Hinnant) */
    long long z = days + 719468, era = z / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    int n = put(b, 0, max, "_");
    n = putn(b, n, max, (unsigned)(s / 3600), 2); n = put(b, n, max, ":");
    n = putn(b, n, max, (unsigned)(s / 60 % 60), 2); n = put(b, n, max, ":");
    n = putn(b, n, max, (unsigned)(s % 60), 2); n = put(b, n, max, ",");
    n = putn(b, n, max, m, 2); n = put(b, n, max, "-");
    n = putn(b, n, max, d, 2); n = put(b, n, max, "-");
    n = putn(b, n, max, (unsigned)y, 4);
    return n;
}
static int parse_stamp(const char *p, u64 *out)          /* "HH:MM:SS,MM-DD-YYYY" -> Unix seconds */
{
    int v[6], k = 0; const char *s = p;
    for (; k < 6; k++) {
        int x = 0, nd = 0;
        while (*s >= '0' && *s <= '9') { x = x * 10 + (*s++ - '0'); nd++; }
        if (!nd) return -1;
        v[k] = x;
        if (k < 5) { if (*s != ':' && *s != ',' && *s != '-') return -1; s++; }
    }
    int hh = v[0], mi = v[1], ss = v[2], mo = v[3], dd = v[4], yy = v[5];
    if (mo < 1 || mo > 12 || dd < 1 || dd > 31 || yy < 2000) return -1;
    int y = mo <= 2 ? yy - 1 : yy;
    long long era = y / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (mo > 2 ? mo - 3 : mo + 9) + 2) / 5 + dd - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    *out = (u64)(days * 86400 + hh * 3600 + mi * 60 + ss);
    return 0;
}

static char msg[400];                                     /* the framed message being sent */
static int msg_len, cur_seq = 0, cur_kind;                /* kind 1 event, 2 NULL */
static int seq_ctr, built;                                /* events 0001-9999; NULL always 0000 */
static struct ev cur;
static char cur_text[24];                                 /* "E130 z015" for the log */

static void aes_cbc(u8 *d, int n, int dec)
{
    u8 iv[16] = { 0 };
    if (dec) { br_aes_ct_cbcdec_keys k; br_aes_ct_cbcdec_init(&k, key, (size_t)keylen); br_aes_ct_cbcdec_run(&k, iv, d, (size_t)n); }
    else     { br_aes_ct_cbcenc_keys k; br_aes_ct_cbcenc_init(&k, key, (size_t)keylen); br_aes_ct_cbcenc_run(&k, iv, d, (size_t)n); }
}

static void build(void)
{
    char body[360], data[48], ts[24];
    int n = 0, dn = 0;
    stamp(ts, sizeof(ts));
    data[0] = 0;
    if (cur_kind == 1) {
        dn = put(data, 0, sizeof(data), "#"); dn = put(data, dn, sizeof(data), acct); dn = put(data, dn, sizeof(data), "|");
        if (fmt_sia) {
            dn = put(data, dn, sizeof(data), "Nri1/"); dn = put(data, dn, sizeof(data), sia_code(cur.q, cur.code));
            dn = putn(data, dn, sizeof(data), cur.zone, 3);
        } else {
            dn = putn(data, dn, sizeof(data), cur.q, 1); dn = putn(data, dn, sizeof(data), cur.code, 3);
            dn = put(data, dn, sizeof(data), " 01 "); dn = putn(data, dn, sizeof(data), cur.zone, 3);
        }
    }
    n = put(body, 0, sizeof(body), keylen ? "\"*" : "\"");
    n = put(body, n, sizeof(body), cur_kind == 2 ? "NULL" : fmt_sia ? "SIA-DCS" : "ADM-CID");
    n = put(body, n, sizeof(body), "\"");
    n = putn(body, n, sizeof(body), (unsigned)cur_seq, 4);
    if (rcvr[0]) { n = put(body, n, sizeof(body), "R"); n = put(body, n, sizeof(body), rcvr); }
    n = put(body, n, sizeof(body), "L"); n = put(body, n, sizeof(body), line);
    n = put(body, n, sizeof(body), "#"); n = put(body, n, sizeof(body), acct);
    n = put(body, n, sizeof(body), "[");
    if (!keylen) {
        n = put(body, n, sizeof(body), data); n = put(body, n, sizeof(body), "]"); n = put(body, n, sizeof(body), ts);
    } else {
        u8 pt[128]; int pl = 0;
        int bar = cur_kind == 1;                               /* NULL: pad "]" time (as real panels send it) */
        int core = bar + dn + 1 + (int)strlen(ts);            /* "|" data "]" time */
        int pad = 16 - core % 16;                              /* 1..16 */
        u8 rnd[16]; rng_bytes(rnd, 16);
        static const char pc[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        for (int i = 0; i < pad; i++) pt[pl++] = (u8)pc[rnd[i] & 31];
        if (bar) pt[pl++] = '|';
        for (int i = 0; i < dn; i++) pt[pl++] = (u8)data[i];
        pt[pl++] = ']';
        for (int i = 0; ts[i]; i++) pt[pl++] = (u8)ts[i];
        aes_cbc(pt, pl, 0);
        for (int i = 0; i < pl && n < (int)sizeof(body) - 3; i++) { body[n++] = HX[pt[i] >> 4]; body[n++] = HX[pt[i] & 15]; }
        body[n] = 0;
    }
    u16 c = crc16(body, n);
    int m = 0;
    msg[m++] = '\n';
    msg[m++] = HX[c >> 12]; msg[m++] = HX[(c >> 8) & 15]; msg[m++] = HX[(c >> 4) & 15]; msg[m++] = HX[c & 15];
    msg[m++] = '0'; msg[m++] = HX[(n >> 8) & 15]; msg[m++] = HX[(n >> 4) & 15]; msg[m++] = HX[n & 15];
    for (int i = 0; i < n; i++) msg[m++] = body[i];
    msg[m++] = '\r';
    msg_len = m;
}

/* ---------------- the log (status) ---------------- */

#define NLOG 8
static struct { u32 up_s; char what[24]; char res[28]; } lg[NLOG];
static int lg_h, lg_n;
static u32 n_ok, n_fail;
static u64 last_ok_at;
static const char *why = "";
static void logit(const char *what, const char *res)
{
    lg[lg_h].up_s = (u32)(timer_us() / 1000000);
    int i = 0; for (; what[i] && i < 23; i++) lg[lg_h].what[i] = what[i]; lg[lg_h].what[i] = 0;
    i = 0; for (; res[i] && i < 27; i++) lg[lg_h].res[i] = res[i]; lg[lg_h].res[i] = 0;
    lg_h = (lg_h + 1) % NLOG; if (lg_n < NLOG) lg_n++;
}

/* ---------------- transport (core 1) ---------------- */

enum { S_IDLE, S_DNS, S_CONN, S_WAIT, S_PAUSE };
static const char *const sname[] = { "idle", "looking up the name", "connecting", "waiting for the answer", "waiting to retry" };
static int st = S_IDLE, rcv_i, tries, naks;
static struct tcp_pcb *pcb;
static struct udp_pcb *upcb;
static ip_addr_t addr[2];
static int addr_ok[2], pcb_for = -1;
static u64 deadline, retry_at, idle_close_at, next_null, next_test, sent_at;
static char rx[256];
static int rxn;
static volatile int answer;                               /* 0 none, 1 ACK, 2 NAK, 3 DUH, 4 other */
static u64 nak_time;

static void tcp_drop(void)
{
    if (pcb) {
        tcp_arg(pcb, 0); tcp_recv(pcb, 0); tcp_err(pcb, 0);
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
        pcb = 0;
    }
    pcb_for = -1; rxn = 0;
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

/* one answer line (without LF / CR) */
static void got_line(const char *l, int n)
{
    int a = -1;
    for (int i = 0; i < n; i++) if (l[i] == '"') { a = i; break; }
    if (a < 0) return;
    const char *id = l + a + 1;
    int enc = *id == '*'; if (enc) id++;
    int typ = !strncmp(id, "ACK\"", 4) ? 1 : !strncmp(id, "NAK\"", 4) ? 2 : !strncmp(id, "DUH\"", 4) ? 3 : 0;
    if (!typ) return;
    const char *s = id + 4;
    int seq = 0; for (int i = 0; i < 4; i++) { if (s[i] < '0' || s[i] > '9') return; seq = seq * 10 + s[i] - '0'; }
    if (typ == 1 && seq != cur_seq) return;                /* an old ACK */
    if (typ == 2) {                                        /* NAK: take the receiver's time */
        const char *b = 0; for (const char *p = s; p < l + n; p++) if (*p == '[') { b = p + 1; break; }
        char plain[96]; int pn = 0;
        if (b && enc && keylen) {
            u8 d[96]; int dn = 0;
            for (const char *p = b; p + 1 < l + n && dn < 96 && hexv(p[0]) >= 0 && hexv(p[1]) >= 0; p += 2) d[dn++] = (u8)(hexv(p[0]) << 4 | hexv(p[1]));
            dn &= ~15;
            aes_cbc(d, dn, 1);
            for (int i = 0; i < dn && pn < 95; i++) plain[pn++] = (char)d[i];
        } else if (b) for (const char *p = b; p < l + n && pn < 95; p++) plain[pn++] = *p;
        plain[pn] = 0;
        nak_time = 0;
        for (int i = 0; i < pn; i++) if (plain[i] == '_') { u64 t; if (!parse_stamp(plain + i + 1, &t)) nak_time = t; break; }
    }
    answer = typ;
}

static void got_bytes(const char *p, int n)
{
    for (int i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\n') { rxn = 0; continue; }
        if (c == '\r') { got_line(rx, rxn); rxn = 0; continue; }
        if (rxn < (int)sizeof(rx) - 1) rx[rxn++] = c;
    }
}

static err_t t_recv(void *arg, struct tcp_pcb *tp, struct pbuf *pb, err_t err)
{
    (void)arg; (void)err;
    if (!pb) { tcp_drop(); if (st == S_WAIT && !answer) answer = 5; return ERR_OK; }
    char b[256]; int n = pb->tot_len > 255 ? 255 : pb->tot_len;
    pbuf_copy_partial(pb, b, (u16)n, 0);
    tcp_recved(tp, pb->tot_len);
    pbuf_free(pb);
    got_bytes(b, n);
    return ERR_OK;
}
static void t_err(void *arg, err_t err) { (void)arg; (void)err; pcb = 0; pcb_for = -1; if (st == S_CONN || st == S_WAIT) answer = 5; }
static void u_recv(void *arg, struct udp_pcb *u, struct pbuf *pb, const ip_addr_t *a, u16_t pt)
{
    (void)arg; (void)u; (void)a; (void)pt;
    char b[256]; int n = pb->tot_len > 255 ? 255 : pb->tot_len;
    pbuf_copy_partial(pb, b, (u16)n, 0);
    pbuf_free(pb);
    rxn = 0;
    got_bytes(b, n);
}

static void send_now(void)
{
    answer = 0; rxn = 0;
    sent_at = timer_us();
    if (use_udp) {
        if (!upcb) { upcb = udp_new(); if (upcb) udp_recv(upcb, u_recv, 0); }
        struct pbuf *pb = upcb ? pbuf_alloc(PBUF_TRANSPORT, (u16)msg_len, PBUF_RAM) : 0;
        if (!pb) { answer = 5; why = "no memory"; return; }
        memcpy(pb->payload, msg, (size_t)msg_len);
        udp_sendto(upcb, pb, &addr[rcv_i], (u16)port[rcv_i]);
        pbuf_free(pb);
        deadline = sent_at + 5000000ULL;
    } else {
        if (tcp_write(pcb, msg, (u16)msg_len, TCP_WRITE_FLAG_COPY) != ERR_OK) { answer = 5; why = "cannot send"; return; }
        tcp_output(pcb);
        deadline = sent_at + 15000000ULL;
    }
    st = S_WAIT;
}

static err_t t_connected(void *arg, struct tcp_pcb *tp, err_t err)
{
    (void)arg; (void)tp;
    if (err != ERR_OK) { answer = 5; return ERR_OK; }
    pcb_for = rcv_i;
    if (st == S_CONN) send_now();
    return ERR_OK;
}

static void connect_or_send(void)
{
    if (use_udp) { send_now(); return; }
    if (pcb && pcb_for == rcv_i) { send_now(); return; }
    tcp_drop();
    pcb = tcp_new();
    if (!pcb) { answer = 5; why = "no memory"; return; }
    tcp_arg(pcb, 0); tcp_recv(pcb, t_recv); tcp_err(pcb, t_err);
    st = S_CONN;
    deadline = timer_us() + 10000000ULL;
    if (tcp_connect(pcb, &addr[rcv_i], (u16)port[rcv_i], t_connected) != ERR_OK) { tcp_abort(pcb); pcb = 0; answer = 5; why = "cannot connect"; }
}

static void p_dns(const char *name, const ip_addr_t *a, void *arg)
{
    (void)name;
    int i = (int)(long)arg;
    if (st != S_DNS || i != rcv_i) return;
    if (!a) { answer = 5; why = "receiver name not found"; return; }
    ip_addr_copy(addr[i], *a); addr_ok[i] = 1;
    connect_or_send();
}

static void start_try(void)
{
    answer = 0;
    if (!host[rcv_i][0]) rcv_i = 0;
    if (!built) { build(); built = 1; }                   /* a retry sends the SAME bytes (time too): receivers drop duplicates by them */
    if (addr_ok[rcv_i]) { connect_or_send(); return; }
    st = S_DNS;
    deadline = timer_us() + 10000000ULL;
    err_t e = dns_gethostbyname(host[rcv_i], &addr[rcv_i], p_dns, (void *)(long)rcv_i);
    if (e == ERR_OK) { addr_ok[rcv_i] = 1; connect_or_send(); }
    else if (e != ERR_INPROGRESS) { answer = 5; why = "receiver name not found"; }
}

static void finished(int ok, const char *res)
{
    logit(cur_kind == 2 ? "NULL (supervision)" : cur_text, res);
    if (ok) { n_ok++; last_ok_at = timer_us(); why = ""; }
    if (cur_kind == 1) { qlock(); if (qt != qh) qt = (qt + 1) % NQ; qunlock(); }
    if (ok && cur.code == 602 && cur_kind == 1) next_test = timer_us() + (u64)test_h * 3600000000ULL;
    cur_kind = 0; tries = 0; naks = 0;
    st = S_IDLE;
    idle_close_at = timer_us() + 20000000ULL;
    if (heartbeat) next_null = timer_us() + (u64)heartbeat * 1000000ULL;
}

static void failed(const char *what)
{
    char r[28] = "no answer from receiver 1";
    r[24] = (char)('1' + rcv_i);
    why = what;
    tries++; n_fail++;
    tcp_drop();
    addr_ok[rcv_i] = 0;                                   /* look the name up again */
    u64 pause = 3000000ULL;                               /* 3 tries per receiver, 3 s apart */
    if (tries % 3 == 0) {
        logit(cur_kind == 2 ? "NULL (supervision)" : cur_text, r);
        if (host[1][0]) rcv_i ^= 1;
        pause = host[1][0] && tries % 6 ? 500000ULL : 30000000ULL;   /* the backup at once; both failed: 30 s */
    }
    st = S_PAUSE;
    retry_at = timer_us() + pause;
    if (cur_kind == 2 && tries >= 6) { cur_kind = 0; tries = 0; st = S_IDLE; next_null = timer_us() + (u64)heartbeat * 1000000ULL; }
}

/* ip_poll (core 1) */
void pco_poll(int have_ip)
{
    if (!enabled) { if (pcb) tcp_drop(); return; }
    u64 now = timer_us();
    if (!have_ip) { if (st == S_CONN || st == S_WAIT || st == S_DNS) { tcp_drop(); st = S_PAUSE; retry_at = now + 3000000ULL; why = "no network"; } return; }
    switch (st) {
    case S_IDLE: {
        if (pcb && now > idle_close_at) tcp_drop();
        if (test_h && next_test && now >= next_test) { pco_event(1, 602, 0); next_test = now + (u64)test_h * 3600000000ULL; }
        qlock();
        int have = qt != qh;
        if (have) cur = q[qt];
        qunlock();
        if (have) {
            cur_kind = 1;
            int n = put(cur_text, 0, sizeof(cur_text), cur.q == 3 ? "R" : "E");
            n = putn(cur_text, n, sizeof(cur_text), cur.code, 3); n = put(cur_text, n, sizeof(cur_text), cur.code == 401 || cur.code == 441 || cur.code == 406 ? " user " : " zone ");
            putn(cur_text, n, sizeof(cur_text), cur.zone, 3);
        } else if (heartbeat && now >= next_null) cur_kind = 2;
        else return;
        if (cur_kind == 1) { seq_ctr = seq_ctr % 9999 + 1; cur_seq = seq_ctr; } else cur_seq = 0;
        built = 0;
        tries = 0; naks = 0;
        start_try();
        return;
    }
    case S_PAUSE:
        if (now >= retry_at) start_try();
        return;
    case S_DNS: case S_CONN: case S_WAIT:
        if (answer == 1) {
            char r[28] = "ACK ";
            u32 ms = (u32)((now - sent_at) / 1000);
            int n = putn(r, 4, sizeof(r), ms, ms >= 1000 ? 4 : ms >= 100 ? 3 : ms >= 10 ? 2 : 1); put(r, n, sizeof(r), " ms");
            finished(1, r);
        } else if (answer == 2) {
            u64 mine = clock_now();
            if (nak_time && mine) skew = (int)((long long)nak_time - (long long)mine);
            if (++naks > 2) { finished(0, "NAK (time?) - dropped"); return; }
            why = "NAK: the receiver's time differs";
            built = 0;
            start_try();                                  /* the same sequence number, new time */
        } else if (answer == 3) finished(0, "DUH - not understood");
        else if (answer == 5) failed(why[0] ? why : "connection error");
        else if (now > deadline) failed(st == S_WAIT ? "no answer" : st == S_CONN ? "cannot connect" : "receiver name not found");
        return;
    }
}

/* ---------------- settings (core 0) ---------------- */

static void scopy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static int set_hex(char *d, const char *s, int maxlen, int minlen)
{
    int n = (int)strlen(s);
    if (n < minlen || n > maxlen) return -1;
    for (int i = 0; i < n; i++) if (hexv(s[i]) < 0) return -1;
    for (int i = 0; i <= n; i++) d[i] = s[i] >= 'a' && s[i] <= 'f' ? (char)(s[i] - 32) : s[i];
    return 0;
}
static int set_key(const char *s)                         /* hex 32/48/64 digits, or "text:<16/24/32 chars>" */
{
    if (!strcmp(s, "off")) { keylen = 0; keytxt[0] = 0; return 0; }
    if (!strncmp(s, "text:", 5)) {
        int n = (int)strlen(s + 5);
        if (n != 16 && n != 24 && n != 32) return -1;
        memcpy(key, s + 5, (size_t)n); keylen = n;
    } else {
        int n = (int)strlen(s);
        if (n != 32 && n != 48 && n != 64) return -1;
        for (int i = 0; i < n; i++) if (hexv(s[i]) < 0) return -1;
        for (int i = 0; i < n / 2; i++) key[i] = (u8)(hexv(s[2 * i]) << 4 | hexv(s[2 * i + 1]));
        keylen = n / 2;
    }
    scopy(keytxt, s, sizeof(keytxt));
    return 0;
}

static void apply(char *k, char *v)
{
    u64 num = 0;
    if (!strcmp(k, "state")) enabled = !strcmp(v, "on");
    else if (!strcmp(k, "host") || !strcmp(k, "backup")) {
        int i = k[0] == 'b';
        char *sp = strchr(v, ' ');
        if (sp) { *sp++ = 0; if (parse_num(sp, &num) && num && num < 65536) port[i] = (int)num; }
        scopy(host[i], strcmp(v, "off") ? v : "", sizeof(host[i]));
    }
    else if (!strcmp(k, "protocol")) use_udp = !strcmp(v, "udp");
    else if (!strcmp(k, "account")) set_hex(acct, v, 16, 3);
    else if (!strcmp(k, "receiver")) { if (!strcmp(v, "off")) rcvr[0] = 0; else set_hex(rcvr, v, 6, 1); }
    else if (!strcmp(k, "line")) set_hex(line, v, 6, 1);
    else if (!strcmp(k, "format")) fmt_sia = !strcmp(v, "sia");
    else if (!strcmp(k, "key")) set_key(v);
    else if (!strcmp(k, "heartbeat") && parse_num(v, &num)) heartbeat = (int)num;
    else if (!strcmp(k, "test") && parse_num(v, &num)) test_h = (int)num;
}

/* core 0, kernel_main (before alarm_boot) */
void pco_prepare(void)
{
    loaded = 1;
    struct fat_file f;
    char b[700];
    if (!fat_mounted() || fat_open("/PCO.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    for (char *l = b; l && *l; ) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        int k = (int)strlen(l);
        while (k && (l[k - 1] == '\r' || l[k - 1] == ' ')) l[--k] = 0;
        char *v = strchr(l, ' ');
        if (v) { *v++ = 0; while (*v == ' ') v++; apply(l, v); }
        l = e ? e + 1 : 0;
    }
    if (enabled && host[0][0]) {
        pco_event(1, 305, 0);                             /* system reset (power up) */
        if (test_h) next_test = timer_us() + (u64)test_h * 3600000000ULL;
        if (heartbeat) next_null = timer_us() + (u64)heartbeat * 1000000ULL;
    } else if (enabled) enabled = 0, why = "no receiver ('pco host <address> <port>')";
}

static void save(void)
{
    char b[600]; int n = 0;
    char num[8];
    n = put(b, n, sizeof(b), "state "); n = put(b, n, sizeof(b), enabled ? "on" : "off");
    for (int i = 0; i < 2; i++) if (host[i][0]) {
        n = put(b, n, sizeof(b), i ? "\r\nbackup " : "\r\nhost "); n = put(b, n, sizeof(b), host[i]);
        int k = 0, v = port[i]; char d[8]; do d[k++] = (char)('0' + v % 10); while (v /= 10); int j = 0; while (k) num[j++] = d[--k]; num[j] = 0;
        n = put(b, n, sizeof(b), " "); n = put(b, n, sizeof(b), num);
    }
    n = put(b, n, sizeof(b), "\r\nprotocol "); n = put(b, n, sizeof(b), use_udp ? "udp" : "tcp");
    n = put(b, n, sizeof(b), "\r\naccount "); n = put(b, n, sizeof(b), acct);
    if (rcvr[0]) { n = put(b, n, sizeof(b), "\r\nreceiver "); n = put(b, n, sizeof(b), rcvr); }
    n = put(b, n, sizeof(b), "\r\nline "); n = put(b, n, sizeof(b), line);
    n = put(b, n, sizeof(b), "\r\nformat "); n = put(b, n, sizeof(b), fmt_sia ? "sia" : "cid");
    if (keylen) { n = put(b, n, sizeof(b), "\r\nkey "); n = put(b, n, sizeof(b), keytxt); }
    n = put(b, n, sizeof(b), "\r\nheartbeat "); n = putn(b, n, sizeof(b), (unsigned)heartbeat, heartbeat >= 10000 ? 5 : heartbeat >= 1000 ? 4 : heartbeat >= 100 ? 3 : heartbeat >= 10 ? 2 : 1);
    n = put(b, n, sizeof(b), "\r\ntest "); n = putn(b, n, sizeof(b), (unsigned)test_h, test_h >= 100 ? 3 : test_h >= 10 ? 2 : 1);
    n = put(b, n, sizeof(b), "\r\n");
    if (!fat_mounted() || fat_write_file("/PCO.TXT", b, (u32)n)) kprintf("(not saved: no SD card or write error)\n");
    else kprintf("(saved in /PCO.TXT)\n");
}

/* one status line for 'alarm' */
int pco_info(char *b, int max)
{
    if (!enabled) { b[0] = 0; return 0; }
    int n = put(b, 0, max, "PCO (SIA DC-09): ");
    n = put(b, n, max, host[0]);
    n = put(b, n, max, st == S_IDLE && !why[0] ? (n_ok ? ", last report confirmed" : ", ready") : ", ");
    if (!(st == S_IDLE && !why[0])) n = put(b, n, max, why[0] ? why : sname[st]);
    return n;
}

void pco_cmd(int argc, char **argv)
{
    if (!loaded) pco_prepare();
    int changed = 1;
    char err[80] = "";
    io_lock();
    if (argc == 1) changed = 0;
    else if (!strcmp(argv[1], "on") || !strcmp(argv[1], "off")) {
        enabled = argv[1][1] == 'n';
        if (enabled && !host[0][0]) { enabled = 0; scopy(err, "first 'pco host <address> <port> [tcp|udp]'", sizeof(err)); changed = 0; }
        if (enabled) { if (test_h) next_test = timer_us() + (u64)test_h * 3600000000ULL; if (heartbeat) next_null = timer_us() + (u64)heartbeat * 1000000ULL; }
    } else if ((!strcmp(argv[1], "host") || !strcmp(argv[1], "backup")) && argc > 2) {
        int i = argv[1][0] == 'b';
        scopy(host[i], strcmp(argv[2], "off") ? argv[2] : "", sizeof(host[i]));
        for (int a = 3; a < argc; a++) {
            u64 p;
            if (!strcmp(argv[a], "udp")) use_udp = 1; else if (!strcmp(argv[a], "tcp")) use_udp = 0;
            else if (parse_num(argv[a], &p) && p && p < 65536) port[i] = (int)p;
        }
        addr_ok[i] = 0;
    } else if (!strcmp(argv[1], "account") && argc > 2) { if (set_hex(acct, argv[2], 16, 3)) scopy(err, "the account is 3-16 hex digits (0-9 A-F)", sizeof(err)), changed = 0; }
    else if (!strcmp(argv[1], "receiver") && argc > 2) { if (!strcmp(argv[2], "off")) rcvr[0] = 0; else if (set_hex(rcvr, argv[2], 6, 1)) scopy(err, "1-6 hex digits", sizeof(err)), changed = 0; }
    else if (!strcmp(argv[1], "line") && argc > 2) { if (set_hex(line, argv[2], 6, 1)) scopy(err, "1-6 hex digits", sizeof(err)), changed = 0; }
    else if (!strcmp(argv[1], "format") && argc > 2 && (!strcmp(argv[2], "cid") || !strcmp(argv[2], "sia"))) fmt_sia = argv[2][0] == 's';
    else if (!strcmp(argv[1], "key") && argc > 2) {
        char k[70] = "";
        if (!strcmp(argv[2], "text") && argc > 3) { scopy(k, "text:", sizeof(k)); scopy(k + 5, argv[3], (int)sizeof(k) - 5); }
        else scopy(k, argv[2], sizeof(k));
        if (set_key(k)) scopy(err, "key: 32 / 48 / 64 hex digits, 'text <16/24/32 characters>' or 'off'", sizeof(err)), changed = 0;
    }
    else if (!strcmp(argv[1], "heartbeat") && argc > 2) { u64 v; if (parse_num(argv[2], &v) && v <= 86400) { heartbeat = (int)v; next_null = timer_us() + v * 1000000ULL; } else changed = 0; }
    else if (!strcmp(argv[1], "test") && argc > 2) { u64 v; if (parse_num(argv[2], &v) && v <= 720) { test_h = (int)v; next_test = v ? timer_us() + v * 3600000000ULL : 0; } else changed = 0; }
    else if (!strcmp(argv[1], "test")) {
        changed = 0;
        if (!enabled) scopy(err, "PCO is off ('pco on')", sizeof(err));
        else { io_unlock(); pco_event(1, 602, 0); kprintf("test report (E602) queued\n"); io_lock(); }
    } else if (!strcmp(argv[1], "send") && argc > 3) {     /* pco send <E|R><code> <zone> - any Contact ID event, for tests with the PCO */
        changed = 0;
        u64 c, z;
        const char *s = argv[2]; int qual = 1;
        if (*s == 'R' || *s == 'r') { qual = 3; s++; } else if (*s == 'E' || *s == 'e') s++;
        if (!enabled) scopy(err, "PCO is off ('pco on')", sizeof(err));
        else if (parse_num(s, &c) && c < 1000 && parse_num(argv[3], &z) && z < 1000) { io_unlock(); pco_event(qual, (int)c, (int)z); kprintf("queued\n"); io_lock(); }
        else scopy(err, "pco send <E|R><code 3 digits> <zone/user>, e.g. pco send E130 3", sizeof(err));
    } else {
        changed = 0;
        io_unlock();
        kprintf("usage: pco [on|off | host <address> [port] [tcp|udp] | backup <address> [port]|off\n"
                "            | account <hex> | receiver <hex>|off | line <hex> | format cid|sia\n"
                "            | key <hex 32/48/64>|text <16/24/32 chars>|off | heartbeat <s> | test [hours] | send E130 3]\n");
        return;
    }
    if (changed) { tcp_drop(); addr_ok[0] = addr_ok[1] = 0; if (st != S_IDLE) { st = S_PAUSE; retry_at = 0; } rcv_i = 0; tries = 0; built = 0; }
    io_unlock();
    if (err[0]) { con_color(COL_YELLOW); kprintf("%s\n", err); con_color(COL_DEFAULT); }
    if (changed) save();

    kprintf("PCO (monitoring centre, SIA DC-09): %s\n", enabled ? "ON" : "off");
    kprintf("  receiver %s:%d", host[0][0] ? host[0] : "-", port[0]);
    if (host[1][0]) kprintf(", backup %s:%d", host[1], port[1]);
    kprintf(" over %s; account %s, line L%s%s%s\n", use_udp ? "UDP" : "TCP", acct, line, rcvr[0] ? ", receiver R" : "", rcvr);
    kprintf("  format %s, %s, supervision %s", fmt_sia ? "SIA-DCS" : "ADM-CID (Contact ID)",
            keylen ? (keylen == 16 ? "AES-128" : keylen == 24 ? "AES-192" : "AES-256") : "not encrypted",
            heartbeat ? "every " : "off");
    if (heartbeat) kprintf("%d s", heartbeat);
    kprintf(", test report ");
    if (test_h) kprintf("every %d h\n", test_h); else kprintf("off\n");
    if (!clock_now()) { con_color(COL_YELLOW); kprintf("  !! no clock yet - messages go without a time%s\n", keylen ? " (receivers want one when encrypted)" : ""); con_color(COL_DEFAULT); }
    if (skew) kprintf("  time taken from the receiver: %d s off\n", skew);
    int qn = (qh - qt + NQ) % NQ;
    kprintf("  now: %s, %d waiting, %u confirmed, %u failed tries%s", sname[st], qn, n_ok, n_fail, qdrop ? ", " : "");
    if (qdrop) kprintf("%d lost (queue full)", qdrop);
    kprintf("\n");
    if (why[0]) { con_color(COL_YELLOW); kprintf("  last problem: %s\n", why); con_color(COL_DEFAULT); }
    if (lg_n) kprintf("  last reports:\n");
    for (int i = 0; i < lg_n; i++) {
        int k = (lg_h - lg_n + i + NLOG) % NLOG;
        kprintf("    %6u s  %-20s %s\n", lg[k].up_s, lg[k].what, lg[k].res);
    }
    (void)last_ok_at;
}
