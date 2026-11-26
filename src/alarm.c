/*
 * alarm.c - the burglar alarm in the kernel (0.120; before, only the SDK demo sdk/examples/alarm.c).
 *
 * Zones = contacts / PIR outputs on GPIO pins (internal pull-up, contact to GND; nc = normally closed, a door
 * contact: open = HIGH = triggered; no = normally open, a PIR relay: closed = LOW = triggered). Zone types:
 *   instant   alarm at once while armed (window)
 *   delay     starts the entry time while armed (the door you come in by); ignored in the exit time
 *   interior  like instant, but ignored in the exit / entry time (the hall PIR on the way to the keypad)
 *             and bypassed when armed "home" (you are inside)
 *   24h       alarm at any time, also disarmed (tamper, panic button) - on the opening edge
 * States: disarmed, exit (arming), armed away / home, entry (pending), alarm (triggered). After the siren
 * time the alarm re-arms by itself (zones still open are skipped until they close); the cause is kept as the
 * "alarm memory" until the next disarm. The armed state is kept in /ALARM.ST, so a power cut does not disarm.
 *
 * On an alarm: N photos 1 s apart (camera_take: /CAM/<date>/ and the first one to MQTT <base>/camera), then
 * a video clip of N seconds (camera_clip_start: MJPEG AVI in /CAM/<date>/), MQTT <base>/alarm/event JSON.
 * The entry time can take a photo too (who comes in). Sound on the jack: beeps in exit / entry time, the
 * last 10 s spoken, voice files from /ALARM (ARMING ARMED ENTRY ALARM DISARMED WRONG NOARM, 1..10), the
 * siren as a rising / falling tone; siren relay / LED on GPIO pins.
 *
 * Settings: /ALARM.TXT (the demo's format + state/photo/photos/video/entry_photo/mqtt); event log: /ALARM.LOG
 * (the last 48 lines, rewritten 2 s after a change). Control: shell 'alarm', the web page /alarm + JSON API
 * GET|POST /api/alarm (webapi.c, core 1), Home Assistant alarm_control_panel over MQTT (set/alarm).
 *
 * Cores: the state machine is in memory under the spinlock `ak`; arm / disarm can run on core 1 (web, MQTT:
 * the code check and the state change are memory only, the siren pin is a GPIO write); everything that needs
 * FAT, the sound or the camera is left in `fx` / queues for core 0 (alarm_poll from con_trygetc).
 */
#include "kernel.h"
#include "sd.h"

#define MAXZ 16
enum { Z_INSTANT, Z_DELAY, Z_INTERIOR, Z_24H };
enum { DISARMED, EXIT_TIME, ARMED, ENTRY_TIME, ALARM };
static const char *const state_name[] = { "disarmed", "exit", "armed", "entry", "alarm" };
static const char *const type_name[] = { "instant", "delay", "interior", "24h" };

struct zone {
    int pin, type, nc;
    char name[24];
    int open, raw, opened, skip;            /* skip = open when re-armed: ignored until it closes */
    u64 raw_since;
};

/* settings */
static int on, exit_s = 30, entry_s = 20, siren_s = 180, siren_pin = 24, armed_pin = 23, sound = 1, volume = 80;
static int photo = 1, photos = 3, video_s = 10, entry_photo = 1, use_mqtt = 1;
static char code[16] = "1234";
static char voice[48] = "/ALARM";
static struct zone zones[MAXZ];
static int nz, loaded;
#define MAXCARD 16
static struct { char uid[24], name[24]; } cards[MAXCARD];
static int ncards;

/* state */
static volatile int state = DISARMED, amode;              /* amode 0 away, 1 home */
static u64 state_t;                                       /* timer_us when the state began */
static int siren_on, cause = -1, memory = -1, wrong;
static u64 locked_until;
static char cause_name[24], memory_name[24], last_photo[64], last_video[64];
static int ak;                                            /* spinlock */
static void lock(void)   { while (__atomic_test_and_set(&ak, __ATOMIC_ACQUIRE)) ; }
static void unlock(void) { __atomic_clear(&ak, __ATOMIC_RELEASE); }

/* work for core 0 */
#define FX_SAVE_ST   1                                    /* write /ALARM.ST */
#define FX_STOP_SND  2
#define FX_ALARM     4                                    /* photos + video + MQTT event */
#define FX_ENTRY     8                                    /* entry photo */
#define FX_SIREN_TST 16
static volatile int fx;
static char sayq[4][12];                                  /* voice files to play */
static volatile int sq_h, sq_t;

/* MQTT (core 1 sends) */
static volatile int mq_dirty = 1;
static char evq[4][200];
static volatile int eq_h, eq_t;

/* event log */
#define NLOG 48
static struct { char t[20]; char e[76]; } lg[NLOG];
static int lg_n, lg_h;                                    /* count, next slot */
static volatile int lg_dirty;
static u64 lg_save_at;

/* web config waiting for core 0 */
static char pend[6144];
static volatile int pend_on;

static void scopy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static int to_int(const char *s, int *v) { int x = 0, k = 0; while (s[k] >= '0' && s[k] <= '9') { x = x * 10 + (s[k] - '0'); k++; } if (!k || s[k]) return -1; *v = x; return 0; }
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static void say(const char *name)                         /* any core: queue a voice file */
{
    int h = sq_h;
    if ((h + 1) % 4 == sq_t) return;
    scopy(sayq[h], name, sizeof(sayq[h]));
    __atomic_store_n(&sq_h, (h + 1) % 4, __ATOMIC_RELEASE);
}

/* one line in the log + on the console; caller holds the lock (or none needed on core 0 before core 1) */
static void event(const char *a, const char *b)
{
    char t[24];
    u64 now = clock_now();
    if (now) clock_format(t, now, 0);
    else { u32 s = (u32)(timer_us() / 1000000); int n = 0; t[n++] = '+';
           char d[12]; int k = 0; do d[k++] = (char)('0' + s % 10); while ((s /= 10)); while (k) t[n++] = d[--k];
           t[n++] = 's'; t[n] = 0; }
    char e[76]; int n = 0;
    for (const char *s = a; *s && n < 75; s++) e[n++] = *s;
    if (b && *b && n < 74) { e[n++] = ' '; for (const char *s = b; *s && n < 75; s++) e[n++] = *s; }
    e[n] = 0;
    scopy(lg[lg_h].t, t, sizeof(lg[lg_h].t));
    scopy(lg[lg_h].e, e, sizeof(lg[lg_h].e));
    lg_h = (lg_h + 1) % NLOG;
    if (lg_n < NLOG) lg_n++;
    lg_dirty = 1;
    con_color(state == ALARM ? COL_RED : state == DISARMED ? COL_GREEN : COL_YELLOW);
    kprintf("\n[ALARM %s] %s\n", t, e);
    con_color(COL_DEFAULT);
    mq_dirty = 1;
    display_poke();
}

static void mqtt_event(const char *ev, const char *zone, const char *file)   /* queue <base>/alarm/event */
{
    int h = eq_h;
    if ((h + 1) % 4 == eq_t) return;
    char *b = evq[h]; int n = 0;
    #define EA(s) do { const char *s_ = (s); while (*s_ && n < 196) { if (*s_ == '"' || *s_ == '\\') b[n++] = '\\'; b[n++] = *s_++; } } while (0)
    #define ER(s) do { const char *s_ = (s); while (*s_ && n < 196) b[n++] = *s_++; } while (0)
    ER("{\"event_type\":\""); EA(ev); ER("\"");
    if (zone && *zone) { ER(",\"zone\":\""); EA(zone); ER("\""); }
    if (file && *file) { ER(",\"photo\":\""); EA(file); ER("\""); }
    ER("}");
    b[n] = 0;
    #undef EA
    #undef ER
    __atomic_store_n(&eq_h, (h + 1) % 4, __ATOMIC_RELEASE);
}

/* ---------------- settings ---------------- */

