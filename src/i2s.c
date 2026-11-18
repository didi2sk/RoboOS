/*
 * I2S output to a DAC module (PCM5102A, MAX98357A, UDA1334, ...).
 *
 *   GPIO18 = BCLK, GPIO19 = LRCLK (word select), GPIO21 = DIN of the DAC (ALT0)
 *   + 3.3 V (or 5 V, see the module) and GND. The DAC's SCK / system clock pin
 *   goes to GND (it makes its own clock from BCLK); FMT low = I2S.
 *
 * The PCM peripheral is the master: 64 bit clocks per frame, two 16-bit
 * channels in I2S format (one clock data delay, LRCLK low = left).
 * Samples reach the PCM FIFO by DMA (channel 5, paced by the PCM's DREQ) from
 * a ring of small segments in the uncached DMA block. The ring is a chain of
 * control blocks that ends at the last filled segment: if nobody refills it in
 * time the DMA simply stops (silence) instead of replaying old sound, and the
 * next poll starts it again. Tones are generated (sine), WAV files streamed.
 *
 * The same DMA ring also feeds the PWM for WAV files on the 3.5 mm jack (or
 * GPIO18/19): PWM clock 250 MHz (PLLD / 2), range = 250 MHz / sample rate
 * (5669 steps = ~12.5 bit at 44.1 kHz), both channels from the PWM FIFO, which
 * takes left and right words in turn (as Circle's PWM sound device).
 */
#include "kernel.h"
#include "sd.h"

#define PCM_BASE   (MMIO_BASE + 0x203000)
#define PCM_CS     (PCM_BASE + 0x00)
#define PCM_FIFO   (PCM_BASE + 0x04)
#define PCM_MODE   (PCM_BASE + 0x08)
#define PCM_RXC    (PCM_BASE + 0x0C)
#define PCM_TXC    (PCM_BASE + 0x10)
#define PCM_DREQ   (PCM_BASE + 0x14)
#define PCM_INTEN  (PCM_BASE + 0x18)
#define PCM_FIFO_BUS 0x7E203004u

#define CS_EN      (1 << 0)
#define CS_TXON    (1 << 2)
#define CS_TXCLR   (1 << 3)
#define CS_RXCLR   (1 << 4)
#define CS_DMAEN   (1 << 9)
#define CS_TXERR   (1 << 15)
#define CS_STBY    (1 << 25)

#define CM_PCMCTL  (MMIO_BASE + 0x101098)
#define CM_PCMDIV  (MMIO_BASE + 0x10109C)
#define CM_PASSWD  0x5A000000
#define CM_ENAB    (1 << 4)
#define CM_BUSY    (1 << 7)
#define CM_SRC_PLLD 6                         /* 500 MHz */
#define CM_MASH1   (1 << 9)
#define PLLD_HZ    500000000UL

#define DMA_CH     5
#define DMA_REG(o) (MMIO_BASE + 0x7000 + DMA_CH * 0x100 + (o))
#define DMA_CS     DMA_REG(0x00)
#define DMA_CONBLK DMA_REG(0x04)
#define DMA_SRC    DMA_REG(0x0C)
#define DMA_DEBUG  DMA_REG(0x20)
#define DMA_ENABLE (MMIO_BASE + 0x7FF0)
#define DCS_ACTIVE (1u << 0)
#define DCS_END    (1u << 1)
#define DCS_ERROR  (1u << 8)
#define DCS_RESET  (1u << 31)
#define TI_WAIT_RESP (1u << 3)
#define TI_DEST_DREQ (1u << 6)
#define TI_SRC_INC   (1u << 8)
#define TI_PERMAP_PCM_TX (2u << 16)
#define TI_NO_WIDE   (1u << 26)

/* PWM as a DMA sink */
#define PWM_BASE   (MMIO_BASE + 0x20C000)
#define PWM_CTL    (PWM_BASE + 0x00)
#define PWM_DMAC   (PWM_BASE + 0x08)
#define PWM_RNG1   (PWM_BASE + 0x10)
#define PWM_RNG2   (PWM_BASE + 0x20)
#define PWM_FIFO_BUS 0x7E20C018u
#define CM_PWMCTL  (MMIO_BASE + 0x1010A0)
#define CM_PWMDIV  (MMIO_BASE + 0x1010A4)
#define TI_PERMAP_PWM (5u << 16)
#define PWM_HZ     250000000UL                 /* PLLD / 2 */

