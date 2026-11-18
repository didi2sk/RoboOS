/*
 * Video player: MPEG-1 video + MP2 sound in an MPEG program stream (.mpg),
 * decoded by pl_mpeg (third_party/pl_mpeg.h, MIT). Both codecs are free: their
 * patents have expired. Make a file with
 *   ffmpeg -i in.mp4 -c:v mpeg1video -q:v 4 -c:a mp2 -b:a 192k -f mpeg out.mpg
 *
 * Core 0 reads the card and decodes; the colour conversion (YCbCr -> RGB) and
 * the scaling run on the worker cores 2 + 3 while core 0 decodes the next frame.
 * The picture is drawn into the hidden framebuffer page and then shown (page
 * flip), so it does not tear. Sound goes to the stream API (jack / GPIO / I2S).
 *
 * This file is built with floating point allowed (see the Makefile).
 */
#include "kernel.h"
#include "sd.h"

#define PLM_NO_STDIO
#define PLM_MALLOC(n)      heap_alloc(n)
#define PLM_FREE(p)        heap_free(p)
#define PLM_REALLOC(p, n)  heap_realloc(p, n)
#define PL_MPEG_IMPLEMENTATION
#include "../third_party/pl_mpeg.h"

#define MAXW 4096
#define MAXH 4096

static struct fat_file vf;
static int read_err;
static const char *err = "";

/* ---------------- the file, as pl_mpeg's buffer callbacks ---------------- */

static void v_load(plm_buffer_t *self, void *user)
{
    (void)user;
    if (self->discard_read_bytes) plm_buffer_discard_read_bytes(self);
    size_t room = self->capacity - self->length;
    int n = fat_read(&vf, self->bytes + self->length, (u32)room);
    if (n <= 0) {
        if (n < 0) read_err = 1;
        self->has_ended = TRUE;
        return;
    }
    self->length += (size_t)n;
}

static void v_seek(plm_buffer_t *self, size_t off, void *user)
{
    (void)self; (void)user;
    vf.pos = off > vf.size ? vf.size : (u32)off;
}

static size_t v_tell(plm_buffer_t *self, void *user)
{
    (void)self; (void)user;
    return vf.pos;
}

/* ---------------- picture: YCbCr -> RGB, scaled, on the worker cores ---------------- */

static u16 xtab[MAXW], ytab[MAXH];         /* source column / row for each screen pixel */
static u8 *st_y, *st_cb, *st_cr;           /* a copy of the frame the workers convert */
static int st_yw, st_cw;
static int dx, dy, dw, dh;                 /* where the picture goes on the screen */
static int pages, back;                    /* back = the page being drawn */

struct part { int page, r0, r1; };
static struct part parts[4];

static inline u32 clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (u32)v; }

static void convert(void *arg)
{
    const struct part *p = arg;
    for (int r = p->r0; r < p->r1; r++) {
        int sy = ytab[r];
        const u8 *Y = st_y + sy * st_yw;
        const u8 *Cb = st_cb + (sy >> 1) * st_cw, *Cr = st_cr + (sy >> 1) * st_cw;
        u32 *d = fb_page_row(p->page, dy + r) + dx;
        for (int x = 0; x < dw; x++) {
            int sx = xtab[x];
            int cr = Cr[sx >> 1] - 128, cb = Cb[sx >> 1] - 128;
            int y = ((Y[sx] - 16) * 76309) >> 16;
            int R = y + ((cr * 104597) >> 16);
            int G = y - ((cb * 25674 + cr * 53278) >> 16);
            int B = y + ((cb * 132201) >> 16);
            d[x] = clamp8(R) << 16 | clamp8(G) << 8 | clamp8(B);
        }
    }
}

static int workers[2], nworkers;
static int busy;                           /* a frame is being converted */
static u32 shown, dropped;
static double target;                      /* the time pl_mpeg decodes up to */
static double frame_len;

static void finish_frame(void)
{
    if (!busy) return;
    for (int i = 0; i < nworkers; i++)
        if (smp_wait(workers[i], 1000) < 0) {          /* a stuck core: convert here from now on */
            convert(&parts[i]);
            workers[i] = workers[--nworkers];
            parts[i] = parts[nworkers];
            i--;
        }
    busy = 0;
    if (pages > 1) { fb_show_page(back); back ^= 1; }
    shown++;
}

