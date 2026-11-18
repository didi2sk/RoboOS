/*
 * TLS (HTTPS) for the web server, with BearSSL (third_party/bearssl, MIT).
 *
 * The certificate and the key are /TLS/CERT.PEM and /TLS/KEY.PEM on the SD card
 * (PEM or DER; EC or RSA, e.g. made with openssl). Without them the board makes
 * its own: an ECDSA P-256 key and a self-signed certificate for its name (and
 * address), valid 20 years - browsers warn about it, curl needs -k or --cacert.
 * Loading / making happens on core 0 (the SD card); the connections run on core 1
 * (ip.c pumps the BearSSL engine from the lwIP callbacks).
 */
#include "kernel.h"
#include "sd.h"
#include "bearssl.h"

static u8 cert_der[4][2048];
static br_x509_certificate chain[4];
static int chain_len;
static br_skey_decoder_context skey;       /* keeps the decoded key */
static int key_type;                       /* BR_KEYTYPE_EC / _RSA, 0 = none */
static char fingerprint[100];
static const char *err = "";

int tls_ready(void) { return key_type && chain_len; }
const char *tls_error(void) { return err; }
const char *tls_fingerprint(void) { return fingerprint; }

/* ---------------- reading PEM / DER ---------------- */

struct pem_out { u8 *buf; int len, max; };
static void pem_dest(void *ctx, const void *src, size_t len)
{
    struct pem_out *o = ctx;
    if (o->len + (int)len <= o->max) memcpy(o->buf + o->len, src, len);
    o->len += (int)len;
}

static u8 obj[4096];

/* every object of a PEM text: CERTIFICATE -> the chain, *PRIVATE KEY -> the key */
static int load_pem(const u8 *t, int n, int want_key)
{
    br_pem_decoder_context pc;
    struct pem_out o = { obj, 0, sizeof(obj) };
    int in_obj = 0, found = 0, is_key = 0;
    br_pem_decoder_init(&pc);
    while (n > 0) {
        size_t k = br_pem_decoder_push(&pc, t, (size_t)n);
        t += k; n -= (int)k;
        switch (br_pem_decoder_event(&pc)) {
        case BR_PEM_BEGIN_OBJ: {
            const char *name = br_pem_decoder_name(&pc);
            is_key = strstr(name, "PRIVATE KEY") != 0;
            o.len = 0;
            in_obj = (want_key == is_key);
            br_pem_decoder_setdest(&pc, in_obj ? pem_dest : 0, &o);
            break;
        }
        case BR_PEM_END_OBJ:
            if (in_obj && o.len <= o.max) {
                if (is_key) {
                    br_skey_decoder_init(&skey);
                    br_skey_decoder_push(&skey, obj, (size_t)o.len);
                    key_type = br_skey_decoder_key_type(&skey);
                    if (key_type) found++;
                } else if (chain_len < 4 && o.len <= (int)sizeof(cert_der[0])) {
                    memcpy(cert_der[chain_len], obj, (size_t)o.len);
                    chain[chain_len].data = cert_der[chain_len];
                    chain[chain_len].data_len = (size_t)o.len;
                    chain_len++;
                    found++;
                }
            }
            in_obj = 0;
            break;
        case BR_PEM_ERROR:
            return found;
        }
    }
    return found;
}

static int load_file(const char *path, int want_key)
{
    static u8 b[8192];
    struct fat_file f;
    if (fat_open(path, &f) || f.is_dir || f.size == 0 || f.size > sizeof(b)) return 0;
    int n = fat_read(&f, b, f.size);
    if (n <= 0) return 0;
    if (n > 10 && !memcmp_n(b, "-----BEGIN", 10)) return load_pem(b, n, want_key);
    if (want_key) {                                       /* DER */
        br_skey_decoder_init(&skey);
        br_skey_decoder_push(&skey, b, (size_t)n);
        key_type = br_skey_decoder_key_type(&skey);
        return key_type != 0;
    }
    if (n > (int)sizeof(cert_der[0])) return 0;
    memcpy(cert_der[0], b, (size_t)n);
    chain[0].data = cert_der[0];
    chain[0].data_len = (size_t)n;
    chain_len = 1;
    return 1;
}