#define NSEG       32
#define SEG_FRAMES 256                         /* 5.8 ms at 44.1 kHz */
#define SEG_WORDS  (SEG_FRAMES * 2)            /* one 32-bit word per channel */
#define SEG_BYTES  (SEG_WORDS * 4)
#define AHEAD_TONE 8                           /* ~46 ms queued for tones */
#define AHEAD_WAV  (NSEG - 2)

struct cb { u32 ti, src, dst, len, stride, next, pad[2]; };

extern char __dma_start[];
#define AREA      ((u8 *)__dma_start + 0x180000)
#define CBS       ((volatile struct cb *)AREA)
#define SEG(i)    ((volatile u32 *)(AREA + 0x400 + (i) * SEG_BYTES))
#define BUS(p)    ((u32)(uintptr_t)(p) | 0xC0000000u)

static int running, rate = 44100, volume = 50, stream_on, st_n;
enum { SINK_I2S = 1, SINK_PWM = 2 };
static int sink, pwm_range;
static int q_start, q_len;                     /* queued segments, oldest first */
static u32 restarts, segments, dma_errors;

static u32 phase, step;                        /* tone generator (sine) */
static int gen_on;
static long gen_left;                          /* samples still to make, -1 = endless */

static struct fat_file wf;                     /* WAV being played */
static int wav_on, wav_ch, wav_bits;
static u32 wav_left;

/* ---------------- sine table (Bhaskara approximation, no libm) ---------------- */
static short sine[256];
static void make_sine(void)
{
    for (int i = 0; i < 256; i++) {
        int deg = i * 360 / 256, s = 1;
        if (deg >= 180) { deg -= 180; s = -1; }
        long n = 4L * deg * (180 - deg), d = 40500 - (long)deg * (180 - deg);
        sine[i] = (short)(s * (32767L * n / d));
    }
}

/* ---------------- clock, PCM, DMA ---------------- */

static void clock_set(int hz)
{
    u64 bclk = (u64)hz * 64;                   /* 64 bit clocks per frame */
    u32 divi = (u32)(PLLD_HZ / bclk);
    u32 divf = (u32)(((PLLD_HZ % bclk) * 4096) / bclk);
    u64 end;
    mmio_write(CM_PCMCTL, CM_PASSWD | (mmio_read(CM_PCMCTL) & 0xFFFF & ~CM_ENAB));
    end = timer_us() + 10000;
    while ((mmio_read(CM_PCMCTL) & CM_BUSY) && timer_us() < end) ;
    mmio_write(CM_PCMDIV, CM_PASSWD | (divi << 12) | divf);
    mmio_write(CM_PCMCTL, CM_PASSWD | CM_MASH1 | CM_SRC_PLLD);
    mmio_write(CM_PCMCTL, CM_PASSWD | CM_MASH1 | CM_SRC_PLLD | CM_ENAB);
    end = timer_us() + 10000;
    while (!(mmio_read(CM_PCMCTL) & CM_BUSY) && timer_us() < end) ;
    rate = hz;
}

static void set_rate(int hz)
{
    if (sink == SINK_PWM) {                    /* the clock stays, the range changes */
        pwm_range = (int)(PWM_HZ / (u32)hz);
        mmio_write(PWM_RNG1, (u32)pwm_range);
        mmio_write(PWM_RNG2, (u32)pwm_range);
        rate = hz;
    } else {
        clock_set(hz);
    }
}

static void dma_stop(void)
{
    mmio_write(DMA_CS, DCS_RESET);
    delay_us(10);
    q_len = 0;
}

static void dma_start_at(int i)
{
    mmio_write(DMA_CS, DCS_END | DCS_ERROR);   /* clear flags */
    mmio_write(DMA_CONBLK, BUS(&CBS[i]));
    mmio_write(DMA_CS, DCS_ACTIVE | (8u << 16) | (8u << 20));
}

int i2s_running(void) { return running; }

static void ring_init(void);

