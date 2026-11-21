/*
 * ai.c - questions to Claude (Anthropic Messages API) over HTTPS from the shell (0.56).
 *   ai <question>              an answer on the screen
 *   ai @<command> <question>   runs the command, sends its output with the question ("ai @wifi why so slow")
 *   ai key <key> | ai key off  the API key, kept in /AI.TXT XOR-scrambled (NOT real encryption: it only
 *                              keeps the key from being read at a glance - whoever has the card can undo it)
 *   ai model <id>              default claude-haiku-5-5 (the cheapest)
 * The system prompt tells the model what RoboOS is and lists the shell commands, so it can help with them.
 * POST https://api.anthropic.com/v1/messages (fetch_post: BearSSL, certificate checked), core 0.
 * The answer: content[] blocks of type "text"; UTF-8 / \uXXXX turned into ASCII for the 8x16 font.
 */
#include "kernel.h"
#include "sd.h"

#define DEF_MODEL "claude-haiku-5-5"
#define API_URL   "https://api.anthropic.com/v1/messages"

static char key[160], model[64], url[160];
static int loaded;

/* ---------------- /AI.TXT ---------------- */

static const char pad[] = "RoboOS:keep-this-key-off-the-screen";
static u8 scramble(int i) { return (u8)(pad[i % (int)(sizeof(pad) - 1)] ^ (u8)(i * 29 + 7)); }

static void save(void);

static void load(void)
{
    struct fat_file f;
    static char b[600];
    int plain = 0;
    loaded = 1;
    key[0] = 0;
    memcpy(model, DEF_MODEL, sizeof(DEF_MODEL));
    memcpy(url, API_URL, sizeof(API_URL));
    if (!fat_mounted() || fat_open("/AI.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    for (char *l = b; *l; ) {
        char *e = l;
        while (*e && *e != '\n' && *e != '\r') e++;
        char save = *e; *e = 0;
        if (!strncmp(l, "model ", 6) && e - l - 6 < (int)sizeof(model) && e - l > 6) memcpy(model, l + 6, (size_t)(e - l - 6 + 1));
        if (!strncmp(l, "url ", 4) && e - l - 4 < (int)sizeof(url) && e - l > 4) memcpy(url, l + 4, (size_t)(e - l - 4 + 1));   /* tests / a proxy */
        const char *kv = 0;
        if (!strncmp(l, "key", 3) && (l[3] == ' ' || l[3] == '=')) { kv = l + 3; while (*kv == ' ' || *kv == '=') kv++; }
        if (!kv) {                                        /* the bare key, or API_KEY=sk-..., "sk-..." etc. */
            const char *sk = strstr(l, "sk-ant-");
            if (sk && strncmp(l, "url", 3) && strncmp(l, "model", 5)) kv = sk;
        }
        if (kv && !strncmp(kv, "sk-", 3)) {               /* typed in by hand on a PC: plain text - scrambled below */
            int k = 0;
            while (kv[k] && kv[k] != ' ' && kv[k] != '\t' && kv[k] != '"' && kv[k] != '\'' && k < (int)sizeof(key) - 1) { key[k] = kv[k]; k++; }
            key[k] = 0;
            plain = 1;
        } else if (kv) {                                  /* hex of key ^ scramble */
            int k = 0;
            for (const char *h = kv; h[0] && h[1] && k < (int)sizeof(key) - 1; h += 2) {
                int v = 0;
                for (int j = 0; j < 2; j++) { char c = h[j]; v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10); }
                key[k] = (char)(v ^ scramble(k)); k++;
            }
            key[k] = 0;
        }
        *e = save;
        l = *e ? e + 1 : e;
        while (*l == '\n' || *l == '\r') l++;
    }
    if (plain) {                                          /* write it back scrambled at once */
        save();
        kprintf("(the plain API key in /AI.TXT was scrambled now)\n");
    }
}

static void save(void)
{
    static char b[600];
    int n = 0;
    const char *m = "model ";
    for (const char *s = m; *s; s++) b[n++] = *s;
    for (const char *s = model; *s; s++) b[n++] = *s;
    b[n++] = '\n';
    if (strcmp(url, API_URL)) { for (const char *s = "url "; *s; s++) b[n++] = *s; for (const char *s = url; *s; s++) b[n++] = *s; b[n++] = '\n'; }
    if (key[0]) {
        b[n++] = 'k'; b[n++] = 'e'; b[n++] = 'y'; b[n++] = ' ';
        for (int i = 0; key[i]; i++) {
            u8 v = (u8)key[i] ^ scramble(i);
            b[n++] = "0123456789abcdef"[v >> 4];
            b[n++] = "0123456789abcdef"[v & 15];
        }
        b[n++] = '\n';
    }
    if (!fat_mounted() || fat_write_file("/AI.TXT", b, (u32)n)) kprintf("could not write /AI.TXT\n");
}

