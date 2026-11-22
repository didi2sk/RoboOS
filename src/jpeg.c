/*
 * jpeg.c - small baseline JPEG encoder (0.103, own code) for camera snapshots.
 *
 * Integer only (the kernel is built with -mgeneral-regs-only): 8x8 DCT as two passes of a fixed-point
 * cosine matrix (x16384), the example quantisation + Huffman tables of the JPEG standard (ITU T.81
 * Annex K), quality scaled like the IJG library (q < 50: 5000/q %, else 200 - 2q %), colour as YCbCr
 * 4:2:0 (one 16x16 MCU = 4 Y + Cb + Cr blocks), edges repeated to the MCU size, JFIF header.
 * Input: 0x00RRGGBB pixels, `stride` pixels per row. Host test: tools/test_jpeg.c (decoded by PIL).
 */
#include "jpeg.h"

static const short dctm[8][8] = {
    {5793, 5793, 5793, 5793, 5793, 5793, 5793, 5793},
    {8035, 6811, 4551, 1598, -1598, -4551, -6811, -8035},
    {7568, 3135, -3135, -7568, -7568, -3135, 3135, 7568},
    {6811, -1598, -8035, -4551, 4551, 8035, 1598, -6811},
    {5793, -5793, -5793, 5793, 5793, -5793, -5793, 5793},
    {4551, -8035, 1598, 6811, -6811, -1598, 8035, -4551},
    {3135, -7568, 7568, -3135, -3135, 7568, -7568, 3135},
    {1598, -4551, 6811, -8035, 8035, -6811, 4551, -1598},
};

static const unsigned char zz[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63 };

static const unsigned char qlum[64] = {   /* natural order */
    16,11,10,16,24,40,51,61, 12,12,14,19,26,58,60,55, 14,13,16,24,40,57,69,56, 14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77, 24,35,55,64,81,104,113,92, 49,64,78,87,103,121,120,101, 72,92,95,98,112,100,103,99 };
static const unsigned char qchr[64] = {
    17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99, 24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99 };

static const unsigned char dc_lum_bits[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const unsigned char dc_chr_bits[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const unsigned char dc_vals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
static const unsigned char ac_lum_bits[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const unsigned char ac_lum_vals[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,
    0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
    0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,
    0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa };
static const unsigned char ac_chr_bits[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const unsigned char ac_chr_vals[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,
    0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,
    0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,
    0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa };

struct huff { unsigned short code[256]; unsigned char len[256]; };

static void build(struct huff *h, const unsigned char *bits, const unsigned char *vals)
{
    unsigned code = 0; int k = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l - 1]; i++) { h->code[vals[k]] = (unsigned short)code; h->len[vals[k]] = (unsigned char)l; code++; k++; }
        code <<= 1;
    }
}

struct enc {
    unsigned char *out; int n, max, over;
    unsigned acc; int nbits;
    unsigned char ql[64], qc[64];       /* natural order */
    struct huff dl, dc, al, ac;
};

static void put(struct enc *e, int b) { if (e->n < e->max) e->out[e->n++] = (unsigned char)b; else e->over = 1; }
static void put16(struct enc *e, int v) { put(e, v >> 8); put(e, v & 0xFF); }

static void bits(struct enc *e, unsigned code, int len)
{
    e->acc = (e->acc << len) | (code & ((1u << len) - 1));
    e->nbits += len;
    while (e->nbits >= 8) {
        int b = (int)((e->acc >> (e->nbits - 8)) & 0xFF);
        put(e, b);
        if (b == 0xFF) put(e, 0);       /* byte stuffing */
        e->nbits -= 8;
    }
}

static void flush_bits(struct enc *e) { if (e->nbits) bits(e, 0x7F, 8 - e->nbits); }

/* forward DCT of 64 samples (already minus 128) + quantisation -> zig-zag coefficients */
static void fdct_q(const int *s, const unsigned char *q, int *outz)
{
    int t[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            int a = 0;
            for (int x = 0; x < 8; x++) a += s[y * 8 + x] * dctm[u][x];
            t[y * 8 + u] = (a + (1 << 10)) >> 11;          /* x8 kept */
        }
    for (int v = 0; v < 8; v++)
        for (int u = 0; u < 8; u++) {
            int a = 0;
            for (int y = 0; y < 8; y++) a += t[y * 8 + u] * dctm[v][y];
            int c = (a + (1 << 16)) >> 17;                   /* /16384 /8 */
            int d = q[v * 8 + u];
            c = c >= 0 ? (c + d / 2) / d : -((-c + d / 2) / d);
            outz[v * 8 + u] = c;
        }
}

static int magn(int v, unsigned *code)
{
    int a = v < 0 ? -v : v, n = 0;
    while (a >> n) n++;
    *code = v < 0 ? (unsigned)(v - 1) : (unsigned)v;    /* negative: one's complement in n bits */
    return n;
}

static void block(struct enc *e, const int *s, const unsigned char *q, struct huff *hd, struct huff *ha, int *pred)
{
    int c[64];
    fdct_q(s, q, c);
    unsigned code; int n;
    int diff = c[0] - *pred; *pred = c[0];
    n = magn(diff, &code);
    bits(e, hd->code[n], hd->len[n]);
    if (n) bits(e, code, n);
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int v = c[zz[k]];
        if (!v) { run++; continue; }
        while (run > 15) { bits(e, ha->code[0xF0], ha->len[0xF0]); run -= 16; }
        n = magn(v, &code);
        int sym = (run << 4) | n;
        bits(e, ha->code[sym], ha->len[sym]);
        bits(e, code, n);
        run = 0;
    }
    if (run) bits(e, ha->code[0], ha->len[0]);
}

