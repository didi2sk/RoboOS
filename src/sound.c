/*
 * Sound: square-wave tones from the PWM peripheral.
 *
 * The Pi has no sound chip: the 3.5 mm jack is driven by the two PWM channels
 * (GPIO40 / GPIO41 on the Pi 3, ALT0) through an RC filter. For a tone the PWM
 * runs in mark-space mode with range = clock / frequency, so the hardware makes
 * the wave by itself and the CPU is free. The volume is the pulse width (50 % =
 * loudest). Boards without a jack (Zero 2 W, CM3) use GPIO18 / GPIO19 (ALT5),
 * where a piezo buzzer or a small amplifier can be connected.
 *
 * With "sound out i2s" the same tones (as sine waves, with a real volume) and
 * WAV files go to an I2S DAC module instead (i2s.c). The choice is kept in
 * /SOUND.TXT.
 *
 * PLAY understands music strings like QBasic: "T120 O4 L4 C D E8 F#8 G2 P4 > C".
 */
#include "kernel.h"
#include "sd.h"

#define PWM_BASE    (MMIO_BASE + 0x20C000)
#define PWM_CTL     (PWM_BASE + 0x00)
#define PWM_RNG1    (PWM_BASE + 0x10)
#define PWM_DAT1    (PWM_BASE + 0x14)
#define PWM_RNG2    (PWM_BASE + 0x20)
#define PWM_DAT2    (PWM_BASE + 0x24)
#define CTL_PWEN1   (1 << 0)
#define CTL_MSEN1   (1 << 7)
#define CTL_PWEN2   (1 << 8)
#define CTL_MSEN2   (1 << 15)

#define CM_PWMCTL   (MMIO_BASE + 0x1010A0)
#define CM_PWMDIV   (MMIO_BASE + 0x1010A4)
#define CM_PASSWD   0x5A000000
#define CM_ENAB     (1 << 4)
#define CM_BUSY     (1 << 7)
#define CM_SRC_OSC  1                    /* 19.2 MHz crystal */
#define PWM_CLOCK   9600000              /* 19.2 MHz / 2 */

static int clock_on, out_gpio = -1, volume = 50, playing, freq_now, cfg_read;
enum { OUT_JACK, OUT_GPIO, OUT_I2S };
static int pin_a, pin_b, pin_fn;
static u64 stop_at;

