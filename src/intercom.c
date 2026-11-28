/*
 * intercom.c - 0.148: door intercom over the web (owner: the doorbell should carry the voice too).
 *
 * Hardware (both on the PCM/I2S pins, sharing the bit clock and the frame clock):
 *   INMP441 microphone  SCK -> GPIO18 (pin 12), WS -> GPIO19 (pin 35), SD -> GPIO20 (pin 38), L/R -> GND, VDD 3.3 V
 *   MAX98357A amplifier BCLK -> GPIO18,        LRC -> GPIO19,         DIN -> GPIO21 (pin 40), VIN 5 V, + speaker
 * PCM master at 16 kHz, 64 bit clocks per frame (I2S: 2 x 32-bit slots, data one clock after the frame edge).
 * TX = 16-bit samples in both slots (the amplifier mixes L+R by default), RX = slot 1 (left) 24 bits.
 *
 * Two DMA channels run LOOPS (one control block pointing to itself), so nothing has to feed them in time:
 *   ch 4: PCM FIFO -> mic ring (16000 words = 1 s), ch 5: speaker ring (16000 frames x 2 words) -> PCM FIFO.
 * Core 1 (`intercom_poll1` from io_poll_all, and the web requests, all under the io lock) follows the DMA
 * addresses: it counts the mic samples that arrived, zeroes the speaker frames already played (so the loop
 * plays silence, not old speech) and writes new speech ~0.2 s ahead of the read position.
 * Web: GET /api/door/audio?from=N -> 12-byte header + G.711 u-law samples (the browser polls every 0.2 s),
 * POST /api/door/speak with base64 u-law (push to talk), GET /api/door = state. The hardware starts when the
 * page asks and stops 8 s after the last request (core 0, `intercom_poll`), so I2S is free otherwise.
 * `intercom test on` = no PCM / DMA (QEMU has none): a 440 Hz tone as the mic, the speaker "played" by time.
 */
#include "kernel.h"
#include "sd.h"

#define RATE       16000
#define TXF        RATE                       /* speaker ring frames (1 s) */
#define RXN        RATE                       /* mic ring words (1 s) */

#define PCM_BASE   (MMIO_BASE + 0x203000)
#define PCM_CS     (PCM_BASE + 0x00)
#define PCM_MODE   (PCM_BASE + 0x08)
#define PCM_RXC    (PCM_BASE + 0x0C)
#define PCM_TXC    (PCM_BASE + 0x10)
#define PCM_DREQ   (PCM_BASE + 0x14)
#define PCM_INTEN  (PCM_BASE + 0x18)
#define PCM_FIFO_BUS 0x7E203004u
#define CS_EN      (1u << 0)
#define CS_RXON    (1u << 1)
#define CS_TXON    (1u << 2)
#define CS_TXCLR   (1u << 3)
#define CS_RXCLR   (1u << 4)
#define CS_DMAEN   (1u << 9)
#define CS_STBY    (1u << 25)

#define CM_PCMCTL  (MMIO_BASE + 0x101098)
#define CM_PCMDIV  (MMIO_BASE + 0x10109C)
#define CM_PASSWD  0x5A000000u
#define CM_ENAB    (1u << 4)
#define CM_BUSY    (1u << 7)
#define PLLD_HZ    500000000UL

#define DMA_RXCH   4
#define DMA_TXCH   5                          /* the same channel i2s.c uses - i2s is stopped while we run */
#define DMA_REG(c, o) (MMIO_BASE + 0x7000 + (c) * 0x100 + (o))
#define DMA_ENABLE (MMIO_BASE + 0x7FF0)
#define TI_WAIT_RESP (1u << 3)
#define TI_DEST_INC  (1u << 4)
#define TI_DEST_DREQ (1u << 6)
#define TI_SRC_INC   (1u << 8)
#define TI_SRC_DREQ  (1u << 10)
#define TI_NO_WIDE   (1u << 26)

