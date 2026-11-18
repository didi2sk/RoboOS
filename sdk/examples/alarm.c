/*
 * alarm.c - a small burglar alarm (demo).
 *
 * Settings are in /ALARM.TXT on the SD card (made with defaults on the first start):
 *   code 1234              the PIN (password) that arms and disarms
 *   exit 20                seconds to leave after arming
 *   entry 15               seconds to disarm after a "delay" zone opens
 *   siren 180              seconds the siren sounds (0 = until disarmed)
 *   siren_pin 24           output: the siren / relay (HIGH = on)
 *   armed_pin 23           output: LED, on = armed, blinks = exit / entry time, fast = alarm
 *   sound on               sound on the audio jack: countdown, siren tone, voice
 *   volume 80              0-100
 *   voice /ALARM           folder with the voice WAV files ("none" = only tones)
 *   port 8080              TCP port for JSON (0 = no network)
 *   zone 17 delay nc Front door      inputs: zone <gpio> <instant|delay|24h> <nc|no> <name>
 *
 * Inputs have the pull-up on: wire each contact between the GPIO and GND.
 * nc = normally closed (a door contact: closed = LOW = OK, open = alarm),
 * no = normally open (a PIR relay or a button: closed = LOW = alarm).
 *   instant  alarm at once while armed
 *   delay    starts the entry time while armed (the way in); ignored during the exit time
 *   24h      alarm at any time, also disarmed (tamper, panic button)
 *
 * Commands on the serial port (UART, 115200 8N1) or the keyboard, one per line:
 *   arm <code>   disarm <code>   status   help
 * JSON over TCP (one line, e.g. nc <board> 8080) or HTTP POST to http://<board>:8080/
 *   {"cmd":"arm","code":"1234"}  {"cmd":"disarm","code":"1234"}  {"cmd":"status"}
 * Every event is printed as a line starting with "[ALARM" (serial port + screen).
 * Five wrong codes block the code for 60 s.
 *
 * Sound (the jack, or GPIO18/19 / an I2S DAC - 'sound out' in the shell): a beep every
 * second of the exit / entry time, the last 10 seconds spoken (10.WAV ... 1.WAV), the
 * siren as a rising and falling tone, and spoken events: ARMING, ARMED, ENTRY, ALARM,
 * DISARMED, WRONG, NOARM (.WAV in the voice folder; a missing file is just skipped).
 * tools/mkvoice.sh makes them with espeak-ng; record your own the same way (8-48 kHz).
 * A voice file plays to its end before the program goes on (keep them short).
 *
 * Note: Ctrl+C on the keyboard / serial port ends the program (it is a demo).
 */
#include "robos.h"

#define MAXZ 16
enum { Z_INSTANT, Z_DELAY, Z_24H };
enum { DISARMED, EXIT_TIME, ARMED, ENTRY_TIME, ALARM };
static const char *const state_name[] = { "disarmed", "exit", "armed", "entry", "alarm" };
static const char *const type_name[] = { "instant", "delay", "24h" };

struct zone {
    int pin, type, nc;
    char name[24];
    int open;                 /* debounced: 1 = open / triggered */
    int raw;                  /* last raw reading */
    int opened;               /* it just opened (a 24h zone fires once per opening) */
    uint64_t raw_since;
};

/* settings */
static char code[16] = "1234";
static int exit_s = 20, entry_s = 15, siren_s = 180, siren_pin = 24, armed_pin = 23, sound = 1, volume = 80, port = 8080;
static char voice[48] = "/ALARM";
static struct zone zones[MAXZ];
static int nz;

/* state */
static int state = DISARMED;
static uint64_t state_t;      /* when the state started (ms) */
static int siren_on;
static int alarm_zone = -1;   /* the zone that caused the alarm / entry time */
static int wrong, locked_until_s;

/* ---------------- small helpers (no C library in programs) ---------------- */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void scopy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int to_int(const char *s) { int v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }
static uint64_t now_s(void) { return millis() / 1000; }

/* the same time for a right and a wrong code */
static int code_ok(const char *c)
{
    int a = slen(c), b = slen(code), d = a ^ b;
    for (int i = 0; i < b; i++) d |= (i < a ? c[i] : 0) ^ code[i];
    return d == 0;
}