static void make_fingerprint(void)
{
    br_sha256_context h;
    u8 d[32];
    static const char hex[] = "0123456789ABCDEF";
    br_sha256_init(&h);
    br_sha256_update(&h, chain[0].data, chain[0].data_len);
    br_sha256_out(&h, d);
    int n = 0;
    for (int i = 0; i < 32; i++) {
        fingerprint[n++] = hex[d[i] >> 4];
        fingerprint[n++] = hex[d[i] & 15];
        if (i < 31) fingerprint[n++] = ':';
    }
    fingerprint[n] = 0;
}

/* ---------------- making a self-signed certificate ---------------- */

static int der_len(u8 *o, int len)                        /* length bytes; returns how many */
{
    if (len < 128) { o[0] = (u8)len; return 1; }
    if (len < 256) { o[0] = 0x81; o[1] = (u8)len; return 2; }
    o[0] = 0x82; o[1] = (u8)(len >> 8); o[2] = (u8)len; return 3;
}

/* tag + length + content, appended at o[*n] */
static void tlv(u8 *o, int *n, int tag, const u8 *c, int len)
{
    o[(*n)++] = (u8)tag;
    *n += der_len(o + *n, len);
    memmove(o + *n, c, (size_t)len);
    *n += len;
}

static void utctime(u8 *o, int *n, u64 t)
{
    char b[32], u[13];
    clock_format(b, t - (u64)((long)clock_offset(t) * 60), 0);   /* clock_format adds the zone: take it off = UTC */
    /* "2026-11-18 21:05:07" -> "261118210507Z" */
    const int pos[12] = { 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18 };
    for (int i = 0; i < 12; i++) u[i] = b[pos[i]];
    u[12] = 'Z';
    tlv(o, n, 0x17, (const u8 *)u, 13);
}

static const u8 OID_ECDSA_SHA256[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02 };
static const u8 OID_EC_PUBKEY[]    = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01 };
static const u8 OID_P256[]         = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 };
static const u8 OID_CN[]           = { 0x55, 0x04, 0x03 };
static const u8 OID_SAN[]          = { 0x55, 0x1D, 0x11 };

static int name_der(u8 *o, const char *cn)
{
    u8 a[96], s[100];
    int an = 0, sn = 0, n = 0;
    tlv(a, &an, 0x06, OID_CN, sizeof(OID_CN));
    tlv(a, &an, 0x0C, (const u8 *)cn, (int)strlen(cn));    /* UTF8String */
    tlv(s, &sn, 0x30, a, an);
    u8 set[104]; int setn = 0;
    tlv(set, &setn, 0x31, s, sn);
    tlv(o, &n, 0x30, set, setn);
    return n;
}

static int write_pem(const char *path, const u8 *der, int len, const char *banner)
{
    static char pem[4096];
    size_t n = br_pem_encode(pem, der, (size_t)len, banner, BR_PEM_LINE64 | BR_PEM_CRLF);
    return fat_write_file(path, pem, (u32)n);
}

