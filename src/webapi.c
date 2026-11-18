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
 *
 * Runs on the I/O core (from the lwIP callbacks in ip.c); no lwIP here.
 */
#include "kernel.h"

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
        code == 403 ? " Forbidden" : code == 404 ? " Not Found" : code == 405 ? " Method Not Allowed" : " Error");
    put("\r\nContent-Type: application/json\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n");
    if (code == 401) put("WWW-Authenticate: Bearer\r\n");
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
            if (ch == '\\' && jp < jend) ch = *jp++;
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

/* ---------------- requests ---------------- */

static int same_token(const char *a, int alen)
{
    int tl = (int)strlen(token), d = alen ^ tl;
    for (int i = 0; i < tl; i++) d |= (i < alen ? a[i] : 0) ^ token[i];   /* the same time for every guess */
    return tl > 0 && d == 0;
}

/*
 * method, path, the Authorization header value (or 0) and the body; the whole
 * HTTP answer goes to o. Returns its length.
 */
int api_handle(const char *method, const char *path, const char *auth, const char *body, int blen,
               const char *from, char *o, int max)
{
    out = o; omax = max; olen = 0;
    if (!token[0]) return fail(403, "API off: set a token on the board with 'http token <secret>'");
    int alen = 0;
    if (auth) {
        while (*auth == ' ') auth++;
        if (!memcmp_n(auth, "Bearer ", 7)) auth += 7; else auth = 0;
    }
    if (auth) { while (*auth == ' ') auth++; while (auth[alen] && auth[alen] != ' ' && auth[alen] != '\r') alen++; }
    if (!auth || !same_token(auth, alen)) {
        kprintf("\n[API] wrong or missing token from %s\n", from);
        return fail(401, "Authorization: Bearer <token> needed");
    }
    int get = !strcmp(method, "GET"), post = !strcmp(method, "POST") || !strcmp(method, "PUT");

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
    if (!strcmp(path, "/api/reboot")) {
        if (!post) return fail(405, "use POST");
        kprintf("\n[API] reboot asked by %s\n", from);
        reboot_at = timer_us() + 500000;
        head(200);
        put("{\"ok\":true,\"rebooting\":true}\n");
        return olen;
    }
    return fail(404, "unknown: GET /api/status, GET|POST /api/gpio, GET /api/gpio/<pin>, POST /api/reboot");
}