static int cfg_text(char *b, int max, int with_code)
{
    int n = 0;
    #define P(s) do { const char *s_ = (s); while (*s_ && n < max - 1) b[n++] = *s_++; } while (0)
    #define PI(v) do { char d_[12]; int k_ = 0, x_ = (v); if (x_ < 0) { P("none"); break; } do d_[k_++] = (char)('0' + x_ % 10); while ((x_ /= 10)); while (k_ && n < max - 1) b[n++] = d_[--k_]; } while (0)
    P("# RoboOS alarm (shell 'alarm', web page /alarm)\r\n");
    P("state "); P(on ? "on" : "off"); P("\r\n");
    if (with_code == 1) { P("code "); P(code); P("\r\n"); }   /* 1 = the file, -2 = admin web (cards, no code), 0 = view */
    P("exit "); PI(exit_s); P("\r\nentry "); PI(entry_s); P("\r\nsiren "); PI(siren_s);
    P("\r\nsiren_pin "); PI(siren_pin); P("\r\narmed_pin "); PI(armed_pin);
    P("\r\nsound "); P(sound ? "on" : "off"); P("\r\nvolume "); PI(volume);
    P("\r\nvoice "); P(voice[0] ? voice : "none");
    P("\r\nphoto "); P(photo ? "on" : "off"); P("\r\nphotos "); PI(photos); P("\r\nvideo "); PI(video_s);
    P("\r\nentry_photo "); P(entry_photo ? "on" : "off"); P("\r\nmqtt "); P(use_mqtt ? "on" : "off");
    P("\r\n# zone <gpio> <instant|delay|interior|24h> <nc|no> <name>\r\n");
    for (int i = 0; i < nz; i++) {
        P("zone "); PI(zones[i].pin); P(" "); P(type_name[zones[i].type]); P(zones[i].nc ? " nc " : " no "); P(zones[i].name); P("\r\n");
    }
    { char a[160]; access_text(a, sizeof(a)); if (a[0]) { P("# keypad gpio <4 rows> <3-4 cols> | keypad i2c [addr] [swap] | rfid rc522 [rst] | rfid wiegand <d0> <d1>\r\n"); P(a); } }
    if (with_code > 0 || with_code == -2) for (int i = 0; i < ncards; i++) { P("card "); P(cards[i].uid); P(" "); P(cards[i].name); P("\r\n"); }
    b[n] = 0;
    #undef P
    #undef PI
    return n;
}

static int pin_ok(int p) { return p >= 2 && p <= 27; }

/* parse a settings text into the globals; err = the first problem (0 = none). Zones are replaced. */
static void parse(char *p, int keep_code, char *err, int emax)
{
    struct zone nzs[MAXZ];
    int nn = 0, nc = 0;
    static struct { char uid[24], name[24]; } ncs[MAXCARD];
    char *off1[2] = { "keypad", "off" }, *off2[2] = { "rfid", "off" };
    access_set(2, off1); access_set(2, off2);             /* the readers come from the text too */
    if (err) err[0] = 0;
    while (*p) {
        char *w[5]; int k = 0;
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        char *q = line;
        while (*q && k < 5) {
            while (is_space(*q)) *q++ = 0;
            if (!*q || *q == '#') break;
            w[k++] = q;
            if (k == 5) break;
            while (*q && !is_space(*q)) q++;
        }
        if (!k) continue;
        if (k == 5) { char *e = w[4] + strlen(w[4]); while (e > w[4] && is_space(e[-1])) *--e = 0; }
        for (int i = 1; i < k && i < 4; i++) { char *e = w[i]; while (*e && !is_space(*e)) e++; *e = 0; }
        int v = 0, num = k > 1 && !to_int(w[1], &v), none = k > 1 && !strcmp(w[1], "none");
        const char *x = w[0];
        #define BAD(m) do { if (err && !err[0]) { scopy(err, m, emax); int l_ = (int)strlen(err); scopy(err + l_, x, emax - l_); } } while (0)
        if (!strcmp(x, "state") && k > 1) on = !strcmp(w[1], "on");
        else if (!strcmp(x, "code") && k > 1) { if (!keep_code && strlen(w[1]) >= 4) scopy(code, w[1], sizeof(code)); else if (!keep_code) BAD("code: at least 4 characters - "); }
        else if (!strcmp(x, "exit") && num && v <= 600) exit_s = v;
        else if (!strcmp(x, "entry") && num && v <= 600) entry_s = v;
        else if (!strcmp(x, "siren") && num && v <= 3600) siren_s = v;
        else if (!strcmp(x, "siren_pin") && (none || (num && pin_ok(v)))) siren_pin = none ? -1 : v;
        else if (!strcmp(x, "armed_pin") && (none || (num && pin_ok(v)))) armed_pin = none ? -1 : v;
        else if ((!strcmp(x, "sound") || !strcmp(x, "beep")) && k > 1) sound = !strcmp(w[1], "on");
        else if (!strcmp(x, "volume") && num && v <= 100) volume = v;
        else if (!strcmp(x, "voice") && k > 1) scopy(voice, none ? "" : w[1], sizeof(voice));
        else if (!strcmp(x, "photo") && k > 1) photo = !strcmp(w[1], "on");
        else if (!strcmp(x, "photos") && num && v >= 1 && v <= 10) photos = v;
        else if (!strcmp(x, "video") && num && v <= 120) video_s = v;
        else if (!strcmp(x, "entry_photo") && k > 1) entry_photo = !strcmp(w[1], "on");
        else if (!strcmp(x, "mqtt") && k > 1) use_mqtt = !strcmp(w[1], "on");
        else if (!strcmp(x, "port")) ;                    /* the demo program's TCP port - not used here */
        else if (!strcmp(x, "keypad") || !strcmp(x, "rfid")) {
            char *ww[12]; int kk = 0;
            for (int i = 0; i < k && i < 4; i++) ww[kk++] = w[i];
            if (k == 5) for (char *q2 = w[4]; *q2 && kk < 12; ) {   /* the rest of the line: more words */
                while (is_space(*q2)) *q2++ = 0;
                if (!*q2) break;
                ww[kk++] = q2;
                while (*q2 && !is_space(*q2)) q2++;
            }
            if (access_set(kk, ww)) BAD("keypad / rfid: see 'alarm help' - ");
        }
        else if (!strcmp(x, "card") && k >= 2) {
            int l = (int)strlen(w[1]), okh = l >= 4 && l <= 20;
            for (int i = 0; i < l; i++) { char c = w[1][i]; if (c >= 'A' && c <= 'F') w[1][i] = (char)(c + 32); else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) okh = 0; }
            if (!okh) { BAD("card: the number in hex - "); continue; }
            if (nc >= MAXCARD) { BAD("too many cards (16) - "); continue; }
            scopy(ncs[nc].uid, w[1], sizeof(ncs[nc].uid));
            char nm[48] = "";
            if (k >= 3) { scopy(nm, w[2], sizeof(nm)); for (int i = 3; i < k; i++) { int l2 = (int)strlen(nm); if (l2 < 46) { nm[l2] = ' '; scopy(nm + l2 + 1, w[i], (int)sizeof(nm) - l2 - 1); } } }
            scopy(ncs[nc].name, nm[0] ? nm : "card", sizeof(ncs[nc].name));
            for (char *c = ncs[nc].name; *c; c++) if ((u8)*c < 32 || (u8)*c > 126 || *c == '"' || *c == '\\') *c = '_';
            nc++;
        }
        else if (!strcmp(x, "zone") && k >= 4) {
            int pin;
            if (to_int(w[1], &pin) || !pin_ok(pin)) { BAD("zone: GPIO 2-27 - "); continue; }
            if (nn >= MAXZ) { BAD("too many zones (16) - "); continue; }
            int dup = 0; for (int i = 0; i < nn; i++) if (nzs[i].pin == pin) dup = 1;
            if (dup) { BAD("zone: the same GPIO twice - "); continue; }
            struct zone *z = &nzs[nn];
            memset(z, 0, sizeof(*z));
            z->pin = pin;
            z->type = !strcmp(w[2], "delay") ? Z_DELAY : !strcmp(w[2], "24h") ? Z_24H : !strcmp(w[2], "interior") ? Z_INTERIOR : Z_INSTANT;
            z->nc = strcmp(w[3], "no") != 0;
            scopy(z->name, k == 5 && w[4][0] ? w[4] : "zone", sizeof(z->name));
            for (char *c = z->name; *c; c++) if ((u8)*c < 32 || (u8)*c > 126 || *c == '"' || *c == '\\') *c = '_';
            nn++;
        } else BAD("not understood: ");
        #undef BAD
    }
    for (int i = 0; i < nn; i++) {
        /* keep the debounced state of a zone that stays on the same pin */
        for (int j = 0; j < nz; j++) if (zones[j].pin == nzs[i].pin && zones[j].nc == nzs[i].nc) {
            nzs[i].open = zones[j].open; nzs[i].raw = zones[j].raw; nzs[i].raw_since = zones[j].raw_since; nzs[i].skip = zones[j].skip; }
    }
    memcpy(zones, nzs, sizeof(nzs));
    nz = nn;
    for (int i = 0; i < nc; i++) memcpy(&cards[i], &ncs[i], sizeof(cards[i]));
    ncards = nc;
    if (siren_pin >= 0 && siren_pin == armed_pin) armed_pin = -1;
}

