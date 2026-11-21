/*
 * HTTP / HTTPS client for 'update get' (core 0): GET a URL into a buffer, following redirects.
 * TCP through the socket API (ip.c), TLS with the BearSSL client and the root CAs in ta_roots.h
 * (tools/mkta.py) plus an own one from /TLS/CA.PEM (for a server in the home network).
 * insecure = no certificate check (the connection is still encrypted, but anybody in the path could
 * hand out another kernel - the RoboOS tag check is all that is left then).
 */
#include "kernel.h"
#include "sd.h"
#include "bearssl.h"
#include "ta_roots.h"

const char *fetch_err = "";

static int stop_key(void) { return con_break(); }   /* Ctrl+C; other keys stay typed */

/* ---------------- the extra CA from /TLS/CA.PEM ---------------- */

static br_x509_trust_anchor tas[24];
static int ntas;
static u8 ca_dn[512], ca_key[600];

struct pem_out { u8 *buf; int len, max; };
static void pem_dest(void *ctx, const void *src, size_t len)
{
    struct pem_out *o = ctx;
    if (o->len + (int)len <= o->max) memcpy(o->buf + o->len, src, len);
    o->len += (int)len;
}
static void dn_dest(void *ctx, const void *src, size_t len) { pem_dest(ctx, src, len); }

static void load_cas(void)
{
    ntas = 0;
    for (u32 i = 0; i < sizeof(ta_roots) / sizeof(ta_roots[0]) && ntas < 23; i++) tas[ntas++] = ta_roots[i];
    struct fat_file f;
    static u8 pem[4096], der[2048];
    if (!fat_mounted() || fat_open("/TLS/CA.PEM", &f) || f.is_dir || f.size >= sizeof(pem)) return;
    int n = fat_read(&f, pem, sizeof(pem));
    if (n <= 0) return;
    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);
    struct pem_out o = { der, 0, sizeof(der) };
    int in = 0, got = 0;
    for (int off = 0; off < n && !got; ) {
        size_t k = br_pem_decoder_push(&pc, pem + off, (size_t)(n - off));
        off += (int)k;
        switch (br_pem_decoder_event(&pc)) {
        case BR_PEM_BEGIN_OBJ: in = !strcmp(br_pem_decoder_name(&pc), "CERTIFICATE"); o.len = 0;
            br_pem_decoder_setdest(&pc, in ? pem_dest : 0, &o); break;
        case BR_PEM_END_OBJ: if (in && o.len <= (int)sizeof(der)) got = 1; break;
        }
    }
    if (!got) return;
    br_x509_decoder_context dc;
    struct pem_out dn = { ca_dn, 0, sizeof(ca_dn) };
    br_x509_decoder_init(&dc, dn_dest, &dn);
    br_x509_decoder_push(&dc, der, (size_t)o.len);
    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || dn.len > (int)sizeof(ca_dn)) return;
    br_x509_trust_anchor *t = &tas[ntas];
    t->dn.data = ca_dn; t->dn.len = (size_t)dn.len;
    t->flags = BR_X509_TA_CA;
    t->pkey = *pk;
    if (pk->key_type == BR_KEYTYPE_RSA) {           /* copy the key: the decoder's buffer goes away */
        if (pk->key.rsa.nlen + pk->key.rsa.elen > sizeof(ca_key)) return;
        memcpy(ca_key, pk->key.rsa.n, pk->key.rsa.nlen); memcpy(ca_key + pk->key.rsa.nlen, pk->key.rsa.e, pk->key.rsa.elen);
        t->pkey.key.rsa.n = ca_key; t->pkey.key.rsa.e = ca_key + pk->key.rsa.nlen;
    } else {
        if (pk->key.ec.qlen > sizeof(ca_key)) return;
        memcpy(ca_key, pk->key.ec.q, pk->key.ec.qlen);
        t->pkey.key.ec.q = ca_key;
    }
    ntas++;
    kprintf("  + own CA from /TLS/CA.PEM\n");
}

/* ---------------- "no check" X.509 engine: the minimal one, but an unknown root is fine ---------------- */