static void copy_plane(u8 *dst, const plm_plane_t *pl) { memcpy(dst, pl->data, pl->width * pl->height); }

static void on_video(plm_t *plm, plm_frame_t *f, void *user)
{
    (void)plm; (void)user;
    if (f->time + frame_len < target) { dropped++; return; }   /* a newer frame is due already */
    finish_frame();
    copy_plane(st_y, &f->y);
    copy_plane(st_cb, &f->cb);
    copy_plane(st_cr, &f->cr);
    st_yw = (int)f->y.width;
    st_cw = (int)f->cb.width;
    int n = nworkers ? nworkers : 1;
    for (int i = 0; i < n; i++) {
        parts[i].page = pages > 1 ? back : 0;
        parts[i].r0 = dh * i / n;
        parts[i].r1 = dh * (i + 1) / n;
    }
    busy = 1;
    if (!nworkers) { convert(&parts[0]); finish_frame(); return; }
    for (int i = 0; i < nworkers; i++)
        if (smp_run(workers[i], convert, &parts[i]) < 0) convert(&parts[i]);
}

/* ---------------- sound ---------------- */

static short pcm[PLM_AUDIO_SAMPLES_PER_FRAME * 2];
static int sound_on;

static void on_audio(plm_t *plm, plm_samples_t *s, void *user)
{
    (void)plm; (void)user;
    for (unsigned i = 0; i < s->count * 2; i++) {
        float v = s->interleaved[i] * 32767.0f;
        pcm[i] = v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (short)v;
    }
    audio_write(pcm, (int)s->count);
}

/* ---------------- the player ---------------- */

const char *video_error(void) { return err; }

static void clear_pages(void)
{
    int sw = (int)fb_width(), sh = (int)fb_height();
    for (int p = 0; p < pages; p++)
        for (int y = 0; y < sh; y++) {
            u32 *row = fb_page_row(p, y);
            for (int x = 0; x < sw; x++) row[x] = 0;
        }
}

/* the rest of an escape sequence typed on the keyboard / UART */
static int next_key(void)
{
    u64 end = timer_us() + 30000;
    int c;
    while ((c = con_trygetc()) < 0 && timer_us() < end) ;
    return c;
}

/* 0 = played to the end, 1 = stopped by a key, < 0 = error (video_error() says why).
 * info != 0: print the size, rate and the result. */