static void setup_pins(void)
{
    if (siren_pin >= 0) { gpio_func(siren_pin, 1); gpio_set(siren_pin, siren_on); }
    if (armed_pin >= 0) { gpio_func(armed_pin, 1); gpio_set(armed_pin, 0); }
    for (int i = 0; i < nz; i++) {
        struct zone *z = &zones[i];
        if (z->pin == siren_pin || z->pin == armed_pin) continue;
        gpio_func(z->pin, 0);
        gpio_pull(z->pin, 2);
    }
    delay_us(2000);
    for (int i = 0; i < nz; i++) {
        struct zone *z = &zones[i];
        int lv = gpio_get(z->pin);
        int t = z->nc ? lv : !lv;
        if (!z->raw_since) { z->raw = z->open = t; z->raw_since = timer_us(); }
    }
    access_setup();
}

static int save_cfg(void)
{
    static char b[2048];
    int n = cfg_text(b, sizeof(b), 1);
    return !fat_mounted() || fat_write_file("/ALARM.TXT", b, (u32)n);
}

static void save_state(void)
{
    const char *s = (state == DISARMED || !on) ? "off\r\n" : amode ? "home\r\n" : "away\r\n";
    if (fat_mounted()) fat_write_file("/ALARM.ST", s, (u32)strlen(s));
}

static void load(void)
{
    loaded = 1;
    static char b[2048];
    struct fat_file f;
    if (!fat_mounted() || fat_open("/ALARM.TXT", &f)) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    char e[80];
    parse(b, 0, e, sizeof(e));
    if (e[0]) kprintf("/ALARM.TXT: %s\n", e);
}

static void log_load(void)
{
    static char b[NLOG * 100];
    struct fat_file f;
    if (!fat_mounted() || fat_open("/ALARM.LOG", &f)) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    for (char *p = b; *p; ) {
        char *l = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        int ll = (int)strlen(l);
        while (ll && (l[ll - 1] == '\r')) l[--ll] = 0;
        if (ll < 21 || l[19] != ' ') continue;            /* "YYYY-MM-DD HH:MM:SS text" */
        l[19] = 0;
        scopy(lg[lg_h].t, l, sizeof(lg[lg_h].t));
        scopy(lg[lg_h].e, l + 20, sizeof(lg[lg_h].e));
        lg_h = (lg_h + 1) % NLOG;
        if (lg_n < NLOG) lg_n++;
    }
}

static void log_save(void)
{
    static char b[NLOG * 100];
    int n = 0;
    lock();
    for (int i = 0; i < lg_n; i++) {
        int j = (lg_h - lg_n + i + NLOG) % NLOG;
        const char *t = lg[j].t;
        char tb[20]; int tl = (int)strlen(t);
        if (tl != 19) { scopy(tb, "0000-00-00 00:00:00", 20); t = tb; }   /* no clock: keep the line shape */
        for (const char *s = t; *s; s++) b[n++] = *s;
        b[n++] = ' ';
        for (const char *s = lg[j].e; *s; s++) b[n++] = *s;
        b[n++] = '\r'; b[n++] = '\n';
    }
    lg_dirty = 0;
    unlock();
    if (fat_mounted()) fat_write_file("/ALARM.LOG", b, (u32)n);
}

/* ---------------- state changes (any core, under the lock) ---------------- */

static void siren(int s)
{
    if (siren_on == s) return;
    siren_on = s;
    if (siren_pin >= 0) gpio_set(siren_pin, s);
    if (!s) fx |= FX_STOP_SND;
}

static void set_state(int s) { state = s; state_t = timer_us(); mq_dirty = 1; }

static void start_alarm(int zi, const char *why)
{
    cause = zi;
    scopy(cause_name, zi >= 0 ? zones[zi].name : why, sizeof(cause_name));
    scopy(memory_name, cause_name, sizeof(memory_name));
    memory = 1;
    set_state(ALARM);
    siren(1);
    event("ALARM!", cause_name);
    say("ALARM");
    fx |= FX_ALARM;
}

static int code_ok(const char *c)                        /* the same time for a right and a wrong code */
{
    int a = (int)strlen(c), b = (int)strlen(code), d = a ^ b;
    for (int i = 0; i < b; i++) d |= (i < a ? c[i] : 0) ^ code[i];
    return d == 0;
}

static const char card_ok[] = "\001card";              /* act(): a known card instead of the code */

/* what: 1 arm, 2 disarm, 3 panic; mode 0 away / 1 home; 1 = done, 0 = refused (msg says why) */
static int act(int what, int mode, const char *c, const char *who, char *msg, int mmax)
{
    lock();
    int r = 0;
    if (!on) { scopy(msg, "the alarm is off ('alarm on' / the web page)", mmax); goto out; }
    if (what == 3) {
        if (state == ALARM) { scopy(msg, "already in alarm", mmax); goto out; }
        start_alarm(-1, "panic");
        event("panic from", who);
        scopy(msg, "ALARM (panic)", mmax); r = 1; goto out;
    }
    if (c != card_ok && locked_until && timer_us() < locked_until) { scopy(msg, "code blocked for a while (too many wrong codes)", mmax); goto out; }
    if (c != card_ok && !code_ok(c ? c : "")) {
        scopy(msg, "wrong code", mmax);
        event("wrong code from", who);
        say("WRONG");
        if (++wrong >= 5) { locked_until = timer_us() + 60000000ULL; wrong = 0; event("code blocked for 60 s", ""); }
        goto out;
    }
    wrong = 0;
    if (what == 1) {
        if (state != DISARMED) { scopy(msg, "already armed", mmax); goto out; }
        for (int i = 0; i < nz; i++) {
            struct zone *z = &zones[i];
            if (!z->open || z->type != Z_INSTANT) continue;   /* delay / interior: the way out; 24h: always watched anyway */
            scopy(msg, "cannot arm, open: ", mmax);
            int l = (int)strlen(msg); scopy(msg + l, z->name, mmax - l);
            event(msg, "");
            say("NOARM");
            goto out;
        }
        amode = mode ? 1 : 0;
        for (int i = 0; i < nz; i++) zones[i].skip = 0;
        memory = -1; memory_name[0] = 0; cause = -1;
        set_state(exit_s > 0 ? EXIT_TIME : ARMED);
        event(exit_s > 0 ? (amode ? "arming (home), exit time, by" : "arming, exit time, by") : (amode ? "ARMED (home) by" : "ARMED by"), who);
        say(exit_s > 0 ? "ARMING" : "ARMED");
        scopy(msg, exit_s > 0 ? "exit time started" : "armed", mmax);
        fx |= FX_SAVE_ST;
        r = 1;
    } else {
        if (state == DISARMED && !siren_on) { scopy(msg, "already disarmed", mmax); r = 1; goto out; }
        siren(0);
        set_state(DISARMED);
        cause = -1;
        event("DISARMED by", who);
        say("DISARMED");
        scopy(msg, memory_name[0] ? "disarmed - there WAS an alarm: " : "disarmed", mmax);
        if (memory_name[0]) { int l = (int)strlen(msg); scopy(msg + l, memory_name, mmax - l); }
        fx |= FX_SAVE_ST;
        r = 1;
    }
out:
    unlock();
    return r;
}

/* ---------------- keypad + cards (0.121, core 0) ---------------- */

static char kbuf[16];                                     /* the code being typed */
static int klen;
static u64 kat, kmsg_until, learn_until;
static char kmsg[40];
static int cfg_dirty;

static void kmsg_set(const char *m) { scopy(kmsg, m, sizeof(kmsg)); kmsg_until = timer_us() + 4000000; display_poke(); }

static void key_in(char k, const char *who)
{
    u64 t = timer_us();
    if (klen && t - kat > 10000000) klen = 0;             /* 10 s without a key: start again */
    kat = t;
    if (sound && !sound_is_exclusive()) sound_tone(1800, 40);
    char msg[80];
    int r = -1;
    if (k >= '0' && k <= '9') { if (klen < 15) kbuf[klen++] = k; kbuf[klen] = 0; display_poke(); return; }
    if (k == '*' || k == 'C') { klen = 0; kmsg_set("cleared"); return; }
    if (!klen) { kmsg_set(k == '#' ? "type the code, then #" : "type the code first"); return; }
    kbuf[klen] = 0;
    if (k == '#') r = act(state == DISARMED ? 1 : 2, 0, kbuf, who, msg, sizeof(msg));
    else if (k == 'A') r = act(1, 0, kbuf, who, msg, sizeof(msg));
    else if (k == 'B') r = act(1, 1, kbuf, who, msg, sizeof(msg));
    else if (k == 'D') r = act(2, 0, kbuf, who, msg, sizeof(msg));
    for (int i = 0; i < 16; i++) kbuf[i] = 0;
    klen = 0;
    if (r >= 0) kmsg_set(msg);
}