static br_x509_minimal_context xm;
static const br_x509_class *noch_vt;
static void nc_start_chain(const br_x509_class **c, const char *sn) { (void)c; xm.vtable->start_chain(&xm.vtable, sn); }
static void nc_start_cert(const br_x509_class **c, uint32_t l) { (void)c; xm.vtable->start_cert(&xm.vtable, l); }
static void nc_append(const br_x509_class **c, const unsigned char *b, size_t l) { (void)c; xm.vtable->append(&xm.vtable, b, l); }
static void nc_end_cert(const br_x509_class **c) { (void)c; xm.vtable->end_cert(&xm.vtable); }
static unsigned nc_end_chain(const br_x509_class **c)
{
    (void)c;
    unsigned r = xm.vtable->end_chain(&xm.vtable);
    return r == BR_ERR_X509_NOT_TRUSTED ? 0 : r;
}
static const br_x509_pkey *nc_get_pkey(const br_x509_class *const *c, unsigned *u) { (void)c; return xm.vtable->get_pkey(&xm.vtable, u); }
static const br_x509_class noch_class = {
    sizeof(br_x509_class *), nc_start_chain, nc_start_cert, nc_append, nc_end_cert, nc_end_chain, nc_get_pkey
};

/* ---------------- one GET ---------------- */

static br_ssl_client_context cc;
static u8 iobuf[BR_SSL_BUFSIZE_BIDI];
static br_sslio_context ioc;
static int sock_h;
static u64 read_timeout = 20000000;
static int trace;                                     /* fetch_post: print every step (for photos of the screen) */
static u64 t_start;
static void step(const char *what) { if (trace) { con_color(COL_GRAY); kprintf("  %s (%u ms)\n", what, (u32)((timer_us() - t_start) / 1000)); con_color(COL_DEFAULT); } }                  /* no byte for this long = give up (the AI may think longer) */

static int low_read(void *ctx, unsigned char *b, size_t len)
{
    (void)ctx;
    u64 end = timer_us() + read_timeout;
    for (;;) {
        int n = sock_read(sock_h, b, (int)len);
        if (n > 0) return n;
        if (n < 0) return -1;                       /* closed */
        if (timer_us() > end || stop_key()) return -1;
        delay_us(500);
    }
}

static int low_write(void *ctx, const unsigned char *b, size_t len)
{
    (void)ctx;
    int n = sock_write(sock_h, b, (int)len, stop_key);
    return n > 0 ? n : -1;
}

static int tls, insecure_now;
static int rd(void *b, int n)                       /* body / header bytes, plain or TLS; 0 = end */
{
    if (tls) { int r = br_sslio_read(&ioc, b, (size_t)n); return r < 0 ? 0 : r; }
    int r = low_read(0, b, (size_t)n);
    return r < 0 ? 0 : r;
}

static int wrn(const char *s, int n)
{
    if (tls) return br_sslio_write_all(&ioc, s, (size_t)n) == 0 ? 0 : -1;
    return low_write(0, (const u8 *)s, (size_t)n) == n ? 0 : -1;
}
static int wr(const char *s) { return wrn(s, (int)strlen(s)); }

static int parse_url(const char *u, int *https, char *host, int hmax, int *port, char *path, int pmax)
{
    if (!strncmp(u, "https://", 8)) { *https = 1; u += 8; *port = 443; }
    else if (!strncmp(u, "http://", 7)) { *https = 0; u += 7; *port = 80; }
    else return -1;
    int k = 0;
    while (*u && *u != '/' && *u != ':' && k < hmax - 1) host[k++] = *u++;
    host[k] = 0;
    if (!k) return -1;
    if (*u == ':') { u++; int p = 0; while (*u >= '0' && *u <= '9') p = p * 10 + (*u++ - '0'); *port = p; }
    k = 0;
    if (*u != '/') path[k++] = '/';
    while (*u && k < pmax - 1) path[k++] = *u++;
    path[k] = 0;
    return 0;
}

static int lower_eq(const char *a, const char *b, int n) { for (int i = 0; i < n; i++) if ((a[i] | 32) != (b[i] | 32)) return 0; return 1; }

/* one request: GET (body 0) or POST; extra = more header lines ("Name: value\r\n"...); quiet = do not
 * print the address; status != 0: any HTTP status is accepted (the body is kept) and stored there.
 * Redirects: 5 at most (GET only). The body length, or -1 (fetch_err says why). */
