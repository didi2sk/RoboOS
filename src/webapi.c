/*
 * JSON API on the web server (port 80, 'http on'): read and set the GPIO pins,
 * board status, reboot. Every /api/ request needs the header
 *   Authorization: Bearer <token>
 * with the token from 'http token <secret>' (saved in /HTTP.TXT). Without a
 * token the API is off.
 *
 *   GET  /api/status            board, version, address, uptime, temperature
 *   GET  /api/gpio              all pins 0-27: mode + level
 *   GET  /api/gpio/17           one pin
 *   POST /api/gpio              {"pin":17,"value":1}  (value 0, 1 or "toggle"; makes the pin an output)
 *                               {"pin":4,"mode":"in","pull":"up"}  modes in/out, pull up/down/off
 *                               [{...},{...}] several at once
 *   POST /api/reboot            answers, then reboots in 0.5 s
 *   GET  /api/sound             what plays, the last result, the files in /SOUNDS
 *   POST /api/sound             {"file":"bell.wav","out":"jack"}   a WAV from /SOUNDS; out = jack, gpio,
 *                               i2s (also "dac" / "device"), or none = the current output; "volume":0-100
 *                               {"tone":880,"ms":500}   a tone      {"stop":true}   stop it
 *                               (core 0 plays it - the SD card is core 0 only - while it waits in the
 *                               shell, BASIC or a program; the answer comes at once: 202 queued)
 *
 * Runs on the I/O core (from the lwIP callbacks in ip.c); no lwIP here.
 */
#include "kernel.h"
#include "sd.h"

static char token[64];
static u64 reboot_at;

void api_set_token(const char *t)
{
    int i = 0;
    for (; t && t[i] && i < (int)sizeof(token) - 1; i++) token[i] = t[i];
    token[i] = 0;
}
const char *api_token(void) { return token; }

/* from ip_poll: the reboot asked for over the network, after the answer went out */
void api_poll(void)
{
    if (reboot_at && timer_us() >= reboot_at) sys_reboot();
}

/* ---------------- the answer ---------------- */

static char *out;
static int olen, omax;

static void put(const char *s) { while (*s && olen < omax - 1) out[olen++] = *s++; out[olen] = 0; }
static void put_i(long v)
{
    char t[24]; int k = 0;
    if (v < 0) { put("-"); v = -v; }
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k && olen < omax - 1) out[olen++] = t[--k];
    out[olen] = 0;
}
static void put_str(const char *s)                       /* a JSON string */
{
    put("\"");
    for (; *s; s++) {
        char c[2] = { *s, 0 };
        if (*s == '"' || *s == '\\') put("\\");
        if ((u8)*s < 32) continue;
        put(c);
    }
    put("\"");
}

static int body_start;

static void head(int code)
{
    olen = 0;
    put("HTTP/1.0 ");
    put_i(code);
    put(code == 200 ? " OK" : code == 400 ? " Bad Request" : code == 401 ? " Unauthorized" :
        code == 202 ? " Accepted" : code == 403 ? " Forbidden" : code == 404 ? " Not Found" : code == 405 ? " Method Not Allowed" : code == 429 ? " Too Many Requests" : " Error");
    put("\r\nContent-Type: application/json\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n");
    if (code == 401) put(users_count() ? "WWW-Authenticate: Basic realm=\"RoboOS\", charset=\"UTF-8\"\r\n" : "WWW-Authenticate: Bearer\r\n");
    put("\r\n");
    body_start = olen;
}

static int fail(int code, const char *msg)
{
    head(code);
    put("{\"ok\":false,\"error\":"); put_str(msg); put("}\n");
    return olen;
}

/* ---------------- a tiny JSON reader (objects with numbers, strings, booleans) ---------------- */

static const char *jp, *jend;
static const char *jerr;

static void ws(void) { while (jp < jend && (*jp == ' ' || *jp == '\t' || *jp == '\r' || *jp == '\n')) jp++; }
static int peek(void) { ws(); return jp < jend ? *jp : -1; }
static int eat(char c) { if (peek() == c) { jp++; return 1; } return 0; }

/* kinds of value */
enum { J_NONE, J_NUM, J_STR, J_TRUE, J_FALSE, J_NULL };