int i2s_start(void)
{
    if (running) return 0;
    if (!sine[64]) make_sine();
    sink = SINK_I2S;
    for (int p = 18; p <= 21; p++) gpio_func(p, 4);          /* ALT0: PCM CLK, FS, DIN, DOUT */
    clock_set(44100);

    mmio_write(PCM_CS, 0);
    delay_us(100);
    mmio_write(PCM_CS, CS_EN | CS_STBY);
    mmio_write(PCM_MODE, (63u << 10) | 32u | (1u << 20) | (1u << 22));   /* FLEN 64, FSLEN 32, FSI, CLKI */
    mmio_write(PCM_TXC, (1u << 30) | (1u << 20) | (8u << 16)             /* ch1: 16 bit at clock 1 */
                      | (1u << 14) | (33u << 4) | 8u);                   /* ch2: 16 bit at clock 33 */
    mmio_write(PCM_RXC, 0);
    mmio_write(PCM_INTEN, 0);
    mmio_write(PCM_CS, CS_EN | CS_STBY | CS_TXCLR | CS_RXCLR);
    delay_us(100);                                                       /* > 2 PCM clocks */
    mmio_write(PCM_DREQ, (0x10u << 24) | (0x30u << 8));                  /* TX panic, TX request */

    ring_init();
    mmio_write(PCM_CS, CS_EN | CS_STBY | CS_DMAEN | CS_TXON);
    running = 1;
    return 0;
}

/* the chain of control blocks for the current sink */
static void ring_init(void)
{
    mmio_write(DMA_ENABLE, mmio_read(DMA_ENABLE) | (1u << DMA_CH));
    dma_stop();
    for (int i = 0; i < NSEG; i++) {
        CBS[i].ti = TI_WAIT_RESP | TI_DEST_DREQ | TI_SRC_INC | TI_PERMAP_PCM_TX | TI_NO_WIDE;
        CBS[i].src = BUS(SEG(i));
        CBS[i].dst = PCM_FIFO_BUS;
        if (sink == SINK_PWM) { CBS[i].ti = (CBS[i].ti & ~(0x1Fu << 16)) | TI_PERMAP_PWM; CBS[i].dst = PWM_FIFO_BUS; }
        CBS[i].len = SEG_BYTES;
        CBS[i].stride = 0;
        CBS[i].next = 0;
    }
    q_start = 0; q_len = 0;
}

void i2s_stop(void)
{
    if (!running) return;
    dma_stop();
    mmio_write(PCM_CS, 0);
    mmio_write(CM_PCMCTL, CM_PASSWD | (mmio_read(CM_PCMCTL) & 0xFFFF & ~CM_ENAB));
    for (int p = 18; p <= 21; p++) gpio_func(p, 0);
    running = 0;
    gen_on = 0;
    wav_on = 0;
    sink = 0;
}

/* ---------------- filling ---------------- */

static int wav_read(u8 *b, u32 n)
{
    if (n > wav_left) n = wav_left;
    int r = fat_read(&wf, b, n);
    if (r <= 0) { wav_left = 0; return 0; }
    wav_left -= (u32)r;
    return r;
}

/* one segment: WAV data, a tone or silence; returns 0 when there is nothing to play */
/* one frame into the segment: I2S takes signed 16-bit words, the PWM 0..range */
static inline void put(volatile u32 *w, int i, int l, int r)
{
    if (sink == SINK_PWM) {
        w[i * 2] = (u32)(((l + 32768) * pwm_range) >> 16);
        w[i * 2 + 1] = (u32)(((r + 32768) * pwm_range) >> 16);
    } else {
        w[i * 2] = (u16)l;
        w[i * 2 + 1] = (u16)r;
    }
}