/* ---------------- JSON ---------------- */

static char *jb;
static int jn, jmax;
static void put(const char *s) { while (*s && jn < jmax - 1) jb[jn++] = *s++; }
static void put_str(const char *s)                        /* a JSON string with escapes */
{
    put("\"");
    for (; *s && jn < jmax - 8; s++) {
        u8 c = (u8)*s;
        if (c == '"' || c == '\\') { jb[jn++] = '\\'; jb[jn++] = (char)c; }
        else if (c == '\n') { jb[jn++] = '\\'; jb[jn++] = 'n'; }
        else if (c == '\r') continue;
        else if (c == '\t') { jb[jn++] = '\\'; jb[jn++] = 't'; }
        else if (c < 0x20) continue;
        else if (c >= 0x80) jb[jn++] = '?';               /* our console is ASCII; keep the JSON valid UTF-8 */
        else jb[jn++] = (char)c;
    }
    put("\"");
}

/* Unicode -> ASCII for the 8x16 font (Slovak / Czech / German letters lose the accent) */
static char ascii_of(u32 cp)
{
    if (cp < 0x80) return (char)cp;
    static const struct { u16 cp; char c; } t[] = {
        {0xE1,'a'},{0xE4,'a'},{0x10D,'c'},{0x10F,'d'},{0xE9,'e'},{0x11B,'e'},{0xED,'i'},{0x13A,'l'},{0x13E,'l'},
        {0x148,'n'},{0xF3,'o'},{0xF4,'o'},{0xF6,'o'},{0x155,'r'},{0x159,'r'},{0x161,'s'},{0x165,'t'},{0xFA,'u'},
        {0x16F,'u'},{0xFC,'u'},{0xFD,'y'},{0x17E,'z'},{0xDF,'s'},
        {0xC1,'A'},{0xC4,'A'},{0x10C,'C'},{0x10E,'D'},{0xC9,'E'},{0x11A,'E'},{0xCD,'I'},{0x139,'L'},{0x13D,'L'},
        {0x147,'N'},{0xD3,'O'},{0xD4,'O'},{0xD6,'O'},{0x154,'R'},{0x158,'R'},{0x160,'S'},{0x164,'T'},{0xDA,'U'},
        {0x16E,'U'},{0xDC,'U'},{0xDD,'Y'},{0x17D,'Z'},
        {0x2018,'\''},{0x2019,'\''},{0x201A,','},{0x201C,'"'},{0x201D,'"'},{0x201E,'"'},{0x2013,'-'},{0x2014,'-'},
        {0x2022,'*'},{0xA0,' '},{0xB0,'o'},{0xD7,'x'},{0x2192,'>'},{0x2026,'.'},{0x2212,'-'},
    };
    for (u32 i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (t[i].cp == cp) return t[i].c;
    return '?';
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

/* p points just after an opening quote: decode the JSON string into out (ASCII); returns the char after
 * the closing quote */
static const char *jstr(const char *p, char *out, int max, int *outlen)
{
    int n = 0;
    while (*p && *p != '"') {
        u32 cp;
        if (*p == '\\') {
            p++;
            char e = *p ? *p++ : 0;
            if (e == 'n') cp = '\n'; else if (e == 't') cp = ' '; else if (e == 'r') continue;
            else if (e == 'b' || e == 'f') continue;
            else if (e == 'u') {
                cp = 0;
                for (int i = 0; i < 4 && hexv(*p) >= 0; i++) cp = cp * 16 + (u32)hexv(*p++);
                if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u') {      /* surrogate pair */
                    u32 lo = 0; p += 2;
                    for (int i = 0; i < 4 && hexv(*p) >= 0; i++) lo = lo * 16 + (u32)hexv(*p++);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
            } else cp = (u8)e;
        } else {
            u8 c = (u8)*p++;
            if (c < 0x80) cp = c;
            else {                                         /* UTF-8 */
                int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
                cp = c & (more == 3 ? 0x07 : more == 2 ? 0x0F : 0x1F);
                while (more-- && ((u8)*p & 0xC0) == 0x80) cp = (cp << 6) | ((u8)*p++ & 0x3F);
            }
        }
        if (n < max - 1) out[n++] = ascii_of(cp);
    }
    out[n] = 0;
    if (outlen) *outlen = n;
    return *p ? p + 1 : p;
}

/* "key" : <value> somewhere after p - pointer to the value (spaces skipped), 0 = none */
static const char *jkey(const char *p, const char *k)
{
    int kl = (int)strlen(k);
    for (; (p = strstr(p, k)); p += kl) {
        if (p[-1] != '"' || p[kl] != '"') continue;
        const char *v = p + kl + 1;
        while (*v == ' ' || *v == '\n' || *v == '\r' || *v == '\t') v++;
        if (*v != ':') continue;
        v++;
        while (*v == ' ' || *v == '\n' || *v == '\r' || *v == '\t') v++;
        return v;
    }
    return 0;
}

static long jnum(const char *p, const char *k)
{
    const char *v = jkey(p, k);
    long n = 0;
    if (!v) return -1;
    while (*v >= '0' && *v <= '9') n = n * 10 + (*v++ - '0');
    return n;
}

/* ---------------- the question ---------------- */

static char body[24 * 1024], resp[64 * 1024], cmds[8 * 1024], cap[6 * 1024], text[16 * 1024];

static void ask(const char *question, const char *cmdline)
{
    int caplen = 0;
    if (cmdline) {                                         /* run it and keep what it printed */
        con_color(COL_GRAY); kprintf("$ %s\n", cmdline); con_color(COL_DEFAULT);
        con_capture(cap, sizeof(cap));
        shell_execute(cmdline);
        caplen = con_capture_end();
    }
    shell_commands(cmds, sizeof(cmds));
    jb = body; jn = 0; jmax = sizeof(body);
    put("{\"model\":"); put_str(model);
    put(",\"max_tokens\":4096,\"output_config\":{\"effort\":\"low\"},\"system\":");
    static char sys[10 * 1024];
    int sn = 0;
    const char *parts[] = {
        "You are the help built into RoboOS ", os_version(), ", a bare-metal hobby operating system (C + AArch64 assembly, "
        "no Linux) running on a ", board_name(), ". The user types questions at its shell. Answer briefly and to the point "
        "(at most about 20 lines), in plain text: no Markdown, no tables, no code fences. The screen shows ASCII only, so "
        "accented letters lose their accents - that is fine. Answer in the language of the question (the owner usually writes "
        "Slovak). When the question is about RoboOS, name the exact shell commands to type; use only commands from the list "
        "below and do not invent options. If output of a command is attached, use it to explain or diagnose.\n\n"
        "RoboOS shell commands (name arguments - meaning):\n", cmds };
    for (u32 i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) for (const char *s = parts[i]; *s && sn < (int)sizeof(sys) - 1; s++) sys[sn++] = *s;
    sys[sn] = 0;
    put_str(sys);
    put(",\"messages\":[{\"role\":\"user\",\"content\":");
    static char q[8 * 1024];
    int qn = 0;
    if (caplen) {
        const char *a[] = { "Output of the RoboOS command `", cmdline, "`:\n", cap, "\n\nQuestion: " };
        for (int i = 0; i < 5; i++) for (const char *s = a[i]; *s && qn < (int)sizeof(q) - 1; s++) q[qn++] = *s;
    }
    for (const char *s = question; *s && qn < (int)sizeof(q) - 1; s++) q[qn++] = *s;
    q[qn] = 0;
    put_str(q);
    put("}]}");
    body[jn] = 0;

    static char hdr[320];
    int hn = 0;
    const char *h[] = { "Content-Type: application/json\r\nanthropic-version: 2026-10-21\r\nx-api-key: ", key, "\r\n" };
    for (int i = 0; i < 3; i++) for (const char *s = h[i]; *s && hn < (int)sizeof(hdr) - 1; s++) hdr[hn++] = *s;
    hdr[hn] = 0;

    con_color(COL_GRAY); kprintf("asking %s ... (Ctrl+C stops)\n", model); con_color(COL_DEFAULT);
    u64 t0 = timer_us();
    int status = 0;
    long n = fetch_post(url, hdr, body, jn, (u8 *)resp, sizeof(resp) - 1, &status, 120);
    memset(hdr, 0, sizeof(hdr));                           /* no copy of the key left around */
    if (n < 0) {
        con_color(COL_RED); kprintf("ai: %s\n", fetch_err); con_color(COL_DEFAULT);
        if (strstr(fetch_err, "certificate")) kprintf("  (the CA of api.anthropic.com is not in RoboOS: put its root as /TLS/CA.PEM)\n");
        return;
    }
    resp[n] = 0;
    if (status != 200) {
        const char *m = jkey(resp, "message");
        con_color(COL_RED); kprintf("ai: HTTP %d", status);
        if (m && *m == '"') { jstr(m + 1, text, 300, 0); kprintf(" - %s", text); }
        kprintf("\n"); con_color(COL_DEFAULT);
        if (status == 401) kprintf("  the API key is wrong: 'ai key <key>' (console.anthropic.com -> API keys)\n");
        if (status == 404) kprintf("  unknown model? 'ai model %s' sets the default again\n", DEF_MODEL);
        if (status == 429 || status == 529) kprintf("  too many requests / overloaded - try again in a while\n");
        return;
    }
    /* the text blocks of content[] */
    int tn = 0;
    const char *c = jkey(resp, "content");
    for (const char *p = c; p && (p = jkey(p, "type")); ) {
        if (!strncmp(p, "\"text\"", 6)) {
            const char *v = jkey(p, "text");
            if (v && *v == '"') { int l; jstr(v + 1, text + tn, (int)sizeof(text) - tn, &l); tn += l; p = v + 1; continue; }
        }
        p++;
    }
    text[tn] = 0;
    const char *sr = jkey(resp, "stop_reason");
    con_color(COL_CYAN);
    if (tn) kprintf("%s\n", text);
    con_color(COL_DEFAULT);
    if (sr && !strncmp(sr, "\"refusal\"", 9)) kprintf("(the model declined to answer this)\n");
    else if (sr && !strncmp(sr, "\"max_tokens\"", 12)) kprintf("(the answer was cut off)\n");
    else if (!tn) kprintf("(an empty answer)\n");
    long in = jnum(resp, "input_tokens"), out = jnum(resp, "output_tokens");
    con_color(COL_GRAY);
    kprintf("(%u.%u s, %ld tokens in, %ld out)\n", (u32)((timer_us() - t0) / 1000000), (u32)((timer_us() - t0) / 100000 % 10), in, out);
    con_color(COL_DEFAULT);
}

/* ---------------- the shell command ---------------- */

void ai_cmd(int argc, char **argv)
{
    if (!loaded) load();
    if (argc < 2) {
        kprintf("AI (Claude by Anthropic): model %s, key %s\n", model, key[0] ? "set" : "NOT set");
        if (key[0]) {
            int l = (int)strlen(key);
            kprintf("  key ...%s (in /AI.TXT, scrambled)\n", l > 4 ? key + l - 4 : "");
        }
        kprintf("usage: ai <question>             ask\n"
                "       ai @<command> <question>  run a command and ask about its output (ai @wifi why is it slow)\n"
                "       ai key <key> | ai key off the API key (console.anthropic.com)\n"
                "       ai model [<id>]           model, default %s (the cheapest)\n", DEF_MODEL);
        return;
    }
    if (!strcmp(argv[1], "key") && argc == 3) {
        if (!strcmp(argv[2], "off")) { memset(key, 0, sizeof(key)); save(); kprintf("key removed\n"); return; }
        if (strncmp(argv[2], "sk-", 3) || strlen(argv[2]) >= sizeof(key)) { kprintf("that does not look like an API key (sk-ant-...)\n"); return; }
        memcpy(key, argv[2], strlen(argv[2]) + 1);
        save();
        kprintf("key saved in /AI.TXT (scrambled). Clear the screen ('clear') so it is not left visible.\n");
        return;
    }
    if (!strcmp(argv[1], "model") && argc <= 3) {
        if (argc == 3) {
            if (strlen(argv[2]) >= sizeof(model)) { kprintf("too long\n"); return; }
            memcpy(model, argv[2], strlen(argv[2]) + 1);
        } else memcpy(model, DEF_MODEL, sizeof(DEF_MODEL));
        save();
        kprintf("model %s\n", model);
        return;
    }
    if (!key[0]) { kprintf("no API key yet: 'ai key sk-ant-...' (make one at console.anthropic.com)\n"); return; }
    static char qb[1024], cl[256];
    int n = 0, first = 1;
    const char *cmdline = 0;
    if (argv[1][0] == '@' && argv[1][1]) {                 /* @command, or @"command with args" */
        int k = 0;
        for (const char *s = argv[1] + 1; *s && k < (int)sizeof(cl) - 1; s++) cl[k++] = *s;
        cl[k] = 0;
        cmdline = cl;
        first = 2;
        if (!strncmp(cl, "ai", 2) && (cl[2] == 0 || cl[2] == ' ')) { kprintf("not 'ai' itself\n"); return; }
    }
    for (int i = first; i < argc; i++) {
        for (const char *s = argv[i]; *s && n < (int)sizeof(qb) - 2; s++) qb[n++] = *s;
        if (i + 1 < argc) qb[n++] = ' ';
    }
    qb[n] = 0;
    if (!n) { kprintf("what is the question?\n"); return; }
    ask(qb, cmdline);
}