static void event(const char *text, const char *detail)
{
    uint64_t s = now_s();
    textColor(state == ALARM ? 4 : state == DISARMED ? 1 : 3);
    printf("[ALARM %02u:%02u:%02u] %s%s%s\n", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60),
           text, detail && *detail ? " " : "", detail ? detail : "");
    textColor(0);
}

/* ---------------- /ALARM.TXT ---------------- */

static const char default_cfg[] =
    "# RoboOS alarm - settings (see sdk/examples/alarm.c)\r\n"
    "code 1234\r\n"
    "exit 20\r\n"
    "entry 15\r\n"
    "siren 180\r\n"
    "siren_pin 24\r\n"
    "armed_pin 23\r\n"
    "sound on\r\n"
    "volume 80\r\n"
    "voice /ALARM\r\n"
    "port 8080\r\n"
    "# zone <gpio> <instant|delay|24h> <nc|no> <name>\r\n"
    "zone 17 delay nc Front door\r\n"
    "zone 27 instant nc Window\r\n"
    "zone 22 instant no PIR hall\r\n"
    "zone 5 24h nc Tamper\r\n";

static void load_config(void)
{
    static char b[2048];
    int n = readFile("/ALARM.TXT", b, sizeof(b) - 1);
    if (n < 0) {
        n = slen(default_cfg);
        for (int i = 0; i < n; i++) b[i] = default_cfg[i];
        if (writeFile("/ALARM.TXT", default_cfg, n) == 0) println("/ALARM.TXT made with the default settings - edit it");
    }
    b[n] = 0;
    char *p = b;
    while (*p) {
        char *w[5]; int k = 0;
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        char *q = line;
        while (*q && k < 5) {                          /* up to 4 words, then the rest of the line */
            while (is_space(*q)) *q++ = 0;
            if (!*q || *q == '#') break;
            w[k++] = q;
            if (k == 5) break;
            while (*q && !is_space(*q)) q++;
        }
        if (k && w[0][0] != '#') {
            if (k == 5) { char *e = w[4] + slen(w[4]); while (e > w[4] && is_space(e[-1])) *--e = 0; }
            if (same(w[0], "code") && k > 1) scopy(code, w[1], sizeof(code));
            else if (same(w[0], "exit") && k > 1) exit_s = to_int(w[1]);
            else if (same(w[0], "entry") && k > 1) entry_s = to_int(w[1]);
            else if (same(w[0], "siren") && k > 1) siren_s = to_int(w[1]);
            else if (same(w[0], "siren_pin") && k > 1) siren_pin = to_int(w[1]);
            else if (same(w[0], "armed_pin") && k > 1) armed_pin = same(w[1], "none") ? -1 : to_int(w[1]);
            else if ((same(w[0], "sound") || same(w[0], "beep")) && k > 1) sound = same(w[1], "on");
            else if (same(w[0], "volume") && k > 1) volume = to_int(w[1]);
            else if (same(w[0], "voice") && k > 1) scopy(voice, same(w[1], "none") ? "" : w[1], sizeof(voice));
            else if (same(w[0], "port") && k > 1) port = to_int(w[1]);
            else if (same(w[0], "zone") && k >= 4 && nz < MAXZ) {
                struct zone *z = &zones[nz];
                z->pin = to_int(w[1]);
                z->type = same(w[2], "delay") ? Z_DELAY : same(w[2], "24h") ? Z_24H : Z_INSTANT;
                z->nc = !same(w[3], "no");
                scopy(z->name, k == 5 ? w[4] : "zone", sizeof(z->name));
                if (z->pin >= 0 && z->pin <= 27) nz++;
                else printf("zone: GPIO %d is not 0-27\n", z->pin);
            } else printf("/ALARM.TXT: what is '%s'?\n", w[0]);
        }
    }
}

/* ---------------- outputs ---------------- */

static void siren(int on)
{
    if (siren_on == on) return;
    siren_on = on;
    digitalWrite(siren_pin, on ? HIGH : LOW);
}

static void lights(void)
{
    uint64_t t = millis();
    int on;
    switch (state) {
    case DISARMED:   on = 0; break;
    case ARMED:      on = 1; break;
    case ALARM:      on = (t / 125) & 1; break;     /* fast */
    default:         on = (t / 500) & 1; break;     /* exit / entry: slow */
    }
    if (armed_pin >= 0) digitalWrite(armed_pin, on);
    actLed(on);
}