static void card_in(const char *uid0, const char *src)
{
    char msg[80], who[48], uid[24];
    int ul = 0;
    for (; uid0[ul] && ul < 23; ul++) { char c = uid0[ul]; uid[ul] = c >= 'A' && c <= 'F' ? (char)(c + 32) : c; }
    uid[ul] = 0;
    int k = -1;
    for (int i = 0; i < ncards; i++) if (!strcmp(cards[i].uid, uid)) k = i;
    if (learn_until && timer_us() < learn_until) {
        learn_until = 0;
        if (k >= 0) { kmsg_set("card already known"); kprintf("\n[ALARM] card %s is already known (%s)\n", uid, cards[k].name); return; }
        if (ncards >= MAXCARD) { kmsg_set("too many cards"); return; }
        lock();
        scopy(cards[ncards].uid, uid, sizeof(cards[0].uid));
        char nm[24] = "card "; int l = 5; int v = ncards + 1; if (v >= 10) nm[l++] = (char)('0' + v / 10); nm[l++] = (char)('0' + v % 10); nm[l] = 0;
        scopy(cards[ncards].name, nm, sizeof(cards[0].name));
        ncards++;
        event("card learned:", uid);
        unlock();
        cfg_dirty = 1;
        kmsg_set("card learned");
        if (sound) sound_tone(2400, 300);
        return;
    }
    if (k < 0) {
        lock();
        scopy(who, "unknown card ", sizeof(who)); int l = (int)strlen(who); scopy(who + l, uid, (int)sizeof(who) - l);
        event(who, src);
        say("WRONG");
        if (++wrong >= 5) { locked_until = timer_us() + 60000000ULL; wrong = 0; event("code blocked for 60 s", ""); }
        unlock();
        kmsg_set("unknown card");
        return;
    }
    scopy(who, cards[k].name, sizeof(who));
    act(state == DISARMED ? 1 : 2, 0, card_ok, who, msg, sizeof(msg));
    kmsg_set(msg);
}

static void readers(void)
{
    char key, uid[24];
    for (int i = 0; i < 4; i++) {
        int e = access_poll(&key, uid, sizeof(uid));
        if (!e) break;
        if (e == 1) key_in(key, "keypad");
        else card_in(uid, "");
    }
    if (cfg_dirty) { cfg_dirty = 0; if (save_cfg()) kprintf("[ALARM] /ALARM.TXT not saved (no SD card?)\n"); }
}

int alarm_is_on(void) { return on; }
const char *alarm_state_text(void)                        /* 0.128: the home page */
{
    if (!on) return "off";
    return state == DISARMED ? "disarmed" : state == EXIT_TIME ? "arming" : state == ENTRY_TIME ? "entry time" :
           state == ALARM ? "ALARM" : amode ? "armed (home)" : "armed";
}

/* the display's alarm screen (display.c, core 0): up to 4 rows of `cols` characters */
int alarm_display(char l[4][40], int cols)
{
    u64 t = timer_us();
    for (int i = 0; i < 4; i++) l[i][0] = 0;
    if (!on) { scopy(l[0], "alarm off", 40); return 1; }
    char b[48];
    int left = (state == EXIT_TIME ? exit_s : entry_s) - (int)((t - state_t) / 1000000);
    if (left < 0) left = 0;
    if (state == EXIT_TIME || state == ENTRY_TIME) {
        scopy(b, state == EXIT_TIME ? "EXIT " : "ENTRY ", sizeof(b));
        int n = (int)strlen(b); if (left >= 100) b[n++] = (char)('0' + left / 100); if (left >= 10) b[n++] = (char)('0' + left / 10 % 10);
        b[n++] = (char)('0' + left % 10); b[n++] = ' '; b[n++] = 's'; b[n] = 0;
    } else scopy(b, state == DISARMED ? "DISARMED" : state == ALARM ? "!! ALARM !!" : amode ? "ARMED home" : "ARMED", sizeof(b));
    scopy(l[0], b, cols + 1);
    if (klen && t - kat < 10000000) { scopy(b, "Code: ", sizeof(b)); int n = 6; for (int i = 0; i < klen && n < 40; i++) b[n++] = '*'; b[n] = 0; }
    else if (kmsg_until > t) scopy(b, kmsg, sizeof(b));
    else if ((state == ALARM || state == ENTRY_TIME) && cause_name[0]) scopy(b, cause_name, sizeof(b));
    else if (learn_until > t) scopy(b, "show the new card", sizeof(b));
    else {
        int o = 0, k = -1; for (int i = 0; i < nz; i++) if (zones[i].open && !zones[i].skip) { o++; if (k < 0) k = i; }
        if (o == 1) { scopy(b, zones[k].name, sizeof(b)); int n = (int)strlen(b); scopy(b + n, " open", (int)sizeof(b) - n); }
        else if (o) { scopy(b, "open zones: ", sizeof(b)); int n = (int)strlen(b); b[n++] = (char)('0' + o / 10 % 10); b[n++] = (char)('0' + o % 10); b[n] = 0; if (b[n - 2] == '0') { b[n - 2] = b[n - 1]; b[n - 1] = 0; } }
        else scopy(b, memory_name[0] ? "was: " : "all closed", sizeof(b));
        if (!o && memory_name[0]) { int n = (int)strlen(b); scopy(b + n, memory_name, (int)sizeof(b) - n); }
    }
    scopy(l[1], b, cols + 1);
    u64 now = clock_now();
    if (now) { char tb[32]; clock_format(tb, now, 0); scopy(l[2], tb + 11, cols + 1); tb[10] = 0; scopy(l[3], tb, cols + 1); return 4; }
    return 2;
}

/* ---------------- the loop (core 0) ---------------- */

static void read_zones(void)
{
    u64 t = timer_us();
    for (int i = 0; i < nz; i++) {
        struct zone *z = &zones[i];
        int lv = gpio_get(z->pin);
        int trig = z->nc ? lv : !lv;
        if (trig != z->raw) { z->raw = trig; z->raw_since = t; }
        if (z->open != z->raw && t - z->raw_since >= 50000) {
            z->open = z->raw;
            if (z->open) z->opened = 1; else z->skip = 0;
            lock();
            event(z->name, z->open ? "open" : "closed");
            unlock();
        }
    }
}

static void run_states(void)
{
    u64 t = timer_us();
    lock();
    for (int i = 0; i < nz; i++) {
        struct zone *z = &zones[i];
        int fresh = z->opened;
        z->opened = 0;
        if (!z->open || z->skip) continue;
        if (z->type == Z_24H) { if (fresh && state != ALARM) start_alarm(i, 0); continue; }
        if (state == ARMED) {
            if (z->type == Z_INTERIOR && amode) continue;  /* home: you are inside */
            if (z->type == Z_DELAY) {
                cause = i;
                scopy(cause_name, z->name, sizeof(cause_name));
                set_state(ENTRY_TIME);
                event("entry time started:", z->name);
                say("ENTRY");
                fx |= FX_ENTRY;
                if (entry_s == 0) start_alarm(i, 0);
            } else start_alarm(i, 0);
            break;
        }
        if (state == ENTRY_TIME && z->type == Z_INSTANT) { start_alarm(i, 0); break; }
    }
    t = timer_us();                                       /* (start_alarm above set a newer state_t) */
    if (state == EXIT_TIME && t - state_t >= (u64)exit_s * 1000000) {
        set_state(ARMED);
        event(amode ? "ARMED (home)" : "ARMED", "");
        say("ARMED");
    } else if (state == ENTRY_TIME && t - state_t >= (u64)entry_s * 1000000) start_alarm(cause, "entry time over");
    else if (state == ALARM && siren_s > 0 && t - state_t >= (u64)siren_s * 1000000) {
        siren(0);
        for (int i = 0; i < nz; i++) zones[i].skip = zones[i].open;   /* a door left open does not re-trigger */
        set_state(ARMED);
        event("siren time over - armed again (alarm memory kept)", "");
    }
    unlock();
}

static void lights(void)
{
    if (armed_pin < 0) return;
    u64 ms = timer_us() / 1000;
    int s = state == DISARMED ? 0 : state == ARMED ? 1 : state == ALARM ? (int)((ms / 125) & 1) : (int)((ms / 500) & 1);
    static int last = -1;
    if (s != last) { gpio_set(armed_pin, s); last = s; }
}

static int no_idle(void) { return 0; }