/* ip: the board's address as text (may be empty) */
static int generate(const char *ip)
{
    br_hmac_drbg_context rng;
    u8 seed[48];
    rng_bytes(seed, sizeof(seed));
    br_hmac_drbg_init(&rng, &br_sha256_vtable, seed, sizeof(seed));

    const br_ec_impl *ec = br_ec_get_default();
    static u8 kbuf[BR_EC_KBUF_PRIV_MAX_SIZE], pbuf[BR_EC_KBUF_PUB_MAX_SIZE];
    br_ec_private_key sk;
    br_ec_public_key pk;
    if (!br_ec_keygen(&rng.vtable, ec, &sk, kbuf, BR_EC_secp256r1)) { err = "key generation failed"; return 0; }
    br_ec_compute_pub(ec, &pk, pbuf, &sk);

    static u8 tbs[1024], t[512], u[512], cert[1400];
    int tn = 0, un = 0, cn = 0;
    const char *name = net_name();

    /* version 3, serial, algorithm */
    static const u8 v3[] = { 0x02, 0x01, 0x02 };
    tlv(tbs, &tn, 0xA0, v3, 3);
    u8 serial[9];
    rng_bytes(serial + 1, 8);
    serial[0] = 0x00; serial[1] &= 0x7F; serial[1] |= 0x01;   /* positive, not zero */
    tlv(tbs, &tn, 0x02, serial + 1, 8);
    un = 0; tlv(u, &un, 0x06, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256));
    tlv(tbs, &tn, 0x30, u, un);
    /* issuer, validity, subject */
    tn += name_der(tbs + tn, name);
    u64 now = clock_now();
    u64 from = now ? now - 86400 : 1735689600ULL;            /* 2025-01-01 if the time is not known */
    un = 0;
    utctime(u, &un, from);
    utctime(u, &un, from + 20ULL * 365 * 86400);
    tlv(tbs, &tn, 0x30, u, un);
    tn += name_der(tbs + tn, name);
    /* the public key */
    un = 0;
    tlv(u, &un, 0x06, OID_EC_PUBKEY, sizeof(OID_EC_PUBKEY));
    tlv(u, &un, 0x06, OID_P256, sizeof(OID_P256));
    int tsn = 0;
    tlv(t, &tsn, 0x30, u, un);
    u8 bits[80];
    bits[0] = 0;
    memcpy(bits + 1, pk.q, pk.qlen);
    tlv(t, &tsn, 0x03, bits, (int)pk.qlen + 1);
    tlv(tbs, &tn, 0x30, t, tsn);
    /* extension: subjectAltName = DNS:<name> [, IP:<address>] */
    u8 san[80]; int sann = 0;
    tlv(san, &sann, 0x82, (const u8 *)name, (int)strlen(name));
    int a[4], k = 0, v = 0, ok = ip && *ip;
    for (const char *p = ip; ok && *p && k < 4; p++) {
        if (*p == '.') { a[k++] = v; v = 0; } else if (*p >= '0' && *p <= '9') v = v * 10 + (*p - '0'); else ok = 0;
    }
    if (ok && k == 3) { a[3] = v; u8 ipb[4] = { (u8)a[0], (u8)a[1], (u8)a[2], (u8)a[3] }; if (a[0]) tlv(san, &sann, 0x87, ipb, 4); }
    u8 sanseq[90]; int ssn = 0;
    tlv(sanseq, &ssn, 0x30, san, sann);
    un = 0;
    tlv(u, &un, 0x06, OID_SAN, sizeof(OID_SAN));
    tlv(u, &un, 0x04, sanseq, ssn);
    u8 ext[120]; int en = 0, exn = 0;
    tlv(ext, &en, 0x30, u, un);
    u8 exts[130];
    tlv(exts, &exn, 0x30, ext, en);
    tlv(tbs, &tn, 0xA3, exts, exn);
    /* tbsCertificate */
    static u8 tbsseq[1100]; int tq = 0;
    tlv(tbsseq, &tq, 0x30, tbs, tn);

    /* sign: SHA-256, ECDSA (DER) */
    br_sha256_context h;
    u8 hash[32], sig[80];
    br_sha256_init(&h);
    br_sha256_update(&h, tbsseq, (size_t)tq);
    br_sha256_out(&h, hash);
    size_t sl = br_ecdsa_sign_asn1_get_default()(ec, &br_sha256_vtable, hash, &sk, sig);
    if (!sl) { err = "signing failed"; return 0; }

    memcpy(u, tbsseq, (size_t)tq); un = tq;
    u8 alg[16]; int an = 0;
    tlv(alg, &an, 0x06, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256));
    tlv(u, &un, 0x30, alg, an);
    u8 sb[84]; sb[0] = 0; memcpy(sb + 1, sig, sl);
    tlv(u, &un, 0x03, sb, (int)sl + 1);
    tlv(cert, &cn, 0x30, u, un);

    /* the key as SEC1 "EC PRIVATE KEY": version 1, key, [0] curve, [1] public key */
    static u8 key[200], kd[200];
    int kn = 0, kdn = 0;
    static const u8 one[] = { 0x01 };
    tlv(kd, &kdn, 0x02, one, 1);
    tlv(kd, &kdn, 0x04, sk.x, (int)sk.xlen);
    u8 c0[12]; int c0n = 0;
    tlv(c0, &c0n, 0x06, OID_P256, sizeof(OID_P256));
    tlv(kd, &kdn, 0xA0, c0, c0n);
    u8 c1[80]; int c1n = 0;
    tlv(c1, &c1n, 0x03, bits, (int)pk.qlen + 1);
    tlv(kd, &kdn, 0xA1, c1, c1n);
    tlv(key, &kn, 0x30, kd, kdn);

    fat_mkdir("/TLS");                                       /* may exist already */
    if (write_pem("/TLS/KEY.PEM", key, kn, "EC PRIVATE KEY") || write_pem("/TLS/CERT.PEM", cert, cn, "CERTIFICATE")) {
        err = "cannot write /TLS on the SD card";
        return 0;
    }
    return 1;
}