/* ---------------- sound ---------------- */

/* a voice file from the voice folder, e.g. say("ARMED") -> /ALARM/ARMED.WAV; waits until it ends */
static int say(const char *name)
{
    char p[64];
    if (!sound || !voice[0]) return -1;
    scopy(p, voice, 48);
    int n = slen(p);
    p[n++] = '/';
    scopy(p + n, name, (int)sizeof(p) - n - 4);
    n = slen(p);
    scopy(p + n, ".WAV", (int)sizeof(p) - n);
    int r = playWav(p);
    return r;
}

static int last_left = -1;

/* exit / entry time: a beep every second, the last 10 seconds spoken */
static void countdown(void)
{
    if (state != EXIT_TIME && state != ENTRY_TIME) { last_left = -1; return; }
    int total = state == EXIT_TIME ? exit_s : entry_s;
    int left = total - (int)((millis() - state_t) / 1000);
    if (left == last_left || left <= 0) return;
    last_left = left;
    if (!sound) return;
    if (left <= 10) {
        char nm[4] = { 0 };
        if (left == 10) { nm[0] = '1'; nm[1] = '0'; } else nm[0] = (char)('0' + left);
        if (say(nm) == 0) return;
    }
    tone(state == ENTRY_TIME ? 2500 : 2000, state == ENTRY_TIME ? 150 : 80);
}

/* the siren: a tone rising 600 -> 1200 Hz and falling back every 2 seconds */
static int wailing;
static uint64_t last_wail;
static void siren_sound(void)
{
    if (!sound) return;
    if (!siren_on) { if (wailing) { noTone(); wailing = 0; } return; }
    uint64_t t = millis();
    if (wailing && t - last_wail < 20) return;
    last_wail = t;
    int ph = (int)(t % 2000);
    int f = ph < 1000 ? 600 + ph * 600 / 1000 : 1200 - (ph - 1000) * 600 / 1000;
    tone(f, 0);
    wailing = 1;
}

/* ---------------- states ---------------- */

static void set_state(int s) { state = s; state_t = millis(); }

static void start_alarm(int zi)
{
    alarm_zone = zi;
    set_state(ALARM);
    siren(1);
    event("ALARM!", zones[zi].name);
    say("ALARM");
}

/* answer: a short text for the caller; returns 1 = done */
static int do_arm(const char *c, const char *who, const char **answer)
{
    if (locked_until_s && now_s() < (uint64_t)locked_until_s) { *answer = "code blocked for a while"; return 0; }
    if (!code_ok(c)) {
        *answer = "wrong code";
        event("wrong code from", who);
        say("WRONG");
        if (++wrong >= 5) { locked_until_s = (int)now_s() + 60; wrong = 0; event("code blocked for 60 s", ""); }
        return 0;
    }
    wrong = 0;
    if (state != DISARMED) { *answer = "already armed"; return 0; }
    for (int i = 0; i < nz; i++)
        if (zones[i].open && zones[i].type != Z_DELAY) {
            static char t[48];
            scopy(t, "cannot arm, open: ", sizeof(t));
            int n = slen(t);
            scopy(t + n, zones[i].name, (int)sizeof(t) - n);
            *answer = t;
            event(t, "");
            say("NOARM");
            return 0;
        }
    set_state(exit_s > 0 ? EXIT_TIME : ARMED);
    event(exit_s > 0 ? "arming, exit time started by" : "ARMED by", who);
    say(exit_s > 0 ? "ARMING" : "ARMED");
    *answer = exit_s > 0 ? "exit time" : "armed";
    return 1;
}

static int do_disarm(const char *c, const char *who, const char **answer)
{
    if (locked_until_s && now_s() < (uint64_t)locked_until_s) { *answer = "code blocked for a while"; return 0; }
    if (!code_ok(c)) {
        *answer = "wrong code";
        event("wrong code from", who);
        say("WRONG");
        if (++wrong >= 5) { locked_until_s = (int)now_s() + 60; wrong = 0; event("code blocked for 60 s", ""); }
        return 0;
    }
    wrong = 0;
    siren(0);
    noTone();
    wailing = 0;
    set_state(DISARMED);
    alarm_zone = -1;
    event("DISARMED by", who);
    say("DISARMED");
    *answer = "disarmed";
    return 1;
}