static int play_voice(const char *name)
{
    if (!sound || !voice[0]) return -1;
    char p[72]; int n = 0;
    for (const char *s = voice; *s && n < 48; s++) p[n++] = *s;
    p[n++] = '/';
    for (const char *s = name; *s && n < 64; s++) p[n++] = *s;
    for (const char *s = ".WAV"; *s; s++) p[n++] = *s;
    p[n] = 0;
    struct fat_file f;
    if (fat_open(p, &f)) return -1;
    return sound_wav(p, no_idle) < 0 ? -1 : 0;
}

static int last_left = -1, wailing;
static u64 last_wail;

static void sounds(void)
{
    while (sq_t != sq_h) {                                /* voice files */
        char nm[12]; scopy(nm, sayq[sq_t], sizeof(nm));
        __atomic_store_n(&sq_t, (sq_t + 1) % 4, __ATOMIC_RELEASE);
        if (sound && !sound_is_exclusive()) { wailing = 0; play_voice(nm); }
    }
    if (!sound || sound_is_exclusive()) return;
    if (state == EXIT_TIME || state == ENTRY_TIME) {      /* a beep a second, the last 10 spoken */
        int total = state == EXIT_TIME ? exit_s : entry_s;
        int left = total - (int)((timer_us() - state_t) / 1000000);
        if (left != last_left && left > 0) {
            last_left = left;
            char nm[4] = { 0 };
            if (left == 10) { nm[0] = '1'; nm[1] = '0'; } else nm[0] = (char)('0' + left);
            if (left > 10 || play_voice(nm)) sound_tone(state == ENTRY_TIME ? 2500 : 2000, state == ENTRY_TIME ? 150 : 80);
        }
    } else last_left = -1;
    if (siren_on && state == ALARM) {                     /* 600 -> 1200 -> 600 Hz every 2 s */
        u64 ms = timer_us() / 1000;
        if (wailing && ms - last_wail < 20) return;
        last_wail = ms;
        int ph = (int)(ms % 2000);
        sound_tone(ph < 1000 ? 600 + ph * 600 / 1000 : 1200 - (ph - 1000) * 600 / 1000, 0);
        wailing = 1;
    } else if (wailing) { sound_stop(); wailing = 0; }
}

static int ph_left, clip_pending, busy;
static u64 ph_next;

static void camera_work(void)
{
    if (ph_left && timer_us() >= ph_next) {
        int first = ph_left == photos;
        int send = first && use_mqtt && mqtt_connected();
        char nm[64] = "";
        int r = camera_take(photo, send, nm);
        ph_left--;
        ph_next = timer_us() + 1000000;
        lock();
        if (r == 0 && nm[0] && photo) { scopy(last_photo, nm, sizeof(last_photo)); event("photo", nm); }
        else if (r) { event("photo:", camera_take_error(r)); if (r == -1) ph_left = 0; }
        if (first && use_mqtt) mqtt_event("alarm", cause_name, r == 0 ? nm : "");
        unlock();
        if (!ph_left && clip_pending) {
            clip_pending = 0;
            int c = camera_clip_start(video_s, last_video);
            if (c) { lock(); event("video:", c == -4 ? "the camera is busy (microscope / cam live)" : "not started"); unlock(); }
        }
    }
}

/* core 0, from con_trygetc */
void alarm_poll(void)
{
    if (busy || io_held()) return;
    if (pend_on) {                                        /* settings from the web page */
        busy = 1;
        char e[80];
        lock();
        if (state != DISARMED && on) e[0] = 0, kprintf("\n[ALARM] web settings ignored: disarm first\n");
        else {
            parse(pend, 1, e, sizeof(e));
            event("settings changed from the web", e[0] ? e : "");
        }
        pend_on = 0;
        unlock();
        setup_pins();
        if (save_cfg()) kprintf("[ALARM] /ALARM.TXT not saved (no SD card?)\n");
        sound_volume(volume);
        mq_dirty = 1;
        mqtt_rediscover();
        busy = 0;
    }
    if (!on) return;
    static u64 next;
    u64 t = timer_us();
    if (t < next) return;
    next = t + 5000;
    busy = 1;
    read_zones();
    run_states();
    lights();
    int f = __atomic_exchange_n(&fx, 0, __ATOMIC_ACQ_REL);
    if (f & FX_STOP_SND) { if (wailing) { sound_stop(); wailing = 0; } }
    if (f & FX_SAVE_ST) save_state();
    if (f & FX_ALARM) {
        save_state();
        if (photo || use_mqtt) { ph_left = photos; ph_next = timer_us(); }
        else if (use_mqtt) mqtt_event("alarm", cause_name, "");
        clip_pending = video_s > 0;
        if (!ph_left && clip_pending) { clip_pending = 0; camera_clip_start(video_s, last_video); }
    }
    if ((f & FX_ENTRY) && entry_photo) {
        char nm[64] = "";
        int r = camera_take(photo, use_mqtt && mqtt_connected(), nm);
        lock();
        if (r == 0 && nm[0]) { scopy(last_photo, nm, sizeof(last_photo)); event("entry photo", nm); }
        else if (r && r != -1) event("entry photo:", camera_take_error(r));
        if (use_mqtt) mqtt_event("entry", cause_name, r == 0 ? nm : "");
        unlock();
    }
    if (f & FX_SIREN_TST) {
        if (siren_pin >= 0) gpio_set(siren_pin, 1);
        if (sound) sound_tone(900, 0);
        delay_us(2000000);
        if (siren_pin >= 0) gpio_set(siren_pin, siren_on);
        sound_stop();
    }
    if (access_active()) readers();
    camera_work();
    sounds();
    if (lg_dirty && !lg_save_at) lg_save_at = timer_us() + 2000000;
    if (lg_save_at && timer_us() >= lg_save_at) { lg_save_at = 0; log_save(); }
    busy = 0;
}

/* kernel_main, after fat_mount */
void alarm_boot(void)
{
    load();
    log_load();
    if (!on) return;
    setup_pins();
    sound_volume(volume);
    struct fat_file f;
    char b[8] = "";
    if (fat_mounted() && !fat_open("/ALARM.ST", &f)) { int n = fat_read(&f, b, 7); if (n < 0) n = 0; b[n] = 0; }
    if (!strncmp(b, "away", 4) || !strncmp(b, "home", 4)) {
        amode = b[0] == 'h';
        for (int i = 0; i < nz; i++) zones[i].skip = zones[i].open;
        set_state(ARMED);
        event(amode ? "ARMED (home) again after a restart" : "ARMED again after a restart", "");
    } else event("started, disarmed", "");
    kprintf("alarm: %s, %d zone%s ('alarm' or the web page /alarm)\n", state == DISARMED ? "disarmed" : "ARMED", nz, nz == 1 ? "" : "s");
}

int alarm_enabled(void) { return on && use_mqtt; }

/* for the Home Assistant discovery (mqtt.c): zone i -> pin, type (0 instant 1 delay 2 interior 3 24h), name; -1 = none */
int alarm_zone_info(int i, int *type, char *name, int nmax)
{
    lock();
    int p = -1;
    if (on && use_mqtt && i >= 0 && i < nz) { p = zones[i].pin; *type = zones[i].type; scopy(name, zones[i].name, nmax); }
    unlock();
    return p;
}

/* ---------------- web / MQTT (core 1) ---------------- */

static int jn, jmax;
static char *jb;
static void J(const char *s) { while (*s && jn < jmax - 1) jb[jn++] = *s++; jb[jn] = 0; }
static void Js(const char *s)
{
    J("\"");
    for (; *s; s++) {
        char c[3] = { *s, 0, 0 };
        if (*s == '"' || *s == '\\') { c[0] = '\\'; c[1] = *s; }
        else if (*s == '\r') continue;
        else if (*s == '\n') { c[0] = '\\'; c[1] = 'n'; }
        else if ((u8)*s < 32) continue;
        J(c);
    }
    J("\"");
}
static void Jn(int v) { char d[12]; int k = 0; if (v < 0) { J("-"); v = -v; } do d[k++] = (char)('0' + v % 10); while ((v /= 10)); while (k) { char c[2] = { d[--k], 0 }; J(c); } }

static const char *ha_state(void)
{
    return !on ? "disarmed" : state == DISARMED ? "disarmed" : state == EXIT_TIME ? "arming" : state == ENTRY_TIME ? "pending" :
           state == ALARM ? "triggered" : amode ? "armed_home" : "armed_away";
}