/* core 0: load /TLS/CERT.PEM + /TLS/KEY.PEM, make them first if they are missing (or renew = 1) */
int tls_prepare(int renew, const char *ip)
{
    if (smp_core() != 0) return 0;
    if (!fat_mounted()) { err = "no SD card"; return 0; }
    struct fat_file f;
    int have = !fat_open("/TLS/CERT.PEM", &f) || !fat_open("/TLS/CERT.DER", &f);
    if (renew || !have) {
        kprintf("making a key and a self-signed certificate (/TLS)...\n");
        if (!generate(ip)) return 0;
    }
    chain_len = 0; key_type = 0;
    if (!load_file("/TLS/CERT.PEM", 0)) load_file("/TLS/CERT.DER", 0);
    if (!load_file("/TLS/KEY.PEM", 1)) load_file("/TLS/KEY.DER", 1);
    if (!chain_len) { err = "no certificate in /TLS/CERT.PEM"; return 0; }
    if (!key_type) { err = "no usable key in /TLS/KEY.PEM (EC or RSA)"; return 0; }
    make_fingerprint();
    return 1;
}

/* ---------------- connections (core 1) ---------------- */

#define TLS_CONNS 4
struct tls_conn {
    br_ssl_server_context sc;
    u8 iobuf[BR_SSL_BUFSIZE_BIDI];
    int used;
};
static struct tls_conn tc[TLS_CONNS];
static br_ssl_session_cache_lru cache;
static u8 cache_buf[4096];
static int cache_on;

struct tls_conn *tls_open(void)
{
    if (!tls_ready()) return 0;
    struct tls_conn *c = 0;
    for (int i = 0; i < TLS_CONNS; i++) if (!tc[i].used) { c = &tc[i]; break; }
    if (!c) return 0;
    if (!cache_on) { br_ssl_session_cache_lru_init(&cache, cache_buf, sizeof(cache_buf)); cache_on = 1; }
    if (key_type == BR_KEYTYPE_EC)
        br_ssl_server_init_full_ec(&c->sc, chain, (size_t)chain_len, BR_KEYTYPE_EC, br_skey_decoder_get_ec(&skey));
    else
        br_ssl_server_init_full_rsa(&c->sc, chain, (size_t)chain_len, br_skey_decoder_get_rsa(&skey));
    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof(c->iobuf), 1);
    br_ssl_server_set_cache(&c->sc, &cache.vtable);
    u8 seed[32];
    rng_bytes(seed, sizeof(seed));
    br_ssl_engine_inject_entropy(&c->sc.eng, seed, sizeof(seed));
    if (!br_ssl_server_reset(&c->sc)) return 0;
    c->used = 1;
    return c;
}

br_ssl_engine_context *tls_engine(struct tls_conn *c) { return &c->sc.eng; }
void tls_close(struct tls_conn *c) { if (c) c->used = 0; }
void tls_close_all(void) { for (int i = 0; i < TLS_CONNS; i++) tc[i].used = 0; }