struct cb { u32 ti, src, dst, len, stride, next, pad[2]; };
extern char __dma_start[];
#define AREA   ((u8 *)__dma_start + 0x1A0000)  /* i2s.c uses +0x180000 .. +0x191400 */
#define CB_TX  ((volatile struct cb *)AREA)
#define CB_RX  ((volatile struct cb *)(AREA + 0x20))
#define TXR    ((volatile u32 *)(AREA + 0x1000))
#define RXR    ((volatile u32 *)(AREA + 0x1000 + TXF * 8))
#define BUS(p) ((u32)(uintptr_t)(p) | 0xC0000000u)

static int enabled, loaded, test, volume = 80, gain = 4;
static volatile int running, owns;
static volatile u64 want_until;
static u64 started_at, test_t0;
static u64 rx_total, tx_rd, tx_zeroed, tx_w;   /* absolute sample / frame counts */
static u32 rx_last, tx_last;
static int mic_peak, spk_peak, mic_peak_hold, spk_peak_hold;
static u64 peak_at;
static u32 listen_reqs, speak_reqs, speak_samples, dropped;
static char last_client[16];
static u32 test_phase;

int intercom_owns_pcm(void) { return owns; }

static void scopy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }

static int isin(u32 ph)                        /* sine, phase 0..65535 -> -32767..32767 (Bhaskara) */
{
    int deg = (int)((ph & 0xFFFF) * 360 / 65536), s = 1;
    if (deg >= 180) { deg -= 180; s = -1; }
    long n = 4L * deg * (180 - deg), d = 40500 - (long)deg * (180 - deg);
    return (int)(s * (32767L * n / d));
}

/* ---------------- G.711 u-law ---------------- */

static u8 ulaw_enc(int s)
{
    int sign = 0;
    if (s < 0) { sign = 0x80; s = -s; }
    if (s > 32635) s = 32635;
    s += 0x84;
    int e = 7;
    for (int m = 0x4000; !(s & m) && e > 0; e--, m >>= 1) ;
    int man = (s >> (e + 3)) & 0x0F;
    return (u8)~(sign | (e << 4) | man);
}
static int ulaw_dec(u8 u)
{
    u = (u8)~u;
    int t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (u & 0x80) ? (0x84 - t) : (t - 0x84);
}

/* ---------------- hardware (core 0, io lock held) ---------------- */

static void clock_set(int hz)
{
    u64 bclk = (u64)hz * 64;
    u32 divi = (u32)(PLLD_HZ / bclk), divf = (u32)(((PLLD_HZ % bclk) * 4096) / bclk);
    mmio_write(CM_PCMCTL, CM_PASSWD | (mmio_read(CM_PCMCTL) & 0xFFFF & ~CM_ENAB));
    u64 end = timer_us() + 10000;
    while ((mmio_read(CM_PCMCTL) & CM_BUSY) && timer_us() < end) ;
    mmio_write(CM_PCMDIV, CM_PASSWD | (divi << 12) | divf);
    mmio_write(CM_PCMCTL, CM_PASSWD | (1u << 9) | 6u);                 /* MASH 1, PLLD */
    mmio_write(CM_PCMCTL, CM_PASSWD | (1u << 9) | 6u | CM_ENAB);
    end = timer_us() + 10000;
    while (!(mmio_read(CM_PCMCTL) & CM_BUSY) && timer_us() < end) ;
}

static void dma_off(int ch)
{
    mmio_write(DMA_REG(ch, 0x00), 1u << 31);   /* reset */
    delay_us(10);
}