static void read_zones(void)
{
    uint64_t t = millis();
    for (int i = 0; i < nz; i++) {
        struct zone *z = &zones[i];
        int level = digitalRead(z->pin);
        int trig = z->nc ? level == HIGH : level == LOW;    /* pull-up: contact to GND */
        if (trig != z->raw) { z->raw = trig; z->raw_since = t; }
        if (z->open != z->raw && t - z->raw_since >= 50) {   /* 50 ms debounce */
            z->open = z->raw;
            if (z->open) z->opened = 1;
            char d[40];
            scopy(d, z->name, 26);
            int n = slen(d);
            scopy(d + n, z->open ? ": open" : ": closed", (int)sizeof(d) - n);
            event("zone", d);
        }
    }
}

static void run_states(void)
{
    uint64_t t = millis();
    for (int i = 0; i < nz; i++) {
        struct zone *z = &zones[i];
        int fresh = z->opened;
        z->opened = 0;
        if (!z->open) continue;
        if (z->type == Z_24H) { if (fresh && state != ALARM) { start_alarm(i); return; } continue; }
        if (state == ARMED && z->type == Z_INSTANT) { start_alarm(i); return; }
        if (state == ENTRY_TIME && z->type == Z_INSTANT) { start_alarm(i); return; }
        if (state == ARMED && z->type == Z_DELAY) {
            alarm_zone = i;
            set_state(ENTRY_TIME);
            event("entry time started:", z->name);
            say("ENTRY");
            if (entry_s == 0) { start_alarm(i); return; }
        }
    }
    if (state == EXIT_TIME && t - state_t >= (uint64_t)exit_s * 1000) { set_state(ARMED); event("ARMED", ""); say("ARMED"); }
    if (state == ENTRY_TIME && t - state_t >= (uint64_t)entry_s * 1000) { start_alarm(alarm_zone); return; }
    if (state == ALARM && siren_on && siren_s > 0 && millis() - state_t >= (uint64_t)siren_s * 1000) {
        siren(0);
        event("siren off (time is up) - still in alarm until disarmed", "");
    }
}

/* ---------------- status as JSON ---------------- */

static char js[1536];
static int jn;
static void jadd(const char *s) { while (*s && jn < (int)sizeof(js) - 2) js[jn++] = *s++; js[jn] = 0; }
static void jnum(int v) { char d[12]; int k = 0; if (v < 0) { jadd("-"); v = -v; } do d[k++] = '0' + v % 10; while (v /= 10); while (k) { char c[2] = { d[--k], 0 }; jadd(c); } }
static void jstr(const char *s)
{
    jadd("\"");
    for (; *s; s++) { char c[2] = { *s, 0 }; if (*s == '"' || *s == '\\') jadd("\\"); if ((uint8_t)*s >= 32) jadd(c); }
    jadd("\"");
}

static const char *status_json(int ok, const char *msg)
{
    jn = 0;
    jadd("{\"ok\":"); jadd(ok ? "true" : "false");
    if (msg) { jadd(",\"message\":"); jstr(msg); }
    jadd(",\"state\":"); jstr(state_name[state]);
    jadd(",\"siren\":"); jadd(siren_on ? "true" : "false");
    if (state == EXIT_TIME || state == ENTRY_TIME) {
        int left = (state == EXIT_TIME ? exit_s : entry_s) - (int)((millis() - state_t) / 1000);
        jadd(",\"seconds_left\":"); jnum(left < 0 ? 0 : left);
    }
    if (alarm_zone >= 0 && (state == ALARM || state == ENTRY_TIME)) { jadd(",\"cause\":"); jstr(zones[alarm_zone].name); }
    jadd(",\"zones\":[");
    for (int i = 0; i < nz; i++) {
        if (i) jadd(",");
        jadd("{\"pin\":"); jnum(zones[i].pin);
        jadd(",\"name\":"); jstr(zones[i].name);
        jadd(",\"type\":"); jstr(type_name[zones[i].type]);
        jadd(",\"open\":"); jadd(zones[i].open ? "true" : "false");
        jadd("}");
    }
    jadd("]}");
    return js;
}

