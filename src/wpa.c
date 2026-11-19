/*
 * WPA2-PSK supplicant - see wpa.h. No kernel headers on purpose (tools/test_wpa.c builds it on the PC).
 */
#include "wpa.h"
#include "bearssl.h"

static void cpy(unsigned char *d, const unsigned char *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }
static void zero(unsigned char *d, int n) { for (int i = 0; i < n; i++) d[i] = 0; }
static int cmpb(const unsigned char *a, const unsigned char *b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1; return 0; }
static int ctequal(const unsigned char *a, const unsigned char *b, int n) { unsigned char d = 0; for (int i = 0; i < n; i++) d |= a[i] ^ b[i]; return d == 0; }

void wpa_hmac_sha1(const unsigned char *key, int klen, const unsigned char *d, int dlen, unsigned char out[20])
{
    br_hmac_key_context kc;
    br_hmac_context hc;
    br_hmac_key_init(&kc, &br_sha1_vtable, key, (size_t)klen);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, d, (size_t)dlen);
    br_hmac_out(&hc, out);
}

void wpa_pmk(const char *pass, const char *ssid, int ssid_len, unsigned char pmk[32])
{
    br_hmac_key_context kc;
    int pl = 0;
    while (pass[pl]) pl++;
    br_hmac_key_init(&kc, &br_sha1_vtable, pass, (size_t)pl);
    for (int blk = 1; blk <= 2; blk++) {                    /* 2 x 20 bytes, the first 32 are used */
        unsigned char u[20], t[20], cnt[4] = { 0, 0, 0, (unsigned char)blk };
        br_hmac_context hc;
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, ssid, (size_t)ssid_len);
        br_hmac_update(&hc, cnt, 4);
        br_hmac_out(&hc, u);
        cpy(t, u, 20);
        for (int i = 1; i < 4096; i++) {
            br_hmac_init(&hc, &kc, 0);
            br_hmac_update(&hc, u, 20);
            br_hmac_out(&hc, u);
            for (int k = 0; k < 20; k++) t[k] ^= u[k];
        }
        cpy(pmk + (blk - 1) * 20, t, blk == 1 ? 20 : 12);
    }
}

/* PRF-n: HMAC-SHA1(K, label || 0 || data || i), i = 0, 1, ... */
void wpa_prf(const unsigned char *key, int klen, const char *label, const unsigned char *data, int dlen,
             unsigned char *out, int olen)
{
    br_hmac_key_context kc;
    br_hmac_key_init(&kc, &br_sha1_vtable, key, (size_t)klen);
    int ll = 0;
    while (label[ll]) ll++;
    for (int i = 0, pos = 0; pos < olen; i++) {
        br_hmac_context hc;
        unsigned char z = 0, c = (unsigned char)i, h[20];
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, label, (size_t)ll);
        br_hmac_update(&hc, &z, 1);
        br_hmac_update(&hc, data, (size_t)dlen);
        br_hmac_update(&hc, &c, 1);
        br_hmac_out(&hc, h);
        int n = olen - pos < 20 ? olen - pos : 20;
        cpy(out + pos, h, n);
        pos += n;
    }
}

/* AES key unwrap (RFC 3394); len = wrapped length (multiple of 8, >= 24); out gets len - 8 bytes */
int wpa_unwrap(const unsigned char kek[16], const unsigned char *in, int len, unsigned char *out)
{
    if (len < 24 || len % 8 || len > 8 + 256) return -1;
    int n = len / 8 - 1;
    unsigned char a[8], b[16], iv[16];
    br_aes_ct_cbcdec_keys dc;
    br_aes_ct_cbcdec_init(&dc, kek, 16);
    cpy(a, in, 8);
    cpy(out, in + 8, n * 8);
    for (int j = 5; j >= 0; j--)
        for (int i = n; i >= 1; i--) {
            unsigned t = (unsigned)(n * j + i);
            cpy(b, a, 8);
            b[4] ^= (unsigned char)(t >> 24); b[5] ^= (unsigned char)(t >> 16);
            b[6] ^= (unsigned char)(t >> 8); b[7] ^= (unsigned char)t;
            cpy(b + 8, out + (i - 1) * 8, 8);
            zero(iv, 16);
            br_aes_ct_cbcdec_run(&dc, iv, b, 16);         /* one block with IV 0 = plain AES decryption */
            cpy(a, b, 8);
            cpy(out + (i - 1) * 8, b + 8, 8);
        }
    for (int i = 0; i < 8; i++) if (a[i] != 0xA6) return -1;
    return n * 8;
}