static void hw_start(void)
{
    if (i2s_running()) i2s_stop();             /* a tone / WAV on I2S ends; it cannot start again while we own PCM */
    owns = 1;
    for (int i = 0; i < TXF * 2; i++) TXR[i] = 0;
    for (int i = 0; i < RXN; i++) RXR[i] = 0;
    rx_total = tx_rd = tx_zeroed = tx_w = 0;
    rx_last = tx_last = 0;
    mic_peak = spk_peak = 0;
    started_at = timer_us();
    if (test) { test_t0 = started_at; test_phase = 0; running = 1; return; }

    for (int p = 18; p <= 21; p++) gpio_func(p, 4);                    /* ALT0: PCM CLK FS DIN DOUT */
    gpio_pull(20, 1);                                                  /* mic data: no floating input when unplugged */
    clock_set(RATE);
    mmio_write(PCM_CS, 0);
    delay_us(100);
    mmio_write(PCM_CS, CS_EN | CS_STBY);
    mmio_write(PCM_MODE, (63u << 10) | 32u | (1u << 20) | (1u << 22));   /* FLEN 64, FSLEN 32, FSI, CLKI (as i2s.c) */
    mmio_write(PCM_TXC, (1u << 30) | (1u << 20) | (8u << 16)             /* ch1 16 bit at clock 1 */
                      | (1u << 14) | (33u << 4) | 8u);                   /* ch2 16 bit at clock 33 */
    mmio_write(PCM_RXC, (1u << 31) | (1u << 30) | (1u << 20));          /* ch1 24 bit (WEX + width 8) at clock 1 */
    mmio_write(PCM_INTEN, 0);
    mmio_write(PCM_CS, CS_EN | CS_STBY | CS_TXCLR | CS_RXCLR);
    delay_us(100);
    mmio_write(PCM_DREQ, (0x10u << 24) | (0x30u << 16) | (0x30u << 8) | 0x20u);   /* TX panic / RX panic / TX req / RX req */

    mmio_write(DMA_ENABLE, mmio_read(DMA_ENABLE) | (1u << DMA_TXCH) | (1u << DMA_RXCH));
    dma_off(DMA_TXCH); dma_off(DMA_RXCH);
    CB_TX->ti = TI_WAIT_RESP | TI_DEST_DREQ | TI_SRC_INC | (2u << 16) | TI_NO_WIDE;   /* PERMAP 2 = PCM TX */
    CB_TX->src = BUS(TXR); CB_TX->dst = PCM_FIFO_BUS; CB_TX->len = TXF * 8; CB_TX->stride = 0;
    CB_TX->next = BUS(CB_TX);                                          /* a loop */
    CB_RX->ti = TI_WAIT_RESP | TI_SRC_DREQ | TI_DEST_INC | (3u << 16) | TI_NO_WIDE;   /* PERMAP 3 = PCM RX */
    CB_RX->src = PCM_FIFO_BUS; CB_RX->dst = BUS(RXR); CB_RX->len = RXN * 4; CB_RX->stride = 0;
    CB_RX->next = BUS(CB_RX);
    __asm__ volatile("dsb sy");
    for (int ch = DMA_RXCH; ch <= DMA_TXCH; ch++) {
        mmio_write(DMA_REG(ch, 0x00), 3u << 1);                        /* clear END / INT */
        mmio_write(DMA_REG(ch, 0x04), BUS(ch == DMA_TXCH ? CB_TX : CB_RX));
        mmio_write(DMA_REG(ch, 0x00), 1u | (8u << 16) | (8u << 20));    /* ACTIVE, priority 8, panic 8 */
    }
    mmio_write(PCM_CS, CS_EN | CS_STBY | CS_DMAEN | CS_TXON | CS_RXON);
    running = 1;
}

static void hw_stop(void)
{
    running = 0;
    if (!test) {
        dma_off(DMA_TXCH); dma_off(DMA_RXCH);
        mmio_write(PCM_CS, 0);
        mmio_write(CM_PCMCTL, CM_PASSWD | (mmio_read(CM_PCMCTL) & 0xFFFF & ~CM_ENAB));
        for (int p = 18; p <= 21; p++) gpio_func(p, 0);
    }
    owns = 0;
}

/* ---------------- core 1: follow the DMA ---------------- */

static int mic_sample(u32 w)                   /* a 24-bit slot word -> 16 bit with the gain */
{
    int s = (int)(w << 8) >> 16;               /* sign-extend bits 23..0, keep the top 16 */
    s *= gain;
    if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
    return s;
}