/* the status (+ settings without the code + the log) as JSON; any core */
int alarm_web_json(char *b, int max, int admin)
{
    jb = b; jmax = max; jn = 0;
    lock();
    u64 t = timer_us();
    J("{\"ok\":true,\"on\":"); J(on ? "true" : "false");
    J(",\"state\":"); Js(state_name[state]);
    J(",\"ha\":"); Js(ha_state());
    J(",\"mode\":"); Js(amode ? "home" : "away");
    J(",\"siren\":"); J(siren_on ? "true" : "false");
    if (state == EXIT_TIME || state == ENTRY_TIME) {
        int left = (state == EXIT_TIME ? exit_s : entry_s) - (int)((t - state_t) / 1000000);
        J(",\"left\":"); Jn(left < 0 ? 0 : left);
    }
    if (cause_name[0] && (state == ALARM || state == ENTRY_TIME)) { J(",\"cause\":"); Js(cause_name); }
    if (memory_name[0]) { J(",\"memory\":"); Js(memory_name); }
    if (locked_until && t < locked_until) { J(",\"locked\":"); Jn((int)((locked_until - t) / 1000000)); }
    J(",\"last_photo\":"); Js(last_photo);
    J(",\"last_video\":"); Js(last_video);
    J(",\"zones\":[");
    for (int i = 0; i < nz; i++) {
        struct zone *z = &zones[i];
        if (i) J(",");
        J("{\"pin\":"); Jn(z->pin); J(",\"name\":"); Js(z->name); J(",\"type\":"); Js(type_name[z->type]);
        J(",\"nc\":"); J(z->nc ? "true" : "false"); J(",\"open\":"); J(z->open ? "true" : "false");
        if (z->skip) J(",\"skip\":true");
        J("}");
    }
    J("],\"log\":[");
    for (int i = 0; i < lg_n && i < 28; i++) {            /* newest first (28 = the answer stays < 8 KB) */
        int j = (lg_h - 1 - i + 2 * NLOG) % NLOG;
        if (i) J(",");
        J("["); Js(lg[j].t); J(","); Js(lg[j].e); J("]");
    }
    J("],\"config\":");
    static char c[3072];
    cfg_text(c, sizeof(c), admin ? -2 : 0);            /* cards only for an admin (a UID is enough to copy a card) */
    Js(c);
    J("}\n");
    unlock();
    return jn;
}

/* web / MQTT commands: cmd arm | disarm | panic | test; 1 ok, 0 refused */
int alarm_web_cmd(const char *cmd, const char *mode, const char *c, const char *who, char *msg, int mmax)
{
    if (!strcmp(cmd, "arm")) return act(1, mode && !strcmp(mode, "home"), c, who, msg, mmax);
    if (!strcmp(cmd, "disarm")) return act(2, 0, c, who, msg, mmax);
    if (!strcmp(cmd, "panic")) return act(3, 0, c, who, msg, mmax);
    if (!strcmp(cmd, "learn")) {
        if (!access_active()) { scopy(msg, "no card reader set up", mmax); return 0; }
        if (on && state != DISARMED) { scopy(msg, "disarm first", mmax); return 0; }
        learn_until = timer_us() + 30000000ULL; display_poke();
        scopy(msg, "show the new card to the reader within 30 s", mmax); return 1;
    }
    if (!strcmp(cmd, "test")) {
        if (state != DISARMED) { scopy(msg, "disarm first", mmax); return 0; }
        fx |= FX_SIREN_TST; scopy(msg, "siren + tone for 2 s", mmax); return 1;
    }
    scopy(msg, "cmd: arm (mode away|home), disarm, panic, test, learn", mmax);
    return 0;
}

/* core 1: new settings text from the web page; 0 queued, -1 busy, -2 armed */
int alarm_web_config(const char *text, int len)
{
    if (pend_on) return -1;
    if (on && state != DISARMED) return -2;
    if (len > (int)sizeof(pend) - 1) len = (int)sizeof(pend) - 1;
    memcpy(pend, text, (size_t)len);
    pend[len] = 0;
    __atomic_store_n(&pend_on, 1, __ATOMIC_RELEASE);
    return 0;
}

/* core 1, MQTT set/alarm: {"action":"ARM_AWAY","code":"1234"} (Home Assistant) or "DISARM 1234" */
void alarm_mqtt_cmd(const char *p, int pl)
{
    char a[16] = "", c[20] = "", buf[160];
    if (pl > (int)sizeof(buf) - 1) pl = (int)sizeof(buf) - 1;
    memcpy(buf, p, (size_t)pl); buf[pl] = 0;
    if (buf[0] == '{') {
        const char *keys[2] = { "\"action\"", "\"code\"" };
        char *outs[2] = { a, c }; int maxs[2] = { sizeof(a), sizeof(c) };
        for (int k = 0; k < 2; k++) {
            char *s = strstr(buf, keys[k]);
            if (!s) continue;
            s = strchr(s + strlen(keys[k]), ':');
            if (!s) continue;
            s++; while (*s == ' ') s++;
            if (*s == '"') s++;
            int n = 0; while (*s && *s != '"' && *s != ',' && *s != '}' && n < maxs[k] - 1) outs[k][n++] = *s++;
            outs[k][n] = 0;
        }
    } else {
        int n = 0; char *s = buf;
        while (*s && *s != ' ' && n < (int)sizeof(a) - 1) a[n++] = *s++;
        a[n] = 0; while (*s == ' ') s++;
        scopy(c, s, sizeof(c));
    }
    char msg[64];
    int r;
    if (!strcmp(a, "ARM_AWAY")) r = act(1, 0, c, "MQTT", msg, sizeof(msg));
    else if (!strcmp(a, "ARM_HOME")) r = act(1, 1, c, "MQTT", msg, sizeof(msg));
    else if (!strcmp(a, "DISARM")) r = act(2, 0, c, "MQTT", msg, sizeof(msg));
    else if (!strcmp(a, "TRIGGER")) r = act(3, 0, c, "MQTT", msg, sizeof(msg));
    else { kprintf("\n[ALARM] MQTT: unknown action '%s'\n", a); return; }
    if (!r) kprintf("\n[ALARM] MQTT %s: %s\n", a, msg);
    mq_dirty = 1;                                         /* HA shows the real state again */
}

void alarm_mqtt_resend(void) { mq_dirty = 1; }

/* core 1, mqtt_poll: the next message to publish (sub-topic, payload, retain); 0 = none */
int alarm_mqtt_next(char *sub, int smax, char *pl, int pmax, int *retain)
{
    if (!on || !use_mqtt) return 0;
    static int step, zstep;
    if (mq_dirty && step == 0) { mq_dirty = 0; step = 1; }
    if (step == 1) { step = 2; scopy(sub, "alarm", smax); scopy(pl, ha_state(), pmax); *retain = 1; return 1; }
    if (step == 2) {
        step = 3;
        scopy(sub, "alarm/attr", smax);
        jb = pl; jmax = pmax; jn = 0;
        lock();
        J("{\"state\":"); Js(state_name[state]);
        J(",\"cause\":"); Js(cause_name[0] && (state == ALARM || state == ENTRY_TIME) ? cause_name : "");
        J(",\"memory\":"); Js(memory_name);
        J(",\"last_photo\":"); Js(last_photo);
        J(",\"open\":[");
        int k = 0;
        for (int i = 0; i < nz; i++) if (zones[i].open) { if (k++) J(","); Js(zones[i].name); }
        J("]}");
        unlock();
        *retain = 1;
        zstep = 0;
        return 1;
    }
    if (step == 3) {                                      /* every zone: alarm/zone/<gpio> ON (open) / OFF */
        lock();
        if (zstep < nz) {
            char t[24] = "alarm/zone/"; int n = 11, p = zones[zstep].pin;
            if (p >= 10) t[n++] = (char)('0' + p / 10);
            t[n++] = (char)('0' + p % 10); t[n] = 0;
            scopy(sub, t, smax);
            scopy(pl, zones[zstep].open ? "ON" : "OFF", pmax);
            zstep++;
            unlock();
            *retain = 1;
            return 1;
        }
        unlock();
        step = 0;
    }
    if (eq_t != eq_h) {
        scopy(sub, "alarm/event", smax);
        scopy(pl, evq[eq_t], pmax);
        __atomic_store_n(&eq_t, (eq_t + 1) % 4, __ATOMIC_RELEASE);
        *retain = 0;
        return 1;
    }
    return 0;
}

/* ---------------- shell ---------------- */