static long fetch_core(const char *url0, const char *extra, const char *body, int blen,
                       u8 *buf, u32 max, int insecure, int quiet, int *status_out)
{
    static char url[512], host[128], path[384], hdr[4096];
    int k = 0;
    for (; url0[k] && k < (int)sizeof(url) - 1; k++) url[k] = url0[k];
    url[k] = 0;
    load_cas();
    insecure_now = insecure;
    for (int hop = 0; hop < 6; hop++) {
        int https, port;
        if (parse_url(url, &https, host, sizeof(host), &port, path, sizeof(path))) { fetch_err = "the address must start with http:// or https://"; return -1; }
        if (quiet) ;
        else if (port != (https ? 443 : 80)) kprintf("  %s://%s:%d%s\n", https ? "https" : "http", host, port, path);
        else kprintf("  %s://%s%s\n", https ? "https" : "http", host, path);
        t_start = timer_us();
        if (trace) { con_color(COL_GRAY); kprintf("  connecting to %s:%d ...\n", host, port); con_color(COL_DEFAULT); }
        sock_h = sock_tcp_connect(host, port, stop_key);
        if (sock_h < 0) { fetch_err = sock_h == -2 ? "name not found (DNS)" : sock_h == -1 ? "no network" : "no connection"; return -1; }
        tls = https;
        if (https) {
            u64 now = clock_now();
            if (!now && !insecure) { sock_close(sock_h); fetch_err = "no time yet (NTP) - a certificate cannot be checked without it"; return -1; }
            if (!now) now = 1767225600ULL;          /* 2026-01-01: only for 'insecure' */
            br_ssl_client_init_full(&cc, &xm, tas, (size_t)ntas);
            br_x509_minimal_set_time(&xm, (uint32_t)(now / 86400 + 719528), (uint32_t)(now % 86400));
            if (insecure) { noch_vt = &noch_class; br_ssl_engine_set_x509(&cc.eng, &noch_vt); }
            br_ssl_engine_set_buffer(&cc.eng, iobuf, sizeof(iobuf), 1);
            u8 seed[32];
            rng_bytes(seed, sizeof(seed));
            br_ssl_engine_inject_entropy(&cc.eng, seed, sizeof(seed));
            int ip = 1;                             /* BearSSL checks DNS names only: no name check for an IP address */
            for (const char *h = host; *h; h++) if (!((*h >= '0' && *h <= '9') || *h == '.')) ip = 0;
            br_ssl_client_reset(&cc, ip || insecure ? 0 : host, 0);
            br_sslio_init(&ioc, &cc.eng, low_read, 0, low_write, 0);
        }
        static char req[1536];
        int n = 0;
        char cl[16]; int ck = 0, bv = blen;
        { char t[12]; int tk = 0; do t[tk++] = (char)('0' + bv % 10); while ((bv /= 10)); while (tk) cl[ck++] = t[--tk]; cl[ck] = 0; }
        const char *parts[] = { body ? "POST " : "GET ", path, " HTTP/1.1\r\nHost: ", host, "\r\nUser-Agent: RoboOS/", os_version(),
                                "\r\nAccept-Encoding: identity\r\nConnection: close\r\n", extra ? extra : "",
                                body ? "Content-Length: " : "", body ? cl : "", body ? "\r\n" : "", "\r\n" };
        for (int i = 0; i < 12; i++) for (const char *s = parts[i]; *s && n < (int)sizeof(req) - 1; s++) req[n++] = *s;
        req[n] = 0;
        step(tls ? "connected, TLS handshake ..." : "connected");
        int werr = wr(req);
        if (!werr && body) { step(tls ? "TLS ok, sending the request ..." : "sending the request ..."); werr = wrn(body, blen); }
        if (!werr && tls) werr = br_sslio_flush(&ioc);
        if (!werr) step("sent, waiting for the answer ...");
        if (werr) {
            int e = tls ? br_ssl_engine_last_error(&cc.eng) : 0;
            sock_close(sock_h);
            fetch_err = e >= 32 && e < 64 ? "the server's certificate is not trusted (BR_ERR_X509; 'insecure' skips the check)"
                      : e ? "TLS handshake failed" : "sending failed";
            if (e) kprintf("  (TLS error %d)\n", e);
            return -1;
        }
        /* headers */
        int hl = 0;
        while (hl < (int)sizeof(hdr) - 1) {
            int r = rd(hdr + hl, 1);
            if (r <= 0) break;
            hl++;
            if (hl >= 4 && !memcmp_n(hdr + hl - 4, "\r\n\r\n", 4)) break;
        }
        hdr[hl] = 0;
        int status = 0;
        if (hl > 12 && !strncmp(hdr, "HTTP/1.", 7)) for (int i = 9; i < 12; i++) status = status * 10 + (hdr[i] - '0');
        if (status) step("answer started, reading ...");
        else if (trace) step("no answer");
        if (!status) {
            int e = tls ? br_ssl_engine_last_error(&cc.eng) : 0;
            sock_close(sock_h);
            fetch_err = e >= 32 && e < 64 ? "the server's certificate is not trusted ('insecure' skips the check)" : e ? "TLS failed" : "no HTTP answer";
            if (e) kprintf("  (TLS error %d)\n", e);
            return -1;
        }
        long clen = -1;
        int chunked = 0;
        char *loc = 0;
        for (char *l = strstr(hdr, "\r\n"); l && l[2] != '\r'; l = strstr(l + 2, "\r\n")) {
            char *h = l + 2;
            if (lower_eq(h, "content-length:", 15)) { char *v = h + 15; while (*v == ' ') v++; clen = 0; while (*v >= '0' && *v <= '9') clen = clen * 10 + (*v++ - '0'); }
            else if (lower_eq(h, "transfer-encoding:", 18) && strstr(h, "chunked") && strstr(h, "chunked") < strstr(h, "\r\n")) chunked = 1;
            else if (lower_eq(h, "location:", 9)) { loc = h + 9; while (*loc == ' ') loc++; }
        }
        if (status >= 300 && status < 400 && loc && !body) {
            char *e = strstr(loc, "\r\n"); if (e) *e = 0;
            if (loc[0] == '/') {                    /* relative: same server */
                char nu[512]; int j = 0;
                const char *pre = https ? "https://" : "http://";
                for (const char *s = pre; *s; s++) nu[j++] = *s;
                for (const char *s = host; *s && j < 400; s++) nu[j++] = *s;
                if (port != (https ? 443 : 80)) {   /* keep a non-standard port */
                    char pb[8]; int pk = 0, pv = port;
                    do pb[pk++] = (char)('0' + pv % 10); while ((pv /= 10) && pk < 7);
                    nu[j++] = ':'; while (pk) nu[j++] = pb[--pk];
                }
                for (const char *s = loc; *s && j < 510; s++) nu[j++] = *s;
                nu[j] = 0;
                memcpy(url, nu, (size_t)j + 1);
            } else { int j = 0; for (; loc[j] && j < (int)sizeof(url) - 1; j++) url[j] = loc[j]; url[j] = 0; }
            if (tls) br_sslio_close(&ioc);
            sock_close(sock_h);
            kprintf("  -> redirect %d\n", status);
            continue;
        }
        if (status_out) *status_out = status;
        if (status != 200 && !status_out) {
            sock_close(sock_h);
            static char m[40] = "the server answered HTTP ";
            m[25] = (char)('0' + status / 100); m[26] = (char)('0' + status / 10 % 10); m[27] = (char)('0' + status % 10); m[28] = 0;
            fetch_err = m;
            return -1;
        }
        if (clen > (long)max) { sock_close(sock_h); fetch_err = "the file is too big (16 MB at most)"; return -1; }
        /* body */
        u32 got = 0, shown = 0;
        if (chunked) {
            for (;;) {
                char line[24]; int ll = 0;
                while (ll < 23) { int r = rd(line + ll, 1); if (r <= 0) break; if (line[ll] == '\n') break; ll++; }
                line[ll] = 0;
                u32 cs = 0;
                for (int i = 0; i < ll; i++) { char ch = line[i]; int v = ch >= '0' && ch <= '9' ? ch - '0' : (ch | 32) >= 'a' && (ch | 32) <= 'f' ? (ch | 32) - 'a' + 10 : -1; if (v < 0) break; cs = cs * 16 + (u32)v; }
                if (!cs) break;
                if (got + cs > max) { sock_close(sock_h); fetch_err = "the file is too big (16 MB at most)"; return -1; }
                while (cs) { int r = rd(buf + got, (int)(cs > 16384 ? 16384 : cs)); if (r <= 0) break; got += (u32)r; cs -= (u32)r; }
                if (cs) break;
                char crlf[2]; rd(crlf, 2);
            }
        } else {
            while (clen < 0 || got < (u32)clen) {
                u32 want = clen < 0 ? max - got : (u32)clen - got;
                if (!want) break;
                int r = rd(buf + got, (int)(want > 16384 ? 16384 : want));
                if (r <= 0) break;
                got += (u32)r;
                if (!quiet && got - shown >= 131072) { kprintf("  %u KB\r", got / 1024); shown = got; }
            }
        }
        if (clen >= 0 && got != (u32)clen) { fetch_err = "the download broke off"; sock_close(sock_h); return -1; }
        if (tls) br_sslio_close(&ioc);
        sock_close(sock_h);
        return (long)got;
    }
    fetch_err = "too many redirects";
    return -1;
}

long fetch_url(const char *url, u8 *buf, u32 max, int insecure)
{
    read_timeout = 20000000;
    return fetch_core(url, 0, 0, 0, buf, max, insecure, 0, 0);
}

/* POST body (JSON etc.) with extra header lines; any HTTP status comes back in *status with its body */
long fetch_post(const char *url, const char *extra, const char *body, int blen,
                u8 *buf, u32 max, int *status, u32 timeout_s)
{
    read_timeout = (u64)timeout_s * 1000000;
    *status = 0;
    trace = 1;
    long r = fetch_core(url, extra, body, blen, buf, max, 0, 1, status);
    if (r >= 0) step("done");
    trace = 0;
    read_timeout = 20000000;
    return r;
}