void intercom_poll1(void)
{
    if (!running) return;
    u64 now = timer_us();
    u32 ri, ti;
    if (test) {
        u64 el = (now - test_t0) * RATE / 1000000;
        while (rx_total < el) {                 /* the fake mic: 440 Hz at 30 % */
            int s = isin(test_phase) * 3 / 10 / gain;
            test_phase += 65536u * 440 / RATE;
            RXR[rx_total % RXN] = ((u32)(s << 8)) & 0xFFFFFF;
            rx_total++;
        }
        ri = (u32)(rx_total % RXN);
        ti = (u32)(el % TXF);
    } else {
        ri = (mmio_read(DMA_REG(DMA_RXCH, 0x10)) - BUS(RXR)) / 4;      /* DEST_AD */
        ti = (mmio_read(DMA_REG(DMA_TXCH, 0x0C)) - BUS(TXR)) / 8;      /* SOURCE_AD */
        if (ri >= RXN) ri = rx_last;
        if (ti >= TXF) ti = tx_last;
    }
    if (!test) {
        u32 d = (ri + RXN - rx_last) % RXN;
        for (u32 k = 0; k < d; k++) { int s = mic_sample(RXR[(rx_last + k) % RXN]); if (s < 0) s = -s; if (s > mic_peak) mic_peak = s; }
        rx_total += d;
    } else {
        for (u32 k = rx_last; k != ri; k = (k + 1) % RXN) { int s = mic_sample(RXR[k]); if (s < 0) s = -s; if (s > mic_peak) mic_peak = s; }
    }
    rx_last = ri;
    tx_rd += (ti + TXF - tx_last) % TXF;
    tx_last = ti;
    /* frames already played: measure, then silence them */
    u64 z = tx_zeroed;
    if (tx_rd - z > TXF) z = tx_rd - TXF;
    for (; z < tx_rd; z++) {
        u32 f = (u32)(z % TXF);
        int s = (short)TXR[f * 2]; if (s < 0) s = -s; if (s > spk_peak) spk_peak = s;
        TXR[f * 2] = 0; TXR[f * 2 + 1] = 0;
    }
    tx_zeroed = z;
    if (now - peak_at > 500000) {               /* peaks over the last half second */
        mic_peak_hold = mic_peak; spk_peak_hold = spk_peak;
        mic_peak = spk_peak = 0; peak_at = now;
    }
}

static void touch(const char *from)
{
    want_until = timer_us() + 8000000ULL;
    if (from) scopy(last_client, from, sizeof(last_client));
}

/* GET /api/door/audio?from=N (core 1, io lock): header (12 B: next position, count, running, mic %) + u-law */
int intercom_audio(u8 *out, int max, const char *from_s, const char *client)
{
    touch(client);
    listen_reqs++;
    u32 from = 0;
    if (from_s) while (*from_s >= '0' && *from_s <= '9') from = from * 10 + (u32)(*from_s++ - '0');
    u32 total = (u32)rx_total, cnt = 0, start = total;
    if (running) {
        intercom_poll1();
        total = (u32)rx_total;
        start = from;
        if (!from || from > total || total - from > RXN - 1600) start = total > 3200 ? total - 3200 : 0;
        cnt = total - start;
        if (cnt > (u32)(max - 12)) { cnt = (u32)(max - 12); start = total - cnt; }
        for (u32 i = 0; i < cnt; i++) out[12 + i] = ulaw_enc(mic_sample(RXR[(start + i) % RXN]));
    }
    u32 next = start + cnt;
    u32 hdr[3] = { next, cnt, (u32)(running ? 1 : 0) | (u32)(mic_peak_hold * 100 / 32768) << 8 | (u32)(spk_peak_hold * 100 / 32768) << 16 };
    for (int i = 0; i < 12; i++) out[i] = (u8)(hdr[i / 4] >> (8 * (i % 4)));
    return 12 + (int)cnt;
}

static int b64v(char c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
         : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
}