int video_play(const char *path, int info)
{
    err = "";
    if (!fb_ready()) { err = "video needs the HDMI screen"; return -3; }
    if (!fat_mounted() || fat_open(path, &vf) || vf.is_dir) { err = "file not found"; return -1; }

    heap_reset();
    read_err = 0;
    plm_buffer_t *buf = plm_buffer_create_with_callbacks(v_load, v_seek, v_tell, vf.size, 0);
    plm_t *plm = plm_create_with_buffer(buf, TRUE);
    if (!plm || !plm_has_headers(plm) || !plm_probe(plm, 256 * 1024) || !plm_get_num_video_streams(plm)) {
        err = read_err ? "read error" : "not an MPEG-1 video (.mpg: MPEG-1 video + MP2 sound)";
        heap_reset();
        return -2;
    }
    int w = plm_get_width(plm), h = plm_get_height(plm);
    double fps = plm_get_framerate(plm);
    if (w <= 0 || h <= 0 || w > MAXW || h > MAXH || fps <= 0) { err = "unsupported picture size"; heap_reset(); return -2; }
    frame_len = 1.0 / fps;

    /* fit the screen, keep the shape, at most 2x bigger */
    int sw = (int)fb_width(), sh = (int)fb_height();
    if ((long)sw * h <= (long)sh * w) { dw = sw; dh = (int)((long)h * sw / w); }
    else                              { dh = sh; dw = (int)((long)w * sh / h); }
    if (dw > 2 * w) { dw = 2 * w; dh = 2 * h; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    dx = (sw - dw) / 2; dy = (sh - dh) / 2;
    for (int x = 0; x < dw; x++) xtab[x] = (u16)(x * w / dw);
    for (int y = 0; y < dh; y++) ytab[y] = (u16)(y * h / dh);

    int pw = ((w + 15) & ~15), ph = ((h + 15) & ~15);     /* planes are whole macroblocks */
    st_y = heap_alloc((size_t)pw * ph);
    st_cb = heap_alloc((size_t)pw * ph / 4);
    st_cr = heap_alloc((size_t)pw * ph / 4);
    if (!st_y || !st_cb || !st_cr) { err = "out of memory"; heap_reset(); return -2; }

    nworkers = 0;
    for (int c = 2; c <= 3; c++) if (smp_worker_ok(c)) workers[nworkers++] = c;

    int rate = plm_get_samplerate(plm);
    sound_on = 0;
    if (plm_get_num_audio_streams(plm) > 0 && rate > 0 && sound_stream_open(rate) == 0) sound_on = 1;
    plm_set_audio_enabled(plm, sound_on);
    if (sound_on) {
        plm_set_audio_stream(plm, 0);
        plm_set_audio_lead_time(plm, 0.2);
        plm_set_audio_decode_callback(plm, on_audio, 0);
    }
    plm_set_video_decode_callback(plm, on_video, 0);
    plm_set_loop(plm, FALSE);

    pages = fb_pages();
    back = pages > 1 ? 1 : 0;
    busy = 0; shown = dropped = 0; target = 0;
    fb_hold(1);
    clear_pages();
    sound_exclusive(1);

    int result = 0, paused = 0;
    u64 t0 = timer_us(), last = t0, paused_us = 0;
    while (!plm_has_ended(plm)) {
        int c = con_trygetc();
        if (c == 27) {                                     /* Esc alone, or a cursor key */
            int a = next_key(), b = (a == '[' || a == 'O') ? next_key() : -1;
            if (a != '[' && a != 'O') { result = 1; break; }
            if (b == 'C' || b == 'D') {
                double t = plm_get_time(plm) + (b == 'C' ? 10.0 : -10.0);
                if (t < 0) t = 0;
                finish_frame();
                plm_seek(plm, t, FALSE);
                target = plm_get_time(plm);
            }
        } else if (c == 'q' || c == 'Q' || c == '\r' || c == '\n' || c == 3) {
            result = 1;
            break;
        } else if (c == ' ') {
            paused = !paused;
        }
        u64 now = timer_us();
        u64 dt_us = now - last;
        last = now;
        if (paused) { paused_us += dt_us; continue; }
        double dt = (double)dt_us / 1000000.0;
        if (dt > 0.25) dt = 0.25;                          /* far behind: slow down, do not hang */
        target += dt;
        plm_decode(plm, dt);
        if (read_err) { err = "read error"; result = -4; break; }
    }
    finish_frame();
    sound_exclusive(0);
    u64 played = timer_us() - t0 - paused_us;
    if (sound_on) audio_close(result != 0);
    if (pages > 1) fb_show_page(0);
    fb_hold(0);
    heap_reset();

    if (info) {
        u32 ms = (u32)(played / 1000);
        u32 fps100 = played ? (u32)((u64)shown * 100000000ULL / played) : 0;
        kprintf("%dx%d at %d.%02d fps -> %dx%d on screen, %s, %s\n", w, h, (int)fps, (int)(fps * 100) % 100,
                dw, dh, nworkers ? (nworkers == 2 ? "cores 2+3" : "1 worker core") : "core 0 only",
                sound_on ? "sound" : "no sound");
        kprintf("%u frames shown, %u dropped in %u.%02u s = %u.%02u fps%s\n", shown, dropped,
                ms / 1000, (ms % 1000) / 10, fps100 / 100, fps100 % 100, result == 1 ? " (stopped)" : "");
        if (result < 0) kprintf("%s\n", err);
    }
    return result;
}

void video_cmd(int argc, char **argv)
{
    char p[PATH_MAX_LEN];
    if (argc < 2) {
        kprintf("usage: video <file.mpg>   (MPEG-1 video + MP2 sound; q stop, space pause,\n"
                "       <- -> 10 s back / forward). Convert with:\n"
                "       ffmpeg -i in.mp4 -c:v mpeg1video -q:v 4 -c:a mp2 -b:a 192k -f mpeg out.mpg\n");
        return;
    }
    if (path_abs(argv[1], p, sizeof(p))) { kprintf("bad file name\n"); return; }
    int r = video_play(p, 1);
    if (r < 0 && r != -4) kprintf("%s: %s\n", argv[1], video_error());
}