static void show(void)
{
    u64 t = timer_us();
    con_color(state == ALARM ? COL_RED : state == DISARMED ? COL_GREEN : COL_YELLOW);
    kprintf("alarm %s: %s%s", on ? "ON" : "off", state_name[state], state == ARMED || state == EXIT_TIME ? (amode ? " (home)" : " (away)") : "");
    if (state == EXIT_TIME || state == ENTRY_TIME) {
        int left = (state == EXIT_TIME ? exit_s : entry_s) - (int)((t - state_t) / 1000000);
        kprintf(", %d s left", left < 0 ? 0 : left);
    }
    if (state == ALARM || state == ENTRY_TIME) kprintf(", cause: %s", cause_name);
    kprintf("%s\n", siren_on ? ", SIREN ON" : "");
    con_color(COL_DEFAULT);
    if (memory_name[0]) kprintf("  alarm memory: %s\n", memory_name);
    kprintf("  exit %d s, entry %d s, siren %d s on %s%d, LED %s%d, sound %s (volume %d, voice %s)\n", exit_s, entry_s, siren_s,
            siren_pin >= 0 ? "GPIO" : "", siren_pin, armed_pin >= 0 ? "GPIO" : "", armed_pin, sound ? "on" : "off", volume, voice[0] ? voice : "none");
    kprintf("  on alarm: %s%d photo%s, video %d s; entry photo %s; MQTT %s%s\n", photo ? "" : "(not saved) ", photos, photos == 1 ? "" : "s",
            video_s, entry_photo ? "on" : "off", use_mqtt ? "on" : "off", use_mqtt && !mqtt_connected() ? " (not connected now)" : "");
    for (int i = 0; i < nz; i++) {
        struct zone *z = &zones[i];
        kprintf("  zone GPIO%-2d %-8s %s  %-23s %s%s\n", z->pin, type_name[z->type], z->nc ? "nc" : "no", z->name,
                z->open ? "OPEN" : "ok", z->skip ? " (skipped until closed)" : "");
    }
    if (!nz) kprintf("  no zones yet: 'alarm zone <gpio> <instant|delay|interior|24h> <nc|no> <name>'\n");
    if (last_photo[0]) kprintf("  last photo %s\n", last_photo);
    if (last_video[0]) kprintf("  last video %s\n", last_video);
    access_info();
    if (ncards) { kprintf("  cards:"); for (int i = 0; i < ncards; i++) kprintf(" %s (%s)%s", cards[i].uid, cards[i].name, i + 1 < ncards ? "," : "\n"); }
}