/* the string (or number) value of "key" in a flat JSON object; 0 = missing */
static int json_get(const char *j, const char *key, char *out, int max)
{
    int kl = slen(key);
    for (const char *p = j; *p; p++) {
        if (*p != '"') continue;
        int i = 0;
        while (i < kl && p[1 + i] == key[i]) i++;
        if (i < kl || p[1 + kl] != '"') continue;
        const char *v = p + kl + 2;
        while (is_space(*v)) v++;
        if (*v != ':') continue;
        v++;
        while (is_space(*v)) v++;
        int n = 0;
        if (*v == '"') { v++; while (*v && *v != '"' && n < max - 1) out[n++] = *v++; }
        else while (*v && *v != ',' && *v != '}' && !is_space(*v) && n < max - 1) out[n++] = *v++;
        out[n] = 0;
        return 1;
    }
    return 0;
}

static const char *handle_json(const char *j)
{
    char cmd[16], c[24];
    const char *answer = "";
    if (!json_get(j, "cmd", cmd, sizeof(cmd))) return status_json(0, "{\"cmd\":\"arm|disarm|status\",\"code\":\"...\"}");
    if (!json_get(j, "code", c, sizeof(c))) c[0] = 0;
    if (same(cmd, "status")) return status_json(1, 0);
    if (same(cmd, "arm"))    { int ok = do_arm(c, "network (JSON)", &answer); return status_json(ok, answer); }
    if (same(cmd, "disarm")) { int ok = do_disarm(c, "network (JSON)", &answer); return status_json(ok, answer); }
    return status_json(0, "unknown cmd (arm, disarm, status)");
}

/* ---------------- network: JSON lines or HTTP ---------------- */

#define NCLI 4
struct client { int h, len; uint64_t since; char buf[1024]; };
static struct client cli[NCLI];
static int listener;

static void cli_close(struct client *c) { sockClose(c->h); c->h = 0; }