static void dht(struct enc *e, int cls_id, const unsigned char *b, const unsigned char *v)
{
    int n = 0; for (int i = 0; i < 16; i++) n += b[i];
    put16(e, 0xFFC4); put16(e, 2 + 1 + 16 + n); put(e, cls_id);
    for (int i = 0; i < 16; i++) put(e, b[i]);
    for (int i = 0; i < n; i++) put(e, v[i]);
}

static struct enc E;   /* ~2 KB of tables: static, the kernel stack is small */

int jpeg_encode(const unsigned *rgb, int w, int h, int stride, int quality, unsigned char *out, int max)
{
    struct enc *e = &E;
    if (w <= 0 || h <= 0 || w > 65535 || h > 65535) return -1;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int sc = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int i = 0; i < 64; i++) {
        int a = (qlum[i] * sc + 50) / 100, b = (qchr[i] * sc + 50) / 100;
        e->ql[i] = (unsigned char)(a < 1 ? 1 : a > 255 ? 255 : a);
        e->qc[i] = (unsigned char)(b < 1 ? 1 : b > 255 ? 255 : b);
    }
    for (int i = 0; i < 256; i++) { e->dl.len[i] = e->dc.len[i] = e->al.len[i] = e->ac.len[i] = 0; }
    build(&e->dl, dc_lum_bits, dc_vals); build(&e->dc, dc_chr_bits, dc_vals);
    build(&e->al, ac_lum_bits, ac_lum_vals); build(&e->ac, ac_chr_bits, ac_chr_vals);
    e->out = out; e->n = 0; e->max = max; e->over = 0; e->acc = 0; e->nbits = 0;

    put16(e, 0xFFD8);
    static const unsigned char jfif[] = { 0xFF,0xE0,0,16,'J','F','I','F',0,1,1,0,0,1,0,1,0,0 };
    for (unsigned i = 0; i < sizeof(jfif); i++) put(e, jfif[i]);
    put16(e, 0xFFDB); put16(e, 2 + 2 * 65);
    put(e, 0); for (int i = 0; i < 64; i++) put(e, e->ql[zz[i]]);
    put(e, 1); for (int i = 0; i < 64; i++) put(e, e->qc[zz[i]]);
    put16(e, 0xFFC0); put16(e, 17); put(e, 8); put16(e, h); put16(e, w); put(e, 3);
    put(e, 1); put(e, 0x22); put(e, 0);
    put(e, 2); put(e, 0x11); put(e, 1);
    put(e, 3); put(e, 0x11); put(e, 1);
    dht(e, 0x00, dc_lum_bits, dc_vals); dht(e, 0x10, ac_lum_bits, ac_lum_vals);
    dht(e, 0x01, dc_chr_bits, dc_vals); dht(e, 0x11, ac_chr_bits, ac_chr_vals);
    put16(e, 0xFFDA); put16(e, 12); put(e, 3);
    put(e, 1); put(e, 0x00); put(e, 2); put(e, 0x11); put(e, 3); put(e, 0x11);
    put(e, 0); put(e, 63); put(e, 0);

    int py = 0, pb = 0, pr = 0;
    static int Y[4][64], Cb[64], Cr[64];
    for (int my = 0; my < h; my += 16)
        for (int mx = 0; mx < w; mx += 16) {
            int sb[64] = {0}, sr[64] = {0};
            for (int y = 0; y < 16; y++) {
                int yy = my + y < h ? my + y : h - 1;
                const unsigned *row = rgb + (long)yy * stride;
                for (int x = 0; x < 16; x++) {
                    int xx = mx + x < w ? mx + x : w - 1;
                    unsigned p = row[xx];
                    int R = (int)((p >> 16) & 0xFF), G = (int)((p >> 8) & 0xFF), B = (int)(p & 0xFF);
                    Y[(y >> 3) * 2 + (x >> 3)][(y & 7) * 8 + (x & 7)] = ((77 * R + 150 * G + 29 * B + 128) >> 8) - 128;
                    int ci = (y >> 1) * 8 + (x >> 1);
                    sb[ci] += -43 * R - 85 * G + 128 * B;
                    sr[ci] += 128 * R - 107 * G - 21 * B;
                }
            }
            for (int i = 0; i < 64; i++) {             /* 4 pixels, /256 -> /1024 */
                Cb[i] = sb[i] >= 0 ? (sb[i] + 512) >> 10 : -((-sb[i] + 512) >> 10);
                Cr[i] = sr[i] >= 0 ? (sr[i] + 512) >> 10 : -((-sr[i] + 512) >> 10);
            }
            for (int b = 0; b < 4; b++) block(e, Y[b], e->ql, &e->dl, &e->al, &py);
            block(e, Cb, e->qc, &e->dc, &e->ac, &pb);
            block(e, Cr, e->qc, &e->dc, &e->ac, &pr);
            if (e->over) return -2;
        }
    flush_bits(e);
    put16(e, 0xFFD9);
    return e->over ? -2 : e->n;
}