static int value(long *num, char *str, int smax)
{
    int c = peek();
    if (c == '"') {
        int n = 0;
        jp++;
        while (jp < jend && *jp != '"') {
            char ch = *jp++;
            if (ch == '\\' && jp < jend) { ch = *jp++; if (ch == 'n') ch = '\n'; else if (ch == 't') ch = ' '; }
            if (n < smax - 1) str[n++] = ch;
        }
        if (jp >= jend) { jerr = "unfinished string"; return J_NONE; }
        jp++;
        str[n] = 0;
        return J_STR;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        int neg = 0; long v = 0;
        if (*jp == '-') { neg = 1; jp++; }
        if (jp >= jend || *jp < '0' || *jp > '9') { jerr = "bad number"; return J_NONE; }
        while (jp < jend && *jp >= '0' && *jp <= '9') { if (v < 100000000) v = v * 10 + (*jp - '0'); jp++; }
        if (jp < jend && (*jp == '.' || *jp == 'e' || *jp == 'E')) { jerr = "whole numbers only"; return J_NONE; }
        *num = neg ? -v : v;
        return J_NUM;
    }
    static const struct { const char *w; int k; } words[] = { { "true", J_TRUE }, { "false", J_FALSE }, { "null", J_NULL } };
    for (int i = 0; i < 3; i++) {
        int l = (int)strlen(words[i].w);
        if (jend - jp >= l && !memcmp_n(jp, words[i].w, (size_t)l)) { jp += l; return words[i].k; }
    }
    jerr = (c == '{' || c == '[') ? "nested objects are not supported" : "bad value";
    return J_NONE;
}

/* one pin request: {"pin":17,"value":1,"mode":"out","pull":"up"} */
struct pin_req { int pin, has_value, val, toggle, mode, pull; };   /* mode/pull -1 = unchanged */

static int object(struct pin_req *r)
{
    r->pin = -1; r->has_value = 0; r->val = 0; r->toggle = 0; r->mode = -1; r->pull = -1;
    if (!eat('{')) { jerr = "expected an object {...}"; return 0; }
    if (eat('}')) { jerr = "empty object"; return 0; }
    do {
        char key[16], s[16];
        long n = 0;
        if (value(&n, key, sizeof(key)) != J_STR) { if (!jerr) jerr = "expected a key"; return 0; }
        if (!eat(':')) { jerr = "expected ':'"; return 0; }
        int k = value(&n, s, sizeof(s));
        if (k == J_NONE) return 0;
        if (!strcmp(key, "pin")) {
            if (k != J_NUM) { jerr = "pin must be a number"; return 0; }
            r->pin = (int)n;
        } else if (!strcmp(key, "value") || !strcmp(key, "state")) {
            r->has_value = 1;
            if (k == J_NUM) r->val = n != 0;
            else if (k == J_TRUE || k == J_FALSE) r->val = k == J_TRUE;
            else if (k == J_STR && !strcmp(s, "toggle")) r->toggle = 1;
            else if (k == J_STR && (!strcmp(s, "on") || !strcmp(s, "high"))) r->val = 1;
            else if (k == J_STR && (!strcmp(s, "off") || !strcmp(s, "low"))) r->val = 0;
            else { jerr = "value: 0, 1, true, false or \"toggle\""; return 0; }
        } else if (!strcmp(key, "mode")) {
            if (k == J_STR && !strcmp(s, "in")) r->mode = 0;
            else if (k == J_STR && !strcmp(s, "out")) r->mode = 1;
            else { jerr = "mode: \"in\" or \"out\""; return 0; }
        } else if (!strcmp(key, "pull")) {
            if (k == J_STR && !strcmp(s, "up")) r->pull = 2;
            else if (k == J_STR && !strcmp(s, "down")) r->pull = 1;
            else if ((k == J_STR && !strcmp(s, "off")) || k == J_NULL) r->pull = 0;
            else { jerr = "pull: \"up\", \"down\" or \"off\""; return 0; }
        } else { jerr = "unknown key (pin, value, mode, pull)"; return 0; }
    } while (eat(','));
    if (!eat('}')) { jerr = "expected ',' or '}'"; return 0; }
    if (r->pin < 0 || r->pin > 27) { jerr = "pin 0-27 is needed"; return 0; }
    if (r->has_value && r->mode == 0) { jerr = "an input cannot get a value"; return 0; }
    return 1;
}

/* ---------------- pins ---------------- */