static int fill(volatile u32 *w)
{
    int any = 0;
    if (wav_on) {
        static u8 b[SEG_FRAMES * 4];
        int bpf = wav_ch * (wav_bits / 8);                 /* bytes per frame */
        int got = wav_read(b, (u32)(SEG_FRAMES * bpf)) / bpf;
        for (int i = 0; i < SEG_FRAMES; i++) {
            int l = 0, r = 0;
            if (i < got) {
                const u8 *p = b + i * bpf;
                if (wav_bits == 16) {
                    l = (short)(p[0] | (p[1] << 8));
                    r = wav_ch == 2 ? (short)(p[2] | (p[3] << 8)) : l;
                } else {
                    l = (p[0] - 128) << 8;
                    r = wav_ch == 2 ? (p[1] - 128) << 8 : l;
                }
                l = l * volume / 100; r = r * volume / 100;
            }
            put(w, i, l, r);
        }
        if (got) any = 1;
        if (got < SEG_FRAMES) wav_on = 0;                  /* end of the data */
        return any;
    }
    int amp = 32767 * volume / 100, was = gen_on;
    for (int i = 0; i < SEG_FRAMES; i++) {
        int v = 0;
        if (gen_on) {
            v = sine[phase >> 24] * amp / 32767;
            phase += step;
            if (gen_left > 0 && !--gen_left) gen_on = 0;   /* exact length, counted in samples */
        }
        put(w, i, v, v);
    }
    return was;
}

static int seg_of_bus(u32 a)
{
    u32 base = BUS(SEG(0));
    if (a < base) return -1;
    return (int)((a - base - 1) / SEG_BYTES);
}