/* output pins: the jack where there is one, else GPIO18/19 */
static void read_config(void)
{
    struct fat_file f;
    char b[8];
    if (cfg_read || !fat_mounted()) return;
    cfg_read = 1;
    if (fat_open("/SOUND.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, 4);
    if (n >= 3 && b[0] == 'i' && b[1] == '2' && b[2] == 's') out_gpio = OUT_I2S;
    else if (n >= 4 && b[0] == 'g' && b[1] == 'p' && b[2] == 'i' && b[3] == 'o') out_gpio = OUT_GPIO;
    else if (n >= 4 && b[0] == 'j') out_gpio = OUT_JACK;
}

int sound_output_mode(void) { read_config(); return out_gpio < 0 ? OUT_JACK : out_gpio; }

static void pick_pins(void)
{
    int t = board_type();
    read_config();
    if (out_gpio < 0) out_gpio = (t == 0x12 || t == 0x0A || t == 0x10);   /* Zero 2 W, CM3, CM3+ */
    if (out_gpio == OUT_I2S) { pin_a = 0; return; }                  /* pins belong to i2s.c */
    if (out_gpio)          { pin_a = 18; pin_b = 19; pin_fn = 2; }   /* ALT5 */
    else if (t == 0x04)    { pin_a = 40; pin_b = 45; pin_fn = 4; }   /* Pi 2: ALT0 */
    else                   { pin_a = 40; pin_b = 41; pin_fn = 4; }   /* Pi 3: ALT0 */
}

static void wait_busy(int busy)
{
    u64 end = timer_us() + 10000;
    while (((mmio_read(CM_PWMCTL) & CM_BUSY) != 0) != busy && timer_us() < end) ;
}

static void start_clock(void)
{
    if (clock_on) return;
    mmio_write(CM_PWMCTL, CM_PASSWD | (mmio_read(CM_PWMCTL) & ~CM_ENAB & 0xFFFFFF));
    wait_busy(0);
    mmio_write(CM_PWMDIV, CM_PASSWD | (2 << 12));                  /* integer divider 2 */
    mmio_write(CM_PWMCTL, CM_PASSWD | CM_SRC_OSC);
    mmio_write(CM_PWMCTL, CM_PASSWD | CM_SRC_OSC | CM_ENAB);
    wait_busy(1);
    clock_on = 1;
}

void sound_stop(void)
{
    if (i2s_running()) i2s_tone(0, 0);
    mmio_write(PWM_CTL, 0);
    playing = 0;
    freq_now = 0;
    stop_at = 0;
}

/* start a tone (both channels); ms > 0 stops it by itself, 0 = until sound_stop() */
void sound_tone(int freq, int ms)
{
    if (freq < 20 || freq > 20000 || !volume) { sound_stop(); return; }
    pick_pins();
    if (out_gpio == OUT_I2S) {
        i2s_volume(volume);
        i2s_tone(freq, ms);
        playing = 1; freq_now = freq; stop_at = 0;
        return;
    }
    start_clock();
    gpio_func(pin_a, pin_fn);
    gpio_func(pin_b, pin_fn);
    u32 range = PWM_CLOCK / (u32)freq;
    u32 data = (u32)((u64)range * volume / 200);                   /* 100 % volume = 50 % duty */
    if (!data) data = 1;
    mmio_write(PWM_CTL, 0);
    mmio_write(PWM_RNG1, range); mmio_write(PWM_DAT1, data);
    mmio_write(PWM_RNG2, range); mmio_write(PWM_DAT2, data);
    mmio_write(PWM_CTL, CTL_PWEN1 | CTL_MSEN1 | CTL_PWEN2 | CTL_MSEN2);
    playing = 1;
    freq_now = freq;
    stop_at = ms > 0 ? timer_us() + (u64)ms * 1000 : 0;
}

/* called from the console idle loop: ends timed tones */
void sound_poll(void)
{
    if (i2s_running()) i2s_poll();
    if (playing && stop_at && timer_us() >= stop_at) sound_stop();
}

int  sound_playing(void) { return playing; }
void sound_volume(int v)
{
    volume = v < 0 ? 0 : v > 100 ? 100 : v;
    i2s_volume(volume);
    if (playing && freq_now && out_gpio != OUT_I2S) sound_tone(freq_now, 0);
}
int  sound_get_volume(void) { return volume; }

/* 0 = jack, 1 = GPIO18/19 (PWM), 2 = I2S DAC; sound_output saves it in /SOUND.TXT,
 * sound_use only switches (the JSON API plays one sound on another output) */
static void set_output(int mode, int save)
{
    static const char *name[3] = { "jack\r\n", "gpio\r\n", "i2s\r\n" };
    read_config();
    if (playing) sound_stop();
    mmio_write(PWM_CTL, 0);
    if (pin_a) { gpio_func(pin_a, 0); gpio_func(pin_b, 0); pin_a = 0; }   /* release the old pins */
    i2s_stop();
    out_gpio = mode;
    cfg_read = 1;
    pick_pins();
    if (mode == OUT_I2S) { i2s_volume(volume); i2s_start(); }
    if (save && fat_mounted()) fat_write_file("/SOUND.TXT", name[mode], (u32)strlen(name[mode]));
}
void sound_output(int mode) { set_output(mode, 1); }
void sound_use(int mode)    { set_output(mode, 0); }

/* a sound stream (video) on the current output; 0 = ok */
int sound_stream_open(int hz)
{
    pick_pins();
    i2s_volume(volume);
    if (out_gpio == OUT_I2S) return audio_open(hz, 1);
    if (!pin_a) return -5;
    sound_stop();
    gpio_func(pin_a, pin_fn);
    gpio_func(pin_b, pin_fn);
    clock_on = 0;                          /* the PWM clock changes */
    return audio_open(hz, 0);
}

/* ---------------- 'sound test': find out where the sound gets lost ---------------- */

#define PWM_STA     (PWM_BASE + 0x04)
#define PWM_FIF1    (PWM_BASE + 0x18)

static void show_regs(void)
{
    u32 sel4 = mmio_read(GPFSEL0 + 16);                  /* GPIO40-49 */
    u32 sel1 = mmio_read(GPFSEL0 + 4);                   /* GPIO10-19 */
    kprintf("  PWM CTL=%x STA=%x RNG1=%u DAT1=%u RNG2=%u DAT2=%u\n", mmio_read(PWM_CTL), mmio_read(PWM_STA),
            mmio_read(PWM_RNG1), mmio_read(PWM_DAT1), mmio_read(PWM_RNG2), mmio_read(PWM_DAT2));
    kprintf("  CM_PWMCTL=%x CM_PWMDIV=%x  GPIO40 fn %u, 41 fn %u, 45 fn %u, 18 fn %u, 19 fn %u (4 = ALT0, 2 = ALT5)\n",
            mmio_read(CM_PWMCTL), mmio_read(CM_PWMDIV), (sel4 >> 0) & 7, (sel4 >> 3) & 7, (sel4 >> 15) & 7,
            (sel1 >> 24) & 7, (sel1 >> 27) & 7);
}

static int test_wait(u32 ms)                             /* 1 = Ctrl+C / q */
{
    u64 end = timer_us() + (u64)ms * 1000;
    while (timer_us() < end) {
        int c = con_trygetc();
        if (c == 3 || c == 'q') return 1;
    }
    return 0;
}

void sound_test(void)
{
    int t = board_type();
    read_config();
    int was = out_gpio;
    if (out_gpio == OUT_I2S) { kprintf("output is i2s - testing the jack / GPIO PWM anyway\n"); out_gpio = OUT_JACK; }
    pick_pins();
    sound_exclusive(1);                                  /* no JSON API sound in between */
    kprintf("sound test: board type %x, pins GPIO%d + GPIO%d (fn %d), volume %d %%. Listen; q stops.\n",
            t, pin_a, pin_b, pin_fn, volume);
    if (!volume) kprintf("  volume is 0! ('sound volume 80')\n");

    /* 1: the plain tone path (PWM mark-space, 19.2 MHz crystal clock) */
    kprintf("1/3  tone 440 Hz, 2 s (PWM makes the square wave itself, no DMA)\n");
    clock_on = 0;
    sound_tone(440, 0);
    show_regs();
    if (test_wait(2000)) goto out;
    sound_stop();
    test_wait(500);

    /* 2: the CPU writes a sine into the PWM FIFO (clock PLLD/2 = 250 MHz, like WAV) - no DMA */
    kprintf("2/3  sine 440 Hz, 2 s, written by the CPU into the PWM FIFO (no DMA)\n");
    mmio_write(PWM_CTL, 0);
    mmio_write(PWM_BASE + 0x08, 0);                      /* DMAC off */
    mmio_write(CM_PWMCTL, CM_PASSWD | (mmio_read(CM_PWMCTL) & 0xFFFF & ~CM_ENAB));
    wait_busy(0);
    mmio_write(CM_PWMDIV, CM_PASSWD | (2 << 12));
    mmio_write(CM_PWMCTL, CM_PASSWD | 6);                /* PLLD 500 MHz */
    mmio_write(CM_PWMCTL, CM_PASSWD | 6 | CM_ENAB);
    wait_busy(1);
    clock_on = 0;                                        /* the tone code sets its clock again */
    u32 range = 250000000 / 44100;
    mmio_write(PWM_RNG1, range);
    mmio_write(PWM_RNG2, range);
    mmio_write(PWM_CTL, 1 << 6);                         /* clear the FIFO */
    delay_us(10);
    mmio_write(PWM_STA, 0xFFFFFFFF);                     /* clear the error bits */
    mmio_write(PWM_CTL, (1 << 0) | (1 << 5) | (1 << 8) | (1 << 13));   /* PWEN + USEF, both channels */
    gpio_func(pin_a, pin_fn);
    gpio_func(pin_b, pin_fn);
    /* a 440 Hz sine, the phase runs on without breaks */
    u32 phase = 0, step = (u32)(((u64)440 << 32) / 44100);
    u64 end = timer_us() + 2000000;
    int stopped = 0;
    while (timer_us() < end) {
        if (mmio_read(PWM_STA) & 1) {                    /* FIFO full */
            int c = con_trygetc();
            if (c == 3 || c == 'q') { stopped = 1; break; }
            continue;
        }
        int s = audio_sine(phase) * volume / 100;
        u32 v = (u32)(((s + 32768) * (int)range) >> 16);
        mmio_write(PWM_FIF1, v);                         /* left */
        mmio_write(PWM_FIF1, v);                         /* right */
        phase += step;
    }
    kprintf("  after: STA=%x (bit2 WERR, bit3 RERR, bit4/5 gap, bit8 bus error)\n", mmio_read(PWM_STA));
    show_regs();
    mmio_write(PWM_CTL, 0);
    if (stopped) goto out;
    test_wait(500);

    /* 3: the same through the DMA ring (the path of WAV files and video sound) */
    kprintf("3/3  sine 440 Hz, 2 s, through DMA (the WAV path)\n");
    pick_pins();
    gpio_func(pin_a, pin_fn);
    gpio_func(pin_b, pin_fn);
    if (audio_open(44100, 0) == 0) {
        static short buf[441 * 2];
        phase = 0;
        for (int n = 0; n < 200 && !stopped; n++) {      /* 2 s in 10 ms pieces, the phase continues */
            for (int k = 0; k < 441; k++) {
                int s = audio_sine(phase) * volume / 100;
                buf[k * 2] = buf[k * 2 + 1] = (short)s;
                phase += step;
            }
            audio_write(buf, 441);
            int c = con_trygetc();
            if (c == 3 || c == 'q') stopped = 1;
            if (n == 100) audio_diag();
        }
        audio_close(stopped);
        audio_diag();
    } else kprintf("  audio_open failed\n");
out:
    sound_stop();
    clock_on = 0;
    out_gpio = was;
    sound_exclusive(0);
    kprintf("Which of 1, 2, 3 did you hear? (a photo of this screen helps)\n");
}

/* > 0 while a WAV or a video owns the sound (the JSON API waits) */
static int exclusive;
void sound_exclusive(int on) { exclusive += on ? 1 : -1; if (exclusive < 0) exclusive = 0; }
int  sound_is_exclusive(void) { return exclusive; }

static int sound_wav_l(const char *path, int (*idle)(void));
int sound_wav(const char *path, int (*idle)(void))
{
    sound_exclusive(1);
    int r = sound_wav_l(path, idle);
    sound_exclusive(0);
    return r;
}

/* WAV: on the I2S DAC, or on the PWM (jack / GPIO18-19) by DMA */
static int sound_wav_l(const char *path, int (*idle)(void))
{
    pick_pins();
    i2s_volume(volume);
    if (out_gpio == OUT_I2S) {
        if (!i2s_running()) i2s_start();
        return i2s_wav(path, idle);
    }
    if (!pin_a) return -5;
    sound_stop();
    gpio_func(pin_a, pin_fn);
    gpio_func(pin_b, pin_fn);
    int r = pwm_wav(path, idle);
    clock_on = 0;                          /* the PWM clock was changed for the WAV */
    return r;
}

const char *sound_pins(void)
{
    pick_pins();
    if (out_gpio == OUT_I2S) return "I2S DAC (GPIO18 BCLK, GPIO19 LRCLK, GPIO21 DIN)";
    return out_gpio ? "GPIO18 + GPIO19" : pin_b == 45 ? "3.5 mm jack (GPIO40 + GPIO45)" : "3.5 mm jack (GPIO40 + GPIO41)";
}

/* wait ms while the network etc. keep running; 1 = idle() asked to stop */
static int wait_ms(u32 ms, int (*idle)(void))
{
    u64 end = timer_us() + (u64)ms * 1000;
    while (timer_us() < end)
        if (idle && idle()) return 1;
    return 0;
}

/* a tone for ms milliseconds, waiting until it ends; 1 = interrupted */
int sound_beep(int freq, int ms, int (*idle)(void))
{
    if (sound_output_mode() == OUT_I2S && freq >= 20 && freq <= 20000) {
        sound_tone(freq, ms);              /* the length is counted in samples */
        return wait_ms((u32)ms, idle);
    }
    sound_tone(freq, 0);
    int r = wait_ms((u32)ms, idle);
    sound_stop();
    return r;
}

/* ---------------- music strings ---------------- */

/* octave 4 (middle C = C4) in hundredths of a hertz */
static const u32 oct4[12] = { 26163, 27718, 29366, 31113, 32963, 34923,
                              36999, 39200, 41530, 44000, 46616, 49388 };

static int num(const char **p, int def)
{
    int v = 0, any = 0;
    while (**p >= '0' && **p <= '9') { v = v * 10 + (*(*p)++ - '0'); any = 1; }
    return any ? v : def;
}

static int play(const char *s, int (*idle)(void), int *err, int dry);

/* returns 0, 1 if idle() stopped it, -1 on a syntax error (*err = position, nothing played) */
int sound_play(const char *s, int (*idle)(void), int *err)
{
    int r = play(s, 0, err, 1);                                    /* check everything first */
    return r ? r : play(s, idle, err, 0);
}

static int play(const char *s, int (*idle)(void), int *err, int dry)
{
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };          /* A B C D E F G */
    int octave = 4, len = 4, tempo = 120, art = 7;                 /* articulation: 8 = legato */
    const char *start = s;
    while (*s) {
        char c = *s++;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == ' ' || c == ',' || c == ';') continue;
        if (c == 'O') { octave = num(&s, 4); if (octave > 7) octave = 7; continue; }
        if (c == '<') { if (octave > 0) octave--; continue; }
        if (c == '>') { if (octave < 7) octave++; continue; }
        if (c == 'L') { int l = num(&s, 4); if (l >= 1 && l <= 64) len = l; continue; }
        if (c == 'T') { int t = num(&s, 120); if (t >= 32 && t <= 255) tempo = t; continue; }
        if (c == 'M') {                                            /* MN normal, ML legato, MS staccato */
            char m = *s ? *s++ | 32 : 0;
            art = m == 'l' ? 8 : m == 's' ? 6 : 7;
            continue;
        }
        int note = -1;
        if (c >= 'A' && c <= 'G') {
            note = base[c - 'A'];
            if (*s == '#' || *s == '+') { note++; s++; }
            else if (*s == '-') { note--; s++; }
        } else if (c != 'P' && c != 'R') {
            if (err) *err = (int)(s - start);
            return -1;
        }
        int l = num(&s, len);
        if (l < 1 || l > 64) l = len;
        u32 ms = 240000 / (u32)(tempo * l);                        /* a whole note = 4 beats */
        while (*s == '.') { ms += ms / 2; s++; }
        if (dry) continue;
        if (note < 0) {                                            /* pause */
            if (wait_ms(ms, idle)) return 1;
            continue;
        }
        int o = octave;
        if (note < 0) { note += 12; o--; }
        if (note > 11) { note -= 12; o++; }
        u32 f = oct4[note];
        for (; o > 4; o--) f *= 2;
        for (; o < 4; o++) f /= 2;
        u32 on = ms * (u32)art / 8;
        if (sound_beep((int)((f + 50) / 100), (int)on, idle)) return 1;
        if (ms > on && wait_ms(ms - on, idle)) return 1;
    }
    return 0;
}