static const char *mode_name(int pin)
{
    static const char *const n[8] = { "in", "out", "alt5", "alt4", "alt0", "alt1", "alt2", "alt3" };
    return n[gpio_mode(pin)];
}

static void put_pin(int pin)
{
    put("{\"pin\":"); put_i(pin);
    put(",\"mode\":\""); put(mode_name(pin));
    put("\",\"value\":"); put_i(gpio_get(pin)); put("}");
}

static void apply(const struct pin_req *r)
{
    if (r->pull >= 0) gpio_pull(r->pin, r->pull);
    if (r->mode >= 0 || r->has_value) gpio_func(r->pin, r->has_value ? 1 : r->mode);
    if (r->has_value) gpio_set(r->pin, r->toggle ? !gpio_get(r->pin) : r->val);
}

static int post_gpio(const char *body, int blen)
{
    struct pin_req reqs[28];
    int n = 0, list;
    jp = body; jend = body + blen; jerr = 0;
    list = eat('[');
    do {
        if (n == 28) { jerr = "at most 28 pins"; break; }
        if (!object(&reqs[n])) break;
        n++;
    } while (list && eat(','));
    if (!jerr && list && !eat(']')) jerr = "expected ',' or ']'";
    if (!jerr && peek() != -1) jerr = "extra text after the JSON";
    if (jerr) return fail(400, jerr);
    for (int i = 0; i < n; i++) apply(&reqs[i]);          /* all checked first: all or nothing */
    head(200);
    put("{\"ok\":true,\"gpio\":[");
    for (int i = 0; i < n; i++) { if (i) put(","); put_pin(reqs[i].pin); }
    put("]}\n");
    return olen;
}

/* ---------------- sound: queued on core 1, played on core 0 ---------------- */

#define SND_DIR "/SOUNDS"
static const char *const out_names[3] = { "jack", "gpio", "i2s" };

struct snd_req { char file[48]; int out, volume, tone, ms, stop; };
static struct snd_req req;                 /* the newest request (snd_lock) */
static volatile u32 req_seq, done_seq;
static volatile int snd_stop;
static int snd_lock;
static char playing[48], last[64] = "nothing played yet";
static char files[32][24];
static int nfiles;