/* the usual RSN element of a WPA2-PSK station: CCMP group + pairwise, PSK, no capabilities */
const unsigned char wpa_rsn_default[22] = {
    0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x00 };

void wpa_start(struct wpa *s, const unsigned char pmk[32], const unsigned char own_mac[6],
               const unsigned char *ie, int ielen)
{
    zero((unsigned char *)s, (int)sizeof(*s));
    cpy(s->pmk, pmk, 32);
    cpy(s->spa, own_mac, 6);
    if (!ie || ielen < 2 || ielen > (int)sizeof(s->ie)) { ie = wpa_rsn_default; ielen = (int)sizeof(wpa_rsn_default); }
    cpy(s->ie, ie, ielen);
    s->ielen = ielen;
}

/* EAPOL-Key body offsets (after the 4-byte EAPOL header) */
enum { K_DESC = 0, K_INFO = 1, K_LEN = 3, K_REPLAY = 5, K_NONCE = 13, K_IV = 45, K_RSC = 61, K_MIC = 77, K_DLEN = 93, K_DATA = 95 };
#define KI_VER     0x0007
#define KI_PAIR    0x0008
#define KI_INSTALL 0x0040
#define KI_ACK     0x0080
#define KI_MIC     0x0100
#define KI_SECURE  0x0200
#define KI_ENC     0x1000

static void derive_ptk(struct wpa *s)
{
    unsigned char d[76];
    int a = cmpb(s->aa, s->spa, 6) < 0, n = cmpb(s->anonce, s->snonce, 32) < 0;
    cpy(d, a ? s->aa : s->spa, 6);
    cpy(d + 6, a ? s->spa : s->aa, 6);
    cpy(d + 12, n ? s->anonce : s->snonce, 32);
    cpy(d + 44, n ? s->snonce : s->anonce, 32);
    wpa_prf(s->pmk, 32, "Pairwise key expansion", d, 76, s->ptk, 48);
    s->have_ptk = 1;
}

/* EAPOL-Key frame into s->out: ki, replay, nonce, data; then the MIC */
static void reply(struct wpa *s, int ver, unsigned ki, const unsigned char *replay, const unsigned char *nonce,
                  const unsigned char *data, int dlen)
{
    unsigned char *f = s->out;
    int blen = K_DATA + dlen;
    zero(f, 14 + 4 + blen);
    cpy(f, s->aa, 6);
    cpy(f + 6, s->spa, 6);
    f[12] = 0x88; f[13] = 0x8E;
    unsigned char *e = f + 14, *k = e + 4;
    e[0] = (unsigned char)ver; e[1] = 3; e[2] = (unsigned char)(blen >> 8); e[3] = (unsigned char)blen;
    k[K_DESC] = 2;                                         /* RSN key descriptor */
    k[K_INFO] = (unsigned char)(ki >> 8); k[K_INFO + 1] = (unsigned char)ki;
    cpy(k + K_REPLAY, replay, 8);
    if (nonce) cpy(k + K_NONCE, nonce, 32);
    k[K_DLEN] = (unsigned char)(dlen >> 8); k[K_DLEN + 1] = (unsigned char)dlen;
    if (dlen) cpy(k + K_DATA, data, dlen);
    unsigned char mic[20];
    wpa_hmac_sha1(s->ptk, 16, e, 4 + blen, mic);           /* KCK */
    cpy(k + K_MIC, mic, 16);
    s->outlen = 14 + 4 + blen;
}

/* the GTK out of the (decrypted) key data: KDE dd len 00-0F-AC 01 keyid 0 gtk */
static int find_gtk(struct wpa *s, const unsigned char *d, int n)
{
    for (int i = 0; i + 2 <= n; ) {
        int t = d[i], l = d[i + 1];
        if (i + 2 + l > n) break;
        if (t == 0xDD && l >= 6 && d[i + 2] == 0x00 && d[i + 3] == 0x0F && d[i + 4] == 0xAC && d[i + 5] == 1) {
            int gl = l - 6;
            if (gl != 16 && gl != 32) { s->err = "GTK has an odd length"; return -1; }
            s->gtk_idx = d[i + 6] & 3;
            s->gtk_len = gl;
            cpy(s->gtk, d + i + 8, gl);
            return 0;
        }
        if (t == 0xDD && l == 0) break;                    /* padding */
        i += 2 + l;
    }
    s->err = "no GTK in the key data";
    return -1;
}