/* POST /api/door/speak, body = base64 u-law at 16 kHz (core 1, io lock); samples taken or -1 not running */
int intercom_speak(const char *b64, int len, const char *client)
{
    touch(client);
    speak_reqs++;
    if (!running) return -1;
    intercom_poll1();
    if (tx_w < tx_rd + 1600) tx_w = tx_rd + 3200;           /* 0.2 s ahead of what is playing */
    u32 acc = 0; int bits = 0, n = 0;
    for (int i = 0; i < len; i++) {
        int v = b64v(b64[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (u32)v; bits += 6;
        if (bits < 8) continue;
        bits -= 8;
        u8 u = (u8)(acc >> bits);
        if (tx_w >= tx_rd + TXF - 1600) { dropped++; continue; }   /* too far ahead: the ring would overrun */
        int s = ulaw_dec(u) * volume / 100;
        u32 f = (u32)(tx_w % TXF);
        TXR[f * 2] = (u16)s; TXR[f * 2 + 1] = (u16)s;
        tx_w++; n++;
    }
    speak_samples += (u32)n;
    return n;
}

/* GET /api/door (core 1): the state as JSON fields (without braces) */
int intercom_json(char *b, int max)
{
    int n = 0;
#define A(s) do { const char *s_ = (s); while (*s_ && n < max - 1) b[n++] = *s_++; } while (0)
#define N(v) do { char t_[12]; int k_ = 0; u32 v_ = (u32)(v); do t_[k_++] = (char)('0' + v_ % 10); while ((v_ /= 10)); while (k_ && n < max - 1) b[n++] = t_[--k_]; } while (0)
    A("\"enabled\":"); A(enabled ? "true" : "false");
    A(",\"running\":"); A(running ? "true" : "false");
    A(",\"test\":"); A(test ? "true" : "false");
    A(",\"rate\":"); N(RATE);
    A(",\"mic\":"); N(mic_peak_hold * 100 / 32768);
    A(",\"speaker\":"); N(spk_peak_hold * 100 / 32768);
    A(",\"volume\":"); N(volume);
    A(",\"gain\":"); N(gain);
    b[n] = 0;
    return n;
#undef A
#undef N
}

int intercom_enabled(void) { return enabled; }

/* ---------------- core 0 ---------------- */

static u64 poll_at, beep_until;

void intercom_poll(void)
{
    u64 now = timer_us();
    if (now < poll_at) return;
    poll_at = now + 20000;
    if (io_held()) return;
    int want = enabled && now < want_until;
    if (want && !running) {
        io_lock(); hw_start(); io_unlock();
        con_color(COL_CYAN); kprintf("\n[DOOR] intercom on (%s)%s\n", last_client[0] ? last_client : "here", test ? " - test mode" : ""); con_color(COL_DEFAULT);
    } else if (!want && running && now > beep_until) {
        io_lock(); hw_stop(); io_unlock();
        con_color(COL_CYAN); kprintf("\n[DOOR] intercom off\n"); con_color(COL_DEFAULT);
    }
}

void intercom_prepare(void)
{
    loaded = 1;
    struct fat_file f;
    char b[200];
    if (!fat_mounted() || fat_open("/INTERCOM.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    for (char *l = b; l && *l; ) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        int k = (int)strlen(l);
        while (k && (l[k - 1] == '\r' || l[k - 1] == ' ')) l[--k] = 0;
        char *v = strchr(l, ' ');
        if (v) {
            *v++ = 0; u64 num;
            if (!strcmp(l, "state")) enabled = !strcmp(v, "on");
            else if (!strcmp(l, "volume") && parse_num(v, &num) && num <= 100) volume = (int)num;
            else if (!strcmp(l, "gain") && parse_num(v, &num) && num >= 1 && num <= 64) gain = (int)num;
        }
        l = e ? e + 1 : 0;
    }
}

static void save(void)
{
    char b[80];
    int n = 0;
    const char *s = enabled ? "state on\r\nvolume " : "state off\r\nvolume ";
    while (*s) b[n++] = *s++;
    { char t[8]; int k = 0, v = volume; do t[k++] = (char)('0' + v % 10); while (v /= 10); while (k) b[n++] = t[--k]; }
    s = "\r\ngain "; while (*s) b[n++] = *s++;
    { char t[8]; int k = 0, v = gain; do t[k++] = (char)('0' + v % 10); while (v /= 10); while (k) b[n++] = t[--k]; }
    b[n++] = '\r'; b[n++] = '\n';
    if (!fat_mounted() || fat_write_file("/INTERCOM.TXT", b, (u32)n)) kprintf("(not saved: no SD card or write error)\n");
    else kprintf("(saved in /INTERCOM.TXT)\n");
}

static void bar(int pct)
{
    if (pct > 100) pct = 100;
    kprintf("[");
    for (int i = 0; i < 40; i++) kprintf(i < pct * 40 / 100 ? "#" : " ");
    kprintf("] %3d %%", pct);
}

void intercom_cmd(int argc, char **argv)
{
    if (!loaded) intercom_prepare();
    int changed = 0;
    if (argc > 1 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) { enabled = argv[1][1] == 'n'; changed = 1; if (!enabled) want_until = 0; }
    else if (argc > 2 && !strcmp(argv[1], "volume")) { u64 v; if (parse_num(argv[2], &v) && v <= 100) { volume = (int)v; changed = 1; } }
    else if (argc > 2 && !strcmp(argv[1], "gain")) { u64 v; if (parse_num(argv[2], &v) && v >= 1 && v <= 64) { gain = (int)v; changed = 1; } }
    else if (argc > 2 && !strcmp(argv[1], "test")) {
        int t = !strcmp(argv[2], "on");
        if (running) { io_lock(); hw_stop(); io_unlock(); }
        test = t;
        kprintf("test mode %s (no I2S hardware used)\n", test ? "ON" : "off");
        return;
    } else if (argc > 1 && (!strcmp(argv[1], "beep") || !strcmp(argv[1], "mic"))) {
        if (!enabled) { kprintf("first 'intercom on'\n"); return; }
        touch(0);
        intercom_poll(); poll_at = 0; intercom_poll();
        if (!running) { kprintf("the intercom did not start\n"); return; }
        if (argv[1][0] == 'b') {                       /* 1 s of 1 kHz into the speaker ring */
            io_lock();
            intercom_poll1();
            u64 w = tx_rd + 1600; u32 ph = 0;
            for (int i = 0; i < RATE; i++, w++) { int s = isin(ph) / 3 * volume / 100; ph += 65536u * 1000 / RATE; u32 f = (u32)(w % TXF); TXR[f * 2] = (u16)s; TXR[f * 2 + 1] = (u16)s; }
            if (tx_w < w) tx_w = w;
            io_unlock();
            beep_until = timer_us() + 1500000;
            kprintf("1 s beep at 1 kHz on the speaker (MAX98357A on GPIO18/19/21)\n");
            return;
        }
        kprintf("microphone level (INMP441 on GPIO18/19/20), any key stops:\n");
        u64 end = timer_us() + 30000000ULL;
        while (timer_us() < end) {
            touch(0);
            if (con_break() || con_trygetc() >= 0) break;
            kprintf("\r  mic "); bar(mic_peak_hold * 100 / 32768);
            u64 t = timer_us() + 200000;
            while (timer_us() < t) { intercom_poll(); }
        }
        kprintf("\n");
        return;
    } else if (argc > 1) {
        kprintf("usage: intercom [on|off | volume 0-100 | gain 1-64 | beep | mic | test on|off]\n");
        return;
    }
    if (changed) save();
    kprintf("intercom (doorbell voice, web page /door): %s%s\n", enabled ? "ON" : "off ('intercom on')", test ? " - TEST MODE (no hardware)" : "");
    kprintf("  microphone INMP441: SCK GPIO18 (pin 12), WS GPIO19 (pin 35), SD GPIO20 (pin 38), L/R to GND, VDD 3.3 V\n");
    kprintf("  amplifier MAX98357A: BCLK GPIO18 (pin 12), LRC GPIO19 (pin 35), DIN GPIO21 (pin 40), VIN 5 V\n");
    kprintf("  speaker volume %d %%, microphone gain x%d, %d Hz u-law\n", volume, gain, RATE);
    if (running) {
        kprintf("  RUNNING %u s for %s: mic ", (u32)((timer_us() - started_at) / 1000000), last_client[0] ? last_client : "the shell");
        bar(mic_peak_hold * 100 / 32768);
        kprintf(", speaker %d %%\n", spk_peak_hold * 100 / 32768);
    } else kprintf("  idle - the I2S pins are used only while the page /door listens\n");
    kprintf("  web: %u listen requests, %u talk requests (%u samples, %u dropped)\n", listen_reqs, speak_reqs, speak_samples, dropped);
    for (int p = 18; p <= 21; p++)
        if (access_uses_pin(p)) { con_color(COL_YELLOW); kprintf("  !! GPIO%d is used by the alarm keypad / knob - they cannot share it\n", p); con_color(COL_DEFAULT); }
    if (display_hat()) { con_color(COL_YELLOW); kprintf("  !! the Waveshare OLED HAT uses GPIO19-21 for its keys\n"); con_color(COL_DEFAULT); }
}