static void http_answer(struct client *c, int code, const char *body)
{
    sockPrint(c->h, code == 200 ? "HTTP/1.0 200 OK\r\n" : "HTTP/1.0 404 Not Found\r\n");
    sockPrint(c->h, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
    sockPrint(c->h, body);
    sockPrint(c->h, "\n");
    cli_close(c);
}

static void serve(struct client *c)
{
    c->buf[c->len] = 0;
    if (c->buf[0] == '{') {                                /* JSON lines (nc): one answer per line */
        for (;;) {
            char *nl = 0;
            for (int i = 0; i < c->len; i++) if (c->buf[i] == '\n') { nl = c->buf + i; break; }
            if (!nl) break;
            *nl = 0;
            sockPrint(c->h, handle_json(c->buf));
            sockPrint(c->h, "\n");
            int rest = c->len - (int)(nl + 1 - c->buf);
            for (int i = 0; i < rest; i++) c->buf[i] = nl[1 + i];
            c->len = rest;
            c->buf[c->len] = 0;
            c->since = millis();
        }
        return;
    }
    /* HTTP: wait for the headers and Content-Length bytes */
    char *body = 0;
    for (int i = 0; i + 3 < c->len; i++)
        if (c->buf[i] == '\r' && c->buf[i + 1] == '\n' && c->buf[i + 2] == '\r' && c->buf[i + 3] == '\n') { body = c->buf + i + 4; break; }
    if (!body) return;
    int need = 0;
    for (char *p = c->buf; p < body; p++)
        if ((*p == 'C' || *p == 'c') && (p[8] == 'L' || p[8] == 'l') && (p[14] == ':')) { need = to_int(p + 15 + (p[15] == ' ')); break; }
    if (c->len - (int)(body - c->buf) < need) return;
    body[need] = 0;
    if (c->buf[0] == 'P') http_answer(c, 200, handle_json(body));            /* POST / PUT */
    else if (c->buf[0] == 'G') http_answer(c, 200, status_json(1, 0));       /* GET: status */
    else if (c->buf[0] == 'O') {                                              /* OPTIONS (browser) */
        sockPrint(c->h, "HTTP/1.0 204 No Content\r\nAccess-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\n\r\n");
        cli_close(c);
    } else http_answer(c, 404, "{\"ok\":false}");
}

static void network(void)
{
    if (listener <= 0) return;
    int h = tcpAccept(listener);
    if (h > 0) {
        int i = 0;
        while (i < NCLI && cli[i].h) i++;
        if (i == NCLI) sockClose(h);                       /* full */
        else { cli[i].h = h; cli[i].len = 0; cli[i].since = millis(); }
    }
    for (int i = 0; i < NCLI; i++) {
        struct client *c = &cli[i];
        if (!c->h) continue;
        int room = (int)sizeof(c->buf) - 1 - c->len;
        int n = room > 0 ? sockRead(c->h, c->buf + c->len, room) : 0;
        if (n < 0) { cli_close(c); continue; }
        if (n > 0) { c->len += n; c->since = millis(); serve(c); }
        if (c->h && (room <= 0 || millis() - c->since > 60000)) cli_close(c);   /* full or quiet for a minute */
    }
}

/* ---------------- serial port / keyboard ---------------- */

static char line[64];
static int ll;

static void command(void)
{
    char *w = line, *arg;
    const char *answer = "";
    line[ll] = 0;
    while (is_space(*w)) w++;
    arg = w;
    while (*arg && !is_space(*arg)) arg++;
    if (*arg) *arg++ = 0;
    while (is_space(*arg)) arg++;
    if (same(w, "arm"))         { do_arm(arg, "serial / keyboard", &answer); printf("%s\n", answer); }
    else if (same(w, "disarm")) { do_disarm(arg, "serial / keyboard", &answer); printf("%s\n", answer); }
    else if (same(w, "status")) println(status_json(1, 0));
    else if (*w)                println("commands: arm <code>, disarm <code>, status   (Ctrl+C ends the program)");
}

static void keys(void)
{
    int k;
    while ((k = getKey()) >= 0) {
        if (k == '\r' || k == '\n') {
            api->putc('\n');
            command();
            ll = 0;
        } else if ((k == 8 || k == 127) && ll > 0) {
            ll--;
            print("\b \b");
        } else if (k >= 32 && k < 127 && ll < (int)sizeof(line) - 1) {
            int after_word = 0;                            /* the code after "arm " / "disarm " shows as * */
            for (int i = 0; i < ll; i++) if (line[i] == ' ') after_word = 1;
            line[ll++] = (char)k;
            api->putc(after_word && k != ' ' && (line[0] == 'a' || line[0] == 'd') ? '*' : (char)k);
        }
    }
}

/* ---------------- program ---------------- */

void setup(void)
{
    textColor(2);
    println("RoboOS alarm (demo) - settings in /ALARM.TXT");
    textColor(0);
    load_config();
    pinMode(siren_pin, OUTPUT);
    digitalWrite(siren_pin, LOW);
    if (armed_pin >= 0) { pinMode(armed_pin, OUTPUT); digitalWrite(armed_pin, LOW); }
    soundVolume(volume);
    printf("siren GPIO %d, LED GPIO %d, exit %d s, entry %d s, siren %d s, sound %s%s%s\n", siren_pin, armed_pin,
           exit_s, entry_s, siren_s, sound ? "on" : "off", sound && voice[0] ? ", voice " : "", sound ? voice : "");
    for (int i = 0; i < nz; i++) {
        struct zone *z = &zones[i];
        pinMode(z->pin, INPUT_PULLUP);
        delay(2);
        int level = digitalRead(z->pin);
        z->raw = z->open = z->nc ? level == HIGH : level == LOW;
        printf("  zone GPIO %2d  %-7s %s  %-24s %s\n", z->pin, type_name[z->type], z->nc ? "nc" : "no", z->name, z->open ? "OPEN" : "ok");
    }
    if (port > 0) {
        listener = tcpListen(port);
        if (listener < 0) printf("network: %s (only the serial port works)\n", sockError(listener));
        else printf("JSON: nc %s %d   or   http://%s:%d/  (POST {\"cmd\":\"arm\",\"code\":\"...\"})\n",
                    ipAddress(), port, ipAddress(), port);
    }
    println("commands: arm <code>, disarm <code>, status");
    event("started,", "disarmed");
}

void loop(void)
{
    read_zones();
    run_states();
    lights();
    countdown();
    siren_sound();
    keys();
    network();
    delay(5);
}