int wpa_rx(struct wpa *s, const unsigned char *f, int len)
{
    s->outlen = 0;
    s->err = 0;
    if (len < 14 + 4 + K_DATA || f[12] != 0x88 || f[13] != 0x8E) { s->err = "not EAPOL-Key"; return 0; }
    const unsigned char *e = f + 14, *k = e + 4;
    int blen = e[2] << 8 | e[3];
    if (e[1] != 3 || blen < K_DATA || 4 + blen > len - 14) { s->err = "not EAPOL-Key"; return 0; }
    if (k[K_DESC] != 2) { s->err = "only RSN (WPA2) keys are supported"; return 0; }
    unsigned ki = (unsigned)(k[K_INFO] << 8 | k[K_INFO + 1]);
    int ver = (int)(ki & KI_VER), dlen = k[K_DLEN] << 8 | k[K_DLEN + 1];
    if (ver != 2) { s->err = ver == 1 ? "TKIP pairwise (WPA1) is not supported" : "this key version needs PMF/SHA-256 (not supported)"; return 0; }
    if (K_DATA + dlen > blen) { s->err = "bad key data length"; return 0; }
    if (!(ki & KI_ACK)) { s->err = "not from the access point"; return 0; }
    if (s->have_replay && cmpb(k + K_REPLAY, s->replay, 8) <= 0) { s->err = "old replay counter"; return 0; }

    if ((ki & KI_PAIR) && !(ki & KI_MIC)) {               /* message 1 of 4 */
        cpy(s->aa, f + 6, 6);
        cpy(s->anonce, k + K_NONCE, 32);
        wpa_random(s->snonce, 32);
        derive_ptk(s);
        cpy(s->replay, k + K_REPLAY, 8);                   /* msg1 has no MIC: remember it, not trusted */
        s->have_replay = 1;
        reply(s, e[0], 2 | KI_PAIR | KI_MIC, k + K_REPLAY, s->snonce, s->ie, s->ielen);
        s->last = 1;
        return 1;
    }
    if (!s->have_ptk) { s->err = "key message before message 1"; return 0; }
    /* every other message carries a MIC made with the KCK */
    {
        unsigned char tmp[4 + 95 + 256], mic[20];
        if (4 + blen > (int)sizeof(tmp)) { s->err = "frame too long"; return 0; }
        cpy(tmp, e, 4 + blen);
        zero(tmp + 4 + K_MIC, 16);
        wpa_hmac_sha1(s->ptk, 16, tmp, 4 + blen, mic);
        if (!ctequal(mic, k + K_MIC, 16)) { s->err = "MIC is wrong (wrong password?)"; return 0; }
    }
    unsigned char kd[256];
    int kdl = 0;
    if (ki & KI_ENC) {
        kdl = wpa_unwrap(s->ptk + 16, k + K_DATA, dlen, kd);   /* KEK */
        if (kdl < 0) { s->err = "cannot unwrap the key data"; return 0; }
    }
    if (ki & KI_PAIR) {                                    /* message 3 of 4 */
        if (!(ki & KI_INSTALL) || !(ki & KI_ENC)) { s->err = "message 3 without install / encrypted data"; return 0; }
        if (!ctequal(k + K_NONCE, s->anonce, 32)) { s->err = "ANonce changed"; return 0; }
        if (find_gtk(s, kd, kdl)) return 0;
        cpy(s->replay, k + K_REPLAY, 8);
        cpy(s->gtk_rsc, k + K_RSC, 8);
        reply(s, e[0], 2 | KI_PAIR | KI_MIC | KI_SECURE, k + K_REPLAY, 0, 0, 0);
        s->set_ptk = 1;
        s->set_gtk = 1;
        s->done = 1;
        s->last = 3;
        return 1;
    }
    /* group key message 1 of 2 (rekeying) */
    if (!(ki & KI_ENC)) { s->err = "group message without encrypted data"; return 0; }
    if (find_gtk(s, kd, kdl)) return 0;
    cpy(s->replay, k + K_REPLAY, 8);
    cpy(s->gtk_rsc, k + K_RSC, 8);
    reply(s, e[0], 2 | KI_MIC | KI_SECURE, k + K_REPLAY, 0, 0, 0);
    s->set_gtk = 1;
    s->last = 5;
    return 1;
}