/* bookkeeping + refill; wav = may read the WAV file now */
static void pump(int wav)
{
    if (!running) return;
    u32 cs = mmio_read(DMA_CS);
    if (cs & DCS_ERROR) dma_errors++;
    if (q_len) {
        if (cs & DCS_ACTIVE) {                             /* segments before the current one are done */
            u32 cb = mmio_read(DMA_CONBLK);
            int cur = (int)((cb - BUS(&CBS[0])) / sizeof(struct cb));
            if (cur >= 0 && cur < NSEG) {
                int done = (cur - q_start + NSEG) % NSEG;
                if (done > q_len) done = q_len;
                q_start = cur; q_len -= done; segments += done;
            }
        } else {                                           /* stopped: where? */
            int last = seg_of_bus(mmio_read(DMA_SRC));
            int done = last < 0 ? q_len : (last - q_start + NSEG) % NSEG + 1;
            if (done > q_len) done = q_len;
            q_start = (q_start + done) % NSEG; q_len -= done; segments += done;
            if (q_len) { restarts++; dma_start_at(q_start); cs = mmio_read(DMA_CS); }
        }
    }
    if ((wav_on && !wav) || stream_on) return;            /* the WAV loop / the stream fills by itself */
    int target = wav_on ? AHEAD_WAV : AHEAD_TONE;
    int added = 0;
    while (q_len < target) {
        if (!wav_on && !gen_on) break;                     /* silence: let the DMA run dry */
        int i = (q_start + q_len) % NSEG;
        if (!fill(SEG(i)) && !q_len) break;
        CBS[i].next = 0;
        __asm__ volatile("dsb sy" ::: "memory");
        if (q_len) CBS[(i + NSEG - 1) % NSEG].next = BUS(&CBS[i]);   /* append to the chain */
        q_len++;
        added = 1;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (added && !(mmio_read(DMA_CS) & DCS_ACTIVE)) dma_start_at(q_start);
}

void i2s_poll(void) { pump(0); }

void i2s_tone(int freq, int ms)
{
    if (!running && i2s_start()) return;
    if (freq <= 0) { gen_on = 0; return; }
    step = (u32)(((u64)freq << 32) / (u32)rate);
    gen_left = ms > 0 ? (long)ms * rate / 1000 : -1;
    gen_on = 1;
    pump(0);
}

void i2s_volume(int v) { volume = v; }

/* ---------------- WAV files ---------------- */

static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

const char *i2s_wav_error(int e)
{
    switch (e) {
    case -1: return "file not found";
    case -2: return "not a WAV file";
    case -3: return "only PCM WAV with 8 or 16 bits, mono or stereo, 8000-48000 Hz";
    case -4: return "the I2S output is not on ('sound out i2s')";
    case -5: return "no sound output for WAV on this board";
    default: return "error";
    }
}

/* play a WAV file to the end on the current sink; 0 = ok, 1 = stopped, < 0 = error */
static int wav_play(const char *path, int (*idle)(void))
{
    u8 h[12], c[8], fmt[16];
    int have_fmt = 0;
    if (fat_open(path, &wf) || wf.is_dir) return -1;
    if (fat_read(&wf, h, 12) != 12 || memcmp_n(h, "RIFF", 4) || memcmp_n(h + 8, "WAVE", 4)) return -2;
    for (;;) {                                             /* walk the chunks up to "data" */
        if (fat_read(&wf, c, 8) != 8) return -2;
        u32 len = rd32(c + 4);
        if (!memcmp_n(c, "fmt ", 4)) {
            if (len < 16 || fat_read(&wf, fmt, 16) != 16) return -2;
            for (u32 k = 16; k < len; k++) fat_read(&wf, c, 1);
            have_fmt = 1;
        } else if (!memcmp_n(c, "data", 4)) {
            wav_left = len;
            break;
        } else {
            for (u32 k = 0; k < len + (len & 1); k += 8) fat_read(&wf, c, len + (len & 1) - k < 8 ? len + (len & 1) - k : 8);
        }
    }
    if (!have_fmt) return -2;
    int format = fmt[0] | (fmt[1] << 8), hz = (int)rd32(fmt + 4);
    wav_ch = fmt[2] | (fmt[3] << 8);
    wav_bits = fmt[14] | (fmt[15] << 8);
    if (format != 1 || (wav_ch != 1 && wav_ch != 2) || (wav_bits != 8 && wav_bits != 16) || hz < 8000 || hz > 48000)
        return -3;

    gen_on = 0;
    while (q_len) { pump(0); if (idle && idle()) return 1; }   /* let earlier sound finish */
    if (hz != rate) set_rate(hz);
    wav_on = 1;
    int stopped = 0;
    while (wav_on || q_len) {
        pump(1);
        if (idle && idle()) { stopped = 1; break; }
    }
    wav_on = 0;
    if (stopped) { dma_stop(); mmio_write(PCM_CS, mmio_read(PCM_CS) | CS_TXCLR); }
    if (rate != 44100 && sink == SINK_I2S) { while (q_len) pump(0); set_rate(44100); }
    return stopped;
}

int i2s_wav(const char *path, int (*idle)(void))
{
    if (!running || sink != SINK_I2S) return -4;
    return wav_play(path, idle);
}

/* WAV on the PWM (jack or GPIO18/19 - sound.c has set the pins); the PWM is
 * left switched off and its clock changed, the tone code sets them up again */
/* the PWM as the DMA sink (the pins are set by sound.c) */
static void pwm_open(int hz)
{
    if (running) i2s_stop();
    if (!sine[64]) make_sine();
    sink = SINK_PWM;
    mmio_write(PWM_CTL, 0);
    mmio_write(PWM_DMAC, 0);
    /* clock: PLLD 500 MHz / 2 */
    mmio_write(CM_PWMCTL, CM_PASSWD | (mmio_read(CM_PWMCTL) & 0xFFFF & ~CM_ENAB));
    u64 end = timer_us() + 10000;
    while ((mmio_read(CM_PWMCTL) & CM_BUSY) && timer_us() < end) ;
    mmio_write(CM_PWMDIV, CM_PASSWD | (2u << 12));
    mmio_write(CM_PWMCTL, CM_PASSWD | CM_SRC_PLLD);
    mmio_write(CM_PWMCTL, CM_PASSWD | CM_SRC_PLLD | CM_ENAB);
    end = timer_us() + 10000;
    while (!(mmio_read(CM_PWMCTL) & CM_BUSY) && timer_us() < end) ;
    set_rate(hz);
    mmio_write(PWM_CTL, 1u << 6);                                   /* clear the FIFO */
    delay_us(10);
    ring_init();
    mmio_write(PWM_DMAC, (1u << 31) | (7u << 8) | 7u);             /* DMA on, panic 7, DREQ 7 */
    mmio_write(PWM_CTL, (1u << 0) | (1u << 5) | (1u << 8) | (1u << 13));   /* PWEN + USEF, both */
    running = 1;
}

static void pwm_close(void)
{
    dma_stop();
    mmio_write(PWM_CTL, 0);
    mmio_write(PWM_DMAC, 0);
    running = 0;
    sink = 0;
    wav_on = 0;
    rate = 44100;
}

int pwm_wav(const char *path, int (*idle)(void))
{
    struct fat_file probe;
    if (fat_open(path, &probe) || probe.is_dir) return -1;
    pwm_open(44100);
    int r = wav_play(path, idle);
    pwm_close();
    return r;
}

/* ---------------- a sound stream (video) ---------------- */

/* start: on the I2S DAC if it is the output, else on the PWM (sound.c has set the pins) */
int audio_open(int hz, int on_i2s)
{
    if (hz < 8000 || hz > 48000) return -3;
    if (on_i2s) {
        if (!running && i2s_start()) return -4;
        gen_on = 0;
        while (q_len) pump(0);
        if (hz != rate) set_rate(hz);
    } else {
        pwm_open(hz);
    }
    stream_on = 1;
    st_n = 0;
    return 0;
}

static void commit(int i)
{
    CBS[i].next = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    if (q_len) CBS[(i + NSEG - 1) % NSEG].next = BUS(&CBS[i]);
    q_len++;
    __asm__ volatile("dsb sy" ::: "memory");
    if (!(mmio_read(DMA_CS) & DCS_ACTIVE)) dma_start_at(q_start);
}

/* stereo 16-bit frames into the ring; waits while it is full */
void audio_write(const short *lr, int frames)
{
    while (frames > 0 && stream_on) {
        pump(0);
        if (q_len >= NSEG - 1) continue;                   /* full: the DMA drains it */
        int i = (q_start + q_len) % NSEG;
        volatile u32 *w = SEG(i);
        while (frames > 0 && st_n < SEG_FRAMES) {
            put(w, st_n++, lr[0] * volume / 100, lr[1] * volume / 100);
            lr += 2;
            frames--;
        }
        if (st_n == SEG_FRAMES) { commit(i); st_n = 0; }
    }
}

/* how much is queued, in ms (for keeping the picture in step with the sound) */
int audio_queued_ms(void)
{
    pump(0);
    return (int)((long)(q_len * SEG_FRAMES + st_n) * 1000 / rate);
}

/* play what is left and stop; now = drop what is queued */
void audio_close(int now)
{
    if (!stream_on) return;
    if (!now && st_n) {                                    /* pad the last piece with silence */
        int i = (q_start + q_len) % NSEG;
        for (; st_n < SEG_FRAMES; st_n++) put(SEG(i), st_n, 0, 0);
        commit(i);
    }
    if (!now) { u64 end = timer_us() + 1000000; while (q_len && timer_us() < end) pump(0); }
    stream_on = 0;
    st_n = 0;
    if (sink == SINK_PWM) pwm_close();
    else {
        if (now) { dma_stop(); mmio_write(PCM_CS, mmio_read(PCM_CS) | CS_TXCLR); }
        if (rate != 44100) set_rate(44100);
    }
}

u32 i2s_segments(void) { return segments; }

/* for 'sound test': the DMA channel and the PWM status, on the screen */
void audio_diag(void)
{
    kprintf("  DMA%d CS=%x CONBLK=%x DEBUG=%x ENABLE=%x  segments %u, restarts %u, DMA errors %u\n", DMA_CH,
            mmio_read(DMA_CS), mmio_read(DMA_CONBLK), mmio_read(DMA_DEBUG), mmio_read(DMA_ENABLE),
            segments, restarts, dma_errors);
    kprintf("  PWM CTL=%x STA=%x DMAC=%x RNG1=%u  CM_PWMCTL=%x CM_PWMDIV=%x\n",
            mmio_read(PWM_CTL), mmio_read(PWM_BASE + 0x04), mmio_read(PWM_DMAC), mmio_read(PWM_BASE + 0x10),
            mmio_read(CM_PWMCTL), mmio_read(CM_PWMDIV));
}

void i2s_status(void)
{
    u32 cs = mmio_read(PCM_CS);
    kprintf("dma:    %s, %d Hz%s, %u segments played, %u restarts, %u DMA errors%s\n",
            running ? (sink == SINK_PWM ? "PWM" : "I2S") : "off", rate,
            sink == SINK_PWM ? " (PWM range)" : "", segments, restarts, dma_errors,
            running && (cs & CS_TXERR) ? ", FIFO underrun seen" : "");
}