static void slock(void)   { while (__atomic_test_and_set(&snd_lock, __ATOMIC_ACQUIRE)) ; }
static void sunlock(void) { __atomic_clear(&snd_lock, __ATOMIC_RELEASE); }
static void scopy(char *d, const char *s, int max) { int i = 0; for (; s && s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }

static char scan[32][24];
static int nscan;
static int list_cb(const struct fat_dirent *e, void *ctx)
{
    (void)ctx;
    if (!e->is_dir && nscan < 32) scopy(scan[nscan++], e->name, sizeof(scan[0]));
    return 0;
}

static volatile int cur_out;               /* the current output, kept by core 0 for core 1 */

/* core 0 (boot, after each sound): the file list and the output for GET /api/sound */
void api_sound_scan(void)
{
    if (smp_core() != 0) return;
    cur_out = sound_output_mode();
    if (!fat_mounted()) return;
    nscan = 0;
    fat_list(SND_DIR, list_cb, 0);
    slock();
    for (int i = 0; i < nscan; i++) scopy(files[i], scan[i], sizeof(files[0]));
    nfiles = nscan;
    sunlock();
}

/* while a WAV plays: stop on {"stop":true} or when a newer request comes */
static int snd_idle(void) { return snd_stop || req_seq != done_seq; }

static int restore_out = -1;               /* a tone on another output: switch back afterwards */
static u64 restore_at;

void api_sound_poll(void)
{
    static int busy;
    if (busy || smp_core() != 0 || io_held() || sound_is_exclusive()) return;
    if (req_seq == done_seq) {
        cur_out = sound_output_mode();
        if (restore_out >= 0 && timer_us() >= restore_at) { sound_stop(); sound_use(restore_out); restore_out = -1; }
        return;
    }
    struct snd_req r;
    slock(); r = req; done_seq = req_seq; snd_stop = 0; sunlock();
    busy = 1;
    if (restore_out >= 0) { sound_stop(); sound_use(restore_out); restore_out = -1; }
    if (r.stop) {
        sound_stop();
        slock(); scopy(last, "stopped", sizeof(last)); sunlock();
        busy = 0;
        return;
    }
    int prev = sound_output_mode();
    if (r.volume >= 0) sound_volume(r.volume);
    if (r.out >= 0 && r.out != prev) sound_use(r.out);
    if (r.file[0]) {
        char path[64];
        scopy(path, SND_DIR "/", sizeof(path));
        scopy(path + 8, r.file, (int)sizeof(path) - 8);
        slock(); scopy(playing, r.file, sizeof(playing)); sunlock();
        kprintf("\n[API] sound %s on %s\n", path, out_names[sound_output_mode()]);
        int e = sound_wav(path, snd_idle);
        slock();
        playing[0] = 0;
        if (e < 0) {
            scopy(last, r.file, sizeof(last));
            int k = (int)strlen(last);
            scopy(last + k, ": ", (int)sizeof(last) - k);
            k = (int)strlen(last);
            scopy(last + k, i2s_wav_error(e), (int)sizeof(last) - k);
        }
        else scopy(last, snd_stop ? "stopped" : e == 1 ? "interrupted" : "ok", sizeof(last));
        sunlock();
        if (sound_output_mode() != prev) sound_use(prev);
        api_sound_scan();
    } else if (r.tone) {
        int ms = r.ms > 0 ? r.ms : 500;
        sound_tone(r.tone, ms);
        slock(); scopy(last, "tone", sizeof(last)); sunlock();
        if (sound_output_mode() != prev) { restore_out = prev; restore_at = timer_us() + (u64)ms * 1000 + 50000; }
    }
    busy = 0;
}

static int post_sound(const char *body, int blen)
{
    struct snd_req r = { "", -1, -1, 0, 0, 0 };
    char key[16], sv[48];
    long n = 0;
    jp = body; jend = body + blen; jerr = 0;
    if (!eat('{')) return fail(400, "expected an object {...}");
    int empty = eat('}');
    if (!empty) do {
        if (value(&n, key, sizeof(key)) != J_STR) return fail(400, jerr ? jerr : "expected a key");
        if (!eat(':')) return fail(400, "expected ':'");
        int k = value(&n, sv, sizeof(sv));
        if (k == J_NONE) return fail(400, jerr);
        if (!strcmp(key, "file")) {
            if (k != J_STR || !sv[0]) return fail(400, "file: a name in " SND_DIR);
            for (int i = 0; sv[i]; i++)
                if (sv[i] == '/' || sv[i] == '\\' || (u8)sv[i] < 32 || (sv[i] == '.' && sv[i + 1] == '.'))
                    return fail(400, "file: a plain name, no folders");
            scopy(r.file, sv, sizeof(r.file));
        } else if (!strcmp(key, "out")) {
            if (k != J_STR) return fail(400, "out: jack, gpio or i2s");
            if (!strcmp(sv, "jack")) r.out = 0;
            else if (!strcmp(sv, "gpio")) r.out = 1;
            else if (!strcmp(sv, "i2s") || !strcmp(sv, "dac") || !strcmp(sv, "device")) r.out = 2;
            else return fail(400, "out: jack, gpio or i2s");
        } else if (!strcmp(key, "volume")) {
            if (k != J_NUM || n < 0 || n > 100) return fail(400, "volume: 0-100");
            r.volume = (int)n;
        } else if (!strcmp(key, "tone")) {
            if (k != J_NUM || n < 20 || n > 20000) return fail(400, "tone: 20-20000 Hz");
            r.tone = (int)n;
        } else if (!strcmp(key, "ms")) {
            if (k != J_NUM || n < 1 || n > 60000) return fail(400, "ms: 1-60000");
            r.ms = (int)n;
        } else if (!strcmp(key, "stop")) {
            r.stop = k == J_TRUE || (k == J_NUM && n);
        } else return fail(400, "unknown key (file, out, volume, tone, ms, stop)");
    } while (eat(','));
    if (!empty && !eat('}')) return fail(400, "expected ',' or '}'");
    if (peek() != -1) return fail(400, "extra text after the JSON");
    if (!r.stop && !r.file[0] && !r.tone) return fail(400, "give file, tone or stop");
    slock();
    req = r;
    req_seq++;
    if (r.stop) snd_stop = 1;
    sunlock();
    head(202);
    put("{\"ok\":true,\"queued\":");
    put_str(r.stop ? "stop" : r.file[0] ? r.file : "tone");
    put(",\"out\":"); put_str(r.out >= 0 ? out_names[r.out] : "current");
    put("}\n");
    return olen;
}

static int get_sound(void)
{
    head(200);
    slock();
    put("{\"ok\":true,\"folder\":\"" SND_DIR "\",\"playing\":");
    if (playing[0]) put_str(playing); else put("null");
    put(",\"last\":"); put_str(last);
    put(",\"out\":"); put_str(out_names[cur_out]);
    put(",\"volume\":"); put_i(sound_get_volume());
    put(",\"files\":[");
    for (int i = 0; i < nfiles; i++) { if (i) put(","); put_str(files[i]); }
    put("]}\n");
    sunlock();
    return olen;
}

/* ---------------- requests ---------------- */

static int same_token(const char *a, int alen)
{
    int tl = (int)strlen(token), d = alen ^ tl;
    for (int i = 0; i < tl; i++) d |= (i < alen ? a[i] : 0) ^ token[i];   /* the same time for every guess */
    return tl > 0 && d == 0;
}

/* an admin for uploads (ip.c): a user login with role 2, or the Bearer token */
int api_admin_ok(const char *auth, int role, const char *from)
{
    if (role == 2) return 1;
    if (role == 1) return 0;
    if (users_locked(from)) return -1;
    if (!token[0]) return -2;
    int al = 0;
    if (auth) { while (*auth == ' ') auth++; if (!memcmp_n(auth, "Bearer ", 7)) auth += 7; else auth = 0; }
    if (auth) { while (*auth == ' ') auth++; while (auth[al] && auth[al] != ' ' && auth[al] != '\r') al++; }
    if (!auth || !same_token(auth, al)) { if (auth) users_failed(from); return 0; }
    users_ok(from);
    return 1;
}

void api_reboot_later(u32 ms) { reboot_at = timer_us() + (u64)ms * 1000; }

/*
 * method, path, the Authorization header value (or 0) and the body; the whole
 * HTTP answer goes to o. Returns its length.
 */
int api_handle(const char *method, const char *path, const char *auth, int role, const char *body, int blen,
               const char *from, char *o, int max)
{
    out = o; omax = max; olen = 0;
    if (role) goto logged_in;                             /* a user login (users.c) instead of the token */
    if (users_locked(from)) return fail(429, "too many wrong logins from this address - wait a minute");
    if (!token[0]) return fail(403, users_count() ? "log in (HTTP Basic: curl -u name:password)" : "API off: set a token on the board with 'http token <secret>'");
    int alen = 0;
    if (auth) {
        while (*auth == ' ') auth++;
        if (!memcmp_n(auth, "Bearer ", 7)) auth += 7; else auth = 0;
    }
    if (auth) { while (*auth == ' ') auth++; while (auth[alen] && auth[alen] != ' ' && auth[alen] != '\r') alen++; }
    if (!auth || !same_token(auth, alen)) {
        if (auth) users_failed(from); else kprintf("\n[API] missing token from %s\n", from);
        return fail(401, users_count() ? "log in (HTTP Basic: curl -u name:password) or Authorization: Bearer <token>" : "Authorization: Bearer <token> needed");
    }
    users_ok(from);
    role = 2;                                             /* the token counts as an admin */
logged_in:;
    int get = !strcmp(method, "GET"), post = !strcmp(method, "POST") || !strcmp(method, "PUT");
    if (role < 2 && !get) return fail(403, "this user may only read (GET) - an admin user can change things");

    if (!strcmp(path, "/api/status") || !strcmp(path, "/api") || !strcmp(path, "/api/")) {
        if (!get) return fail(405, "use GET");
        u32 t[2] = { 0, 0 };
        int tc = mbox_prop(0x00030006, t, 2) ? (int)t[1] : -1;
        head(200);
        put("{\"ok\":true,\"name\":"); put_str(net_name());
        put(",\"board\":"); put_str(board_name());
        put(",\"version\":"); put_str(os_version());
        put(",\"address\":"); put_str(ip_address_str());
        put(",\"uptime_s\":"); put_i((long)(timer_us() / 1000000));
        u64 now = clock_now();
        if (now) {
            char tb[32];
            clock_format(tb, now, 1);
            put(",\"time\":"); put_str(tb);
            put(",\"unix\":"); put_i((long)now);
        }
        if (tc >= 0) { put(",\"cpu_temp_mc\":"); put_i(tc); }
        put("}\n");
        return olen;
    }
    if (!strcmp(path, "/api/gpio")) {
        if (post) return post_gpio(body, blen);
        if (!get) return fail(405, "use GET or POST");
        head(200);
        put("{\"ok\":true,\"gpio\":[");
        for (int p = 0; p <= 27; p++) { if (p) put(","); put_pin(p); }
        put("]}\n");
        return olen;
    }
    if (!memcmp_n(path, "/api/gpio/", 10)) {
        int p = 0, k = 10;
        if (!path[k]) return fail(404, "pin number missing");
        while (path[k] >= '0' && path[k] <= '9' && p < 100) p = p * 10 + (path[k++] - '0');
        if (path[k] || p > 27) return fail(404, "pin 0-27");
        if (!get) return fail(405, "use GET (POST /api/gpio sets pins)");
        head(200);
        put("{\"ok\":true,\"gpio\":"); put_pin(p); put("}\n");
        return olen;
    }
    if (!strcmp(path, "/api/ble")) {                      /* Bluetooth thermometers (bt.c) */
        if (!get) return fail(405, "use GET");
        static char jb[1536];
        int jl = bt_ble_json(jb, sizeof(jb));
        head(200);
        if (jl) put(jb);
        return olen;
    }
    if (!strcmp(path, "/api/sound")) {
        if (post) return post_sound(body, blen);
        if (!get) return fail(405, "use GET or POST");
        return get_sound();
    }
    if (!strcmp(path, "/api/display")) {                  /* the I2C display (display.c draws it on core 0) */
        if (post) {
            char key[16], sv[192];
            long n = 0, row = 0;
            int have_text = 0, clear = 0, tl = 0;
            static char tx[192];
            jp = body; jend = body + blen; jerr = 0;
            if (!eat('{')) return fail(400, "expected an object {...}");
            do {
                if (value(&n, key, sizeof(key)) != J_STR) return fail(400, jerr ? jerr : "expected a key");
                if (!eat(':')) return fail(400, "expected ':'");
                int k = value(&n, sv, sizeof(sv));
                if (k == J_NONE) return fail(400, jerr);
                if (!strcmp(key, "text")) { if (k != J_STR) return fail(400, "text: a string (\\n = new row)"); scopy(tx, sv, sizeof(tx)); have_text = 1; }
                else if (!strcmp(key, "row")) { if (k != J_NUM || n < 1 || n > 10) return fail(400, "row: 1-10"); row = n; }
                else if (!strcmp(key, "clear")) clear = k == J_TRUE || (k == J_NUM && n);
                else return fail(400, "unknown key (text, row, clear)");
            } while (eat(','));
            if (!eat('}')) return fail(400, "expected ',' or '}'");
            if (!have_text && !clear) return fail(400, "give text (and row) or clear");
            if (!display_rows()) return fail(409, "no display found on the board");
            while (tx[tl]) tl++;
            display_net(row ? (int)row - 1 : -1, have_text ? tx : "", have_text ? tl : 0);
            head(202);
            put("{\"ok\":true}\n");
            return olen;
        }
        if (!get) return fail(405, "use GET or POST");
        head(200);
        put("{\"ok\":true,\"display\":"); put_str(display_type_name());
        put(",\"rows\":"); put_i(display_rows());
        put(",\"cols\":"); put_i(display_cols());
        put(",\"mode\":"); put_str(display_mode_name());
        put(",\"text\":[");
        for (int r = 0; r < display_rows(); r++) {
            char rt[32]; int k = 0;
            const char *s = display_row_text(r);
            for (; k < display_cols() && s[k]; k++) rt[k] = s[k];
            while (k && rt[k - 1] == ' ') k--;
            rt[k] = 0;
            if (r) put(",");
            put_str(rt);
        }
        put("]}\n");
        return olen;
    }
    if (!strcmp(path, "/api/radio")) {                    /* FM radio (radio.c runs the command on core 0) */
        if (post) {
            char key[16], sv[48];
            long n = 0;
            int got = 0;
            jp = body; jend = body + blen; jerr = 0;
            if (!eat('{')) return fail(400, "expected an object {\"cmd\":\"104.4\"}");
            do {
                if (value(&n, key, sizeof(key)) != J_STR) return fail(400, jerr ? jerr : "expected a key");
                if (!eat(':')) return fail(400, "expected ':'");
                int k = value(&n, sv, sizeof(sv));
                if (k != J_STR || strcmp(key, "cmd")) return fail(400, "only {\"cmd\":\"...\"}: 104.4, up, down, on, off, vol 8, mono on");
                got = 1;
            } while (eat(','));
            if (!eat('}') || !got) return fail(400, "expected {\"cmd\":\"...\"}");
            if (!radio_present()) return fail(409, "no FM radio on the board");
            int l = 0; while (sv[l]) l++;
            radio_net(sv, l);
            head(202);
            put("{\"ok\":true,\"queued\":"); put_str(sv); put("}\n");
            return olen;
        }
        if (!get) return fail(405, "use GET or POST");
        head(200);
        u32 f = radio_freq();
        char fs[12]; int k = 0;
        { u32 m = f / 100; char t[6]; int tk = 0; do t[tk++] = (char)('0' + m % 10); while ((m /= 10)); while (tk) fs[k++] = t[--tk]; }
        fs[k++] = '.'; fs[k++] = (char)('0' + f / 10 % 10); fs[k] = 0;
        put("{\"ok\":true,\"present\":"); put(radio_present() ? "true" : "false");
        put(",\"on\":"); put(radio_on() ? "true" : "false");
        put(",\"freq\":"); put(fs);
        put(",\"volume\":"); put_i(radio_volume());
        put(",\"rssi\":"); put_i(radio_rssi());
        put(",\"stereo\":"); put(radio_stereo() ? "true" : "false");
        { char p[9]; int j = 0; const char *s = radio_ps(); for (int i = 0; i < 8; i++) if (s[i]) p[j++] = s[i]; p[j] = 0; put(",\"station\":"); put_str(p); }
        { char r[65]; int j = 0; const char *s = radio_rt(); for (int i = 0; i < 64 && s[i]; i++) r[j++] = s[i]; r[j] = 0; put(",\"text\":"); put_str(r); }
        put("}\n");
        return olen;
    }
    if (!strcmp(path, "/api/sensors")) {                  /* 0.82: the I2C sensors (read here, on core 1 - like MQTT) */
        if (!get) return fail(405, "use GET");
        struct sensor_values v;
        sensors_read(&v);
        head(200);
        put("{\"ok\":true");
        char b[24];
        #define C100(name, val) do { int x_ = (val); int k_ = 0; if (x_ < 0) { b[k_++] = '-'; x_ = -x_; } \
            char t_[12]; int n_ = 0; u32 w_ = (u32)(x_ / 100); do t_[n_++] = (char)('0' + w_ % 10); while ((w_ /= 10)); \
            while (n_) b[k_++] = t_[--n_]; b[k_++] = '.'; b[k_++] = (char)('0' + x_ / 10 % 10); b[k_++] = (char)('0' + x_ % 10); b[k_] = 0; \
            put(",\"" name "\":"); put(b); } while (0)
        if (v.have & SV_CPU) C100("cpu_c", v.cpu_mc / 10);
        if (v.have & SV_TEMP) C100("temp_c", v.temp_c100);
        if (v.have & SV_HUM) C100("hum_pct", v.hum_c100);
        if (v.have & SV_PRESS) C100("press_hpa", v.press_pa);
        #undef C100
        if (v.have & SV_AIN) { put(",\"ain_mv\":["); for (int i = 0; i < 4; i++) { if (i) put(","); put_i(v.ain_mv[i]); } put("]"); }
        put(",\"log_min\":"); put_i(sensors_log_every());
        put("}\n");
        return olen;
    }
    if (!strcmp(path, "/api/reboot")) {
        if (!post) return fail(405, "use POST");
        kprintf("\n[API] reboot asked by %s\n", from);
        reboot_at = timer_us() + 500000;
        head(200);
        put("{\"ok\":true,\"rebooting\":true}\n");
        return olen;
    }
    return fail(404, "unknown: GET /api/status, GET|POST /api/gpio, GET /api/gpio/<pin>, GET|POST /api/sound, GET|POST /api/display, GET|POST /api/radio, GET /api/sensors, POST /api/reboot");
}