void alarm_cmd(int argc, char **argv)
{
    if (!loaded) load();
    const char *a = argc > 1 ? argv[1] : "";
    char msg[80];
    int armed = on && state != DISARMED;
    if (!*a) { show(); return; }
    if (!strcmp(a, "arm") || !strcmp(a, "home") || !strcmp(a, "disarm") || !strcmp(a, "panic")) {
        const char *c = argc > 2 ? argv[2] : "";
        int what = !strcmp(a, "disarm") ? 2 : !strcmp(a, "panic") ? 3 : 1;
        if (what != 3 && !*c) { kprintf("alarm %s <code>\n", a); return; }
        act(what, !strcmp(a, "home"), c, "keyboard", msg, sizeof(msg));
        kprintf("%s\n", msg);
        return;
    }
    if (!strcmp(a, "log")) {
        lock();
        for (int i = 0; i < lg_n; i++) { int j = (lg_h - lg_n + i + NLOG) % NLOG; kprintf("%s  %s\n", lg[j].t, lg[j].e); }
        unlock();
        if (!lg_n) kprintf("(no events yet)\n");
        return;
    }
    if (!strcmp(a, "key") && argc > 2) {                  /* type on the keypad from here (tests, remote keypads) */
        for (int i = 2; i < argc; i++) for (const char *k = argv[i]; *k; k++) key_in(*k >= 'a' && *k <= 'd' ? (char)(*k - 32) : *k, "keypad (alarm key)");
        kprintf("%s\n", kmsg_until > timer_us() ? kmsg : klen ? "(code typed, end it with #)" : "");
        return;
    }
    if (!strcmp(a, "swipe") && argc > 2) { card_in(argv[2], "(alarm swipe)"); kprintf("%s\n", kmsg); return; }
    if (!strcmp(a, "card") && argc == 2) {
        if (!ncards) kprintf("no cards ('alarm card learn' then show a card to the reader)\n");
        for (int i = 0; i < ncards; i++) kprintf("  %s  %s\n", cards[i].uid, cards[i].name);
        return;
    }
    if (!strcmp(a, "test")) {
        char m[64];
        alarm_web_cmd("test", 0, 0, "keyboard", m, sizeof(m));
        kprintf("%s\n", m);
        return;
    }
    int changed = 0;
    if (armed && (!strcmp(a, "off") || !strcmp(a, "zone") || !strcmp(a, "set") || !strcmp(a, "code") || !strcmp(a, "shield") ||
                  !strcmp(a, "card") || !strcmp(a, "keypad") || !strcmp(a, "rfid"))) {
        kprintf("disarm first ('alarm disarm <code>')\n");
        return;
    }
    if (!strcmp(a, "shield")) {                           /* the IoT shield (both variants): IN1-4, relay K1 / OUT1, LED */
        static char b[2048];
        int n = cfg_text(b, sizeof(b), 1);
        char *z = strstr(b, "# zone");
        if (z) n = (int)(z - b);
        const char *add = "siren_pin 24\r\narmed_pin 23\r\nzone 17 delay nc IN1 door\r\nzone 27 instant nc IN2 window\r\n"
                          "zone 22 interior no IN3 PIR\r\nzone 5 24h nc IN4 tamper\r\n";
        for (const char *s2 = add; *s2 && n < (int)sizeof(b) - 1; s2++) b[n++] = *s2;
        b[n] = 0;
        char e[80];
        lock(); parse(b, 0, e, sizeof(e)); unlock();
        kprintf("IoT shield: IN1 GPIO17 door (delay), IN2 GPIO27 window, IN3 GPIO22 PIR (interior, normally open),\n"
                "IN4 GPIO5 tamper (24h); siren = relay K1 / OUT1 GPIO24, LED GPIO23. Rename / remove with 'alarm zone'.\n");
        changed = 1;
    } else if (!strcmp(a, "on") || !strcmp(a, "off")) {
        on = !strcmp(a, "on");
        if (!on) { siren(0); set_state(DISARMED); }
        changed = 1;
    } else if (!strcmp(a, "code") && argc > 3) {
        if (!code_ok(argv[2])) { kprintf("wrong old code\n"); return; }
        if (strlen(argv[3]) < 4 || strlen(argv[3]) > 15) { kprintf("the new code: 4-15 characters\n"); return; }
        scopy(code, argv[3], sizeof(code));
        changed = 1;
    } else if (!strcmp(a, "zone") && argc > 2) {
        static char b[2048];
        int n = cfg_text(b, sizeof(b), 1);
        int pin;
        if (!strcmp(argv[2], "del") && argc > 3 && !to_int(argv[3], &pin)) {
            int k = -1; for (int i = 0; i < nz; i++) if (zones[i].pin == pin) k = i;
            if (k < 0) { kprintf("no zone on GPIO%d\n", pin); return; }
            for (int i = k; i < nz - 1; i++) zones[i] = zones[i + 1];
            nz--;
            changed = 1;
        } else if (argc > 5) {
            char l[96] = "zone "; int m = 5;
            for (int i = 2; i < argc && m < 90; i++) { for (const char *s = argv[i]; *s && m < 90; s++) l[m++] = *s; if (i + 1 < argc) l[m++] = ' '; }
            l[m] = 0;
            if (to_int(argv[2], &pin) || !pin_ok(pin)) { kprintf("GPIO 2-27\n"); return; }
            if (strcmp(argv[3], "instant") && strcmp(argv[3], "delay") && strcmp(argv[3], "interior") && strcmp(argv[3], "24h")) { kprintf("type: instant, delay, interior or 24h\n"); return; }
            if (strcmp(argv[4], "nc") && strcmp(argv[4], "no")) { kprintf("nc (normally closed) or no (normally open)\n"); return; }
            /* the text without this pin's old zone + the new line */
            char *o = b, *w = b;
            while (*o) {
                char *ls = o; while (*o && *o != '\n') o++; if (*o) o++;
                int dropme = 0;
                if (!strncmp(ls, "zone ", 5)) { int p2; char t[4] = { 0 }; int q = 0; for (const char *s = ls + 5; *s >= '0' && *s <= '9' && q < 3; s++) t[q++] = *s; if (!to_int(t, &p2) && p2 == pin) dropme = 1; }
                if (!dropme) { while (ls < o) *w++ = *ls++; } else ls = o;
            }
            *w = 0; n = (int)(w - b);
            for (int i = 0; l[i] && n < (int)sizeof(b) - 3; i++) b[n++] = l[i];
            b[n++] = '\r'; b[n++] = '\n'; b[n] = 0;
            char e[80];
            lock(); parse(b, 0, e, sizeof(e)); unlock();
            if (e[0]) kprintf("%s\n", e);
            changed = 1;
        } else { kprintf("alarm zone <gpio> <instant|delay|interior|24h> <nc|no> <name>   /   alarm zone del <gpio>\n"); return; }
    } else if (!strcmp(a, "card") && argc > 2 && !strcmp(argv[2], "learn")) {
        if (!access_active()) { kprintf("no card reader set up ('alarm rfid rc522' / 'alarm rfid wiegand <d0> <d1>')\n"); return; }
        learn_until = timer_us() + 30000000ULL;
        display_poke();
        kprintf("show the new card to the reader within 30 s (it is saved by itself)\n");
        return;
    } else if (!strcmp(a, "card") && argc > 3 && (!strcmp(argv[2], "del") || !strcmp(argv[2], "add"))) {
        static char b[3072];
        int n = cfg_text(b, sizeof(b), 1), found = 0;
        if (argv[2][0] == 'd') {                          /* drop the line(s) of that card (number or name) */
            char *o = b, *w = b;
            while (*o) {
                char *ls = o; while (*o && *o != '\n') o++; if (*o) o++;
                int dropme = 0;
                if (!strncmp(ls, "card ", 5)) {
                    char u[24]; int q = 0; const char *p2 = ls + 5; while (*p2 && *p2 != ' ' && q < 23) u[q++] = *p2++; u[q] = 0;
                    char nm[64]; int r = 0; if (*p2 == ' ') p2++; while (*p2 && *p2 != '\r' && *p2 != '\n' && r < 63) nm[r++] = *p2++; nm[r] = 0;
                    char want[64]; int wl = 0;           /* the name may have spaces: join the words */
                    for (int i = 3; i < argc && wl < 62; i++) { for (const char *s2 = argv[i]; *s2 && wl < 62; s2++) want[wl++] = *s2; if (i + 1 < argc) want[wl++] = ' '; }
                    want[wl] = 0;
                    char low[64]; int i2 = 0; for (; want[i2]; i2++) low[i2] = want[i2] >= 'A' && want[i2] <= 'F' ? (char)(want[i2] + 32) : want[i2]; low[i2] = 0;
                    if (!strcmp(u, low) || !strcmp(nm, want)) dropme = found = 1;
                }
                if (!dropme) while (ls < o) *w++ = *ls++;
            }
            *w = 0; n = (int)(w - b);
            if (!found) { kprintf("no such card (see 'alarm card')\n"); return; }
        } else {
            char l[96] = "card "; int m = 5;
            for (int i = 3; i < argc && m < 90; i++) { for (const char *s2 = argv[i]; *s2 && m < 90; s2++) l[m++] = *s2; if (i + 1 < argc) l[m++] = ' '; }
            l[m] = 0;
            for (int i = 0; l[i] && n < (int)sizeof(b) - 3; i++) b[n++] = l[i];
            b[n++] = '\r'; b[n++] = '\n'; b[n] = 0;
        }
        char e[80];
        lock(); parse(b, 0, e, sizeof(e)); unlock();
        if (e[0]) kprintf("%s\n", e);
        changed = 1;
    } else if ((!strcmp(a, "keypad") || !strcmp(a, "rfid")) && argc > 2) {
        char l[96]; int m = 0;
        for (int i = 1; i < argc && m < 90; i++) { for (const char *s2 = argv[i]; *s2 && m < 90; s2++) l[m++] = *s2; if (i + 1 < argc) l[m++] = ' '; }
        l[m] = 0;
        char *ww[12]; char cp[96]; int kk = 0; scopy(cp, l, sizeof(cp));
        for (char *q = cp; *q && kk < 12; ) { while (*q == ' ') *q++ = 0; if (!*q) break; ww[kk++] = q; while (*q && *q != ' ') q++; }
        if (access_set(kk, ww)) {
            kprintf("alarm keypad plug [reversed]  |  alarm keypad gpio [<r1> .. <c4>] (a pin may be gnd)  |  alarm keypad i2c [addr] [swap]  |  alarm keypad off\n"
                    "alarm rfid rc522 [rst gpio]  |  alarm rfid wiegand <d0> <d1>  |  alarm rfid off\n");
            char e2[80]; static char b2[3072]; cfg_text(b2, sizeof(b2), 1); lock(); parse(b2, 0, e2, sizeof(e2)); unlock();   /* undo */
            return;
        }
        static char b[3072];
        int n = cfg_text(b, sizeof(b), 1);
        char e[80];
        lock(); parse(b, 0, e, sizeof(e)); unlock();
        (void)n;
        changed = 1;
    } else if (!strcmp(a, "set") && argc > 3) {
        if (!strcmp(argv[2], "code") || !strcmp(argv[2], "zone") || !strcmp(argv[2], "state")) { kprintf("use 'alarm code <old> <new>', 'alarm zone', 'alarm on|off'\n"); return; }
        char l[96]; int m = 0;
        for (int i = 2; i < argc && m < 90; i++) { for (const char *s = argv[i]; *s && m < 90; s++) l[m++] = *s; if (i + 1 < argc) l[m++] = ' '; }
        l[m] = 0;
        char e[80];
        lock(); struct zone keep[MAXZ]; int kn = nz; memcpy(keep, zones, sizeof(keep));
        static char b[2048];
        int n = cfg_text(b, sizeof(b), 1);
        for (int i = 0; l[i] && n < (int)sizeof(b) - 3; i++) b[n++] = l[i];
        b[n++] = '\r'; b[n++] = '\n'; b[n] = 0;
        parse(b, 0, e, sizeof(e));
        if (nz != kn) { memcpy(zones, keep, sizeof(keep)); nz = kn; }
        unlock();
        if (e[0]) { kprintf("%s\n(keys: exit entry siren siren_pin armed_pin sound volume voice photo photos video entry_photo mqtt)\n", e); return; }
        changed = 1;
    } else {
        kprintf("usage: alarm                      status, zones\n"
                "       alarm on | off             the alarm on / off (off only when disarmed)\n"
                "       alarm arm <code>           arm (away): exit time, then armed\n"
                "       alarm home <code>          arm, but 'interior' zones are bypassed (you are inside)\n"
                "       alarm disarm <code>\n"
                "       alarm panic                alarm now\n"
                "       alarm zone <gpio> <instant|delay|interior|24h> <nc|no> <name>    add / change a zone\n"
                "       alarm zone del <gpio>\n"
                "       alarm set <key> <value>    exit entry siren (s), siren_pin armed_pin (GPIO|none), sound on|off,\n"
                "                                  volume, voice <folder>|none, photo on|off, photos 1-10, video <s>,\n"
                "                                  entry_photo on|off, mqtt on|off\n"
                "       alarm code <old> <new>     change the code (default 1234!)\n"
                "       alarm shield               zones + siren for the RoboOS IoT shield (IN1-IN4, relay K1, LED)\n"
                "       alarm keypad plug [reversed]   the keypad's 8-pin connector pushed onto header pins 23-37 (inner row)\n"
                "       alarm keypad gpio          4x4 keypad straight on the header: wires 1-8 -> pins 29 31 33 35 32 36 38 40\n"
                "                                  (GPIO 5 6 13 19 rows, 12 16 20 21 columns; not with the IoT shield)\n"
                "       alarm keypad gpio <r1> <r2> <r3> <r4> <c1> <c2> <c3> [c4]   other pins\n"
                "       alarm keypad i2c [addr] [swap]   keypad on a PCF8574 (default 0x20)   |  alarm keypad off\n"
                "       alarm rfid rc522 [rst]     RC522 card reader on SPI0 (GPIO8-11)\n"
                "       alarm rfid wiegand <d0> <d1>   Wiegand reader / keypad   |  alarm rfid off\n"
                "       alarm card                 the cards; alarm card learn (show a new card in 30 s),\n"
                "                                  alarm card add <hex> [name], alarm card del <hex|name>\n"
                "       alarm key 1234#            type on the keypad from here; alarm swipe <hex> = a card read\n"
                "  keypad: code + # = arm (disarmed) / disarm, code + A = arm away, B = arm home, D = disarm, * = clear\n"
                "       alarm test                 siren + tone for 2 s\n"
                "       alarm log                  the last 48 events (also /ALARM.LOG)\n"
                "  web page: http(s)://<board>/alarm   (token or login like /camera)\n");
        return;
    }
    if (changed) {
        setup_pins();
        sound_volume(volume);
        if (save_cfg()) kprintf("(not saved: no SD card or write error)\n");
        else kprintf("(saved in /ALARM.TXT)\n");
        lock(); event("settings changed from the keyboard", ""); unlock();
        fx |= FX_SAVE_ST;
        mq_dirty = 1;
        mqtt_rediscover();
    }
    show();
}
