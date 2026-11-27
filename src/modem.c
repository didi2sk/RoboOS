/*
 * GSM / LTE modem for the alarm (0.144): SIM800L (2G) or SIMCom A7670 / SIM7600 (LTE) on the mini UART
 * (GPIO14 = Pi TX -> modem RX, pin 8; GPIO15 = Pi RX <- modem TX, pin 10), 9600 baud, standard AT commands
 * (3GPP 27.005 / 27.007) that both families understand:
 *   ATE0  AT+CMEE=2  ATI  AT+CPIN?  AT+CMGF=1  AT+CSCS="GSM"  AT+CNMI=2,1,0,0,0  AT+CLIP=1  AT+COPS=3,0
 *   AT+CSQ  AT+CREG?  AT+COPS?  AT+CMGS="<n>" (> text Ctrl+Z)  +CMTI -> AT+CMGR=<i> + AT+CMGD=<i>  ATD<n>;  ATH
 * SIM800 finds the speed by itself (auto-baud); a modem that does not answer at 9600 is tried at 115200 and
 * set to 9600 with AT+IPR=9600 + AT&W (A7670 / SIM7600 start at 115200).
 *
 * Why 9600: the mini UART has an 8-byte receive FIFO; at 9600 that is 8 ms, long enough for core 1, which empties
 * it into a ring (modem_rx_pump from io_poll_all). Core 0 (modem_poll from con_trygetc) does the AT dialogue as a
 * state machine - it never waits, so the alarm keeps running while an SMS goes out (up to a minute).
 *
 * What it does: SMS to the numbers in /MODEM.TXT on alarm / arm / disarm (each can be off), calls them one by one
 * in an alarm (stopped by a disarm), and takes SMS commands from those numbers only: ARM <code>, HOME <code>,
 * DISARM <code>, STATUS, HELP (answered by SMS). The modem's own power (3.4-4.4 V, 2 A peaks) must NOT come from the Pi.
 */
#include "kernel.h"
#include "sd.h"

#define MAXNUM 5

static void scopy(char *d, const char *s, int max) { int i = 0; for (; s && s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static const char *strrchr_(const char *s, char c) { const char *r = 0; for (; *s; s++) if (*s == c) r = s; return r; }
#define strrchr strrchr_
#define RXSZ 2048

static int on, ready, loaded;
static char nums[MAXNUM][20];
static int nn;
static char pin[12];
static int sms_alarm = 1, sms_arm = 1, sms_disarm = 1, do_call = 1, cmds = 1;

/* status */
static char model[40], oper[28], lasterr[48];
static int csq = 99, reg = -1, sim = 0, baud = 9600, probes, misses;
static u32 sent_ok, sent_fail, got_in, calls;

/* core 1 -> core 0 ring */
static u8 rx[RXSZ];
static volatile int rxh, rxt;

void modem_rx_pump(void)
{
    if (!on) return;
    for (int i = 0; i < 64; i++) {
        int c = uart_modem_getc();
        if (c < 0) break;
        int h = rxh, n = (h + 1) % RXSZ;
        if (n == rxt) break;                              /* full: drop */
        rx[h] = (u8)c;
        __atomic_store_n(&rxh, n, __ATOMIC_RELEASE);
    }
}

static int rx_get(void)
{
    if (!io_core_active()) modem_rx_pump();
    if (rxt == __atomic_load_n(&rxh, __ATOMIC_ACQUIRE)) return -1;
    int c = rx[rxt];
    __atomic_store_n(&rxt, (rxt + 1) % RXSZ, __ATOMIC_RELEASE);
    return c;
}

static void tx(const char *s) { for (; *s; s++) uart_modem_putc((u8)*s); }

/* ---------------- jobs ---------------- */

enum { J_AT, J_SMS, J_DIAL, J_READ };
enum { P_NONE, P_PROBE, P_ATI, P_CPIN, P_CSQ, P_CREG, P_COPS, P_READ, P_RAW, P_IPR, P_LAST };
struct job { u8 type, purpose; char a[24]; char text[164]; int ms; };
#define NJ 16
static struct job jq[NJ];
static int jh, jt;
static int jlk;
static void jlock(void) { while (__atomic_test_and_set(&jlk, __ATOMIC_ACQUIRE)) ; }
static void junlock(void) { __atomic_clear(&jlk, __ATOMIC_RELEASE); }

static int push(int type, int purpose, const char *a, const char *text, int ms)
{
    jlock();
    int n = (jh + 1) % NJ, r = -1;
    if (n != jt) {
        struct job *j = &jq[jh];
        memset(j, 0, sizeof(*j));
        j->type = (u8)type; j->purpose = (u8)purpose; j->ms = ms;
        if (a) scopy(j->a, a, sizeof(j->a));
        if (text) scopy(j->text, text, sizeof(j->text));
        jh = n; r = 0;
    }
    junlock();
    return r;
}
static int at(const char *c, int purpose, int ms) { return push(J_AT, purpose, c, 0, ms); }

/* the job being run */
static struct job cur;
static int busy, phase;                                   /* phase: J_SMS 1 = waiting for '>', 2 = for OK */
static u64 deadline;
static char rbuf[4][100];                                 /* lines collected for the current job */
static int rn;
static char rd_from[24];

/* calls in an alarm */
static int call_idx = -1;
static u64 call_next, hang_at;

/* ---------------- config ---------------- */

static int digits_tail_same(const char *a, const char *b)   /* the last 9 digits are the same (+421 / 0 prefixes) */
{
    char x[24], y[24]; int nx = 0, ny = 0;
    for (; *a && nx < 23; a++) if (*a >= '0' && *a <= '9') x[nx++] = *a;
    for (; *b && ny < 23; b++) if (*b >= '0' && *b <= '9') y[ny++] = *b;
    if (nx < 6 || ny < 6) return 0;
    int k = nx < ny ? nx : ny; if (k > 9) k = 9;
    for (int i = 1; i <= k; i++) if (x[nx - i] != y[ny - i]) return 0;
    return 1;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static void load(void)
{
    loaded = 1;
    struct fat_file f;
    static char b[1024];
    if (!fat_mounted() || fat_open("/MODEM.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    nn = 0;
    for (char *p = b; *p; ) {
        char *l = p; while (*p && *p != '\n') p++; if (*p) *p++ = 0;
        while (is_space(*l)) l++;
        char *v = l; while (*v && !is_space(*v)) v++;
        if (*v) { *v++ = 0; while (is_space(*v)) v++; }
        char *e = v + strlen(v); while (e > v && is_space(e[-1])) *--e = 0;
        int yes = !strcmp(v, "on") || !strcmp(v, "1");
        if (!strcmp(l, "state")) on = yes;
        else if (!strcmp(l, "number") && *v && nn < MAXNUM) scopy(nums[nn++], v, sizeof(nums[0]));
        else if (!strcmp(l, "pin")) scopy(pin, strcmp(v, "none") ? v : "", sizeof(pin));
        else if (!strcmp(l, "sms_alarm")) sms_alarm = yes;
        else if (!strcmp(l, "sms_arm")) sms_arm = yes;
        else if (!strcmp(l, "sms_disarm")) sms_disarm = yes;
        else if (!strcmp(l, "call")) do_call = yes;
        else if (!strcmp(l, "commands")) cmds = yes;
    }
}

static int save(void)
{
    static char b[1024];
    int n = 0;
    #define P(s) do { const char *s_ = (s); while (*s_ && n < (int)sizeof(b) - 1) b[n++] = *s_++; } while (0)
    #define ONOFF(k, v) do { P(k); P((v) ? " on\r\n" : " off\r\n"); } while (0)
    P("# RoboOS GSM/LTE modem (shell 'modem'): SIM800L / A7670 / SIM7600 on GPIO14/15 (pins 8/10)\r\n");
    ONOFF("state", on);
    for (int i = 0; i < nn; i++) { P("number "); P(nums[i]); P("\r\n"); }
    P("pin "); P(pin[0] ? pin : "none"); P("\r\n");
    ONOFF("sms_alarm", sms_alarm); ONOFF("sms_arm", sms_arm); ONOFF("sms_disarm", sms_disarm);
    ONOFF("call", do_call); ONOFF("commands", cmds);
    #undef P
    #undef ONOFF
    b[n] = 0;
    return fat_mounted() ? fat_write_file("/MODEM.TXT", b, (u32)n) : -1;
}

/* ---------------- start / stop ---------------- */

static void start(void)
{
    jlock(); jh = jt = 0; junlock();
    busy = 0; ready = 0; probes = 0; misses = 0; baud = 9600;
    model[0] = oper[0] = lasterr[0] = 0; csq = 99; reg = -1; sim = 0;
    rxh = rxt = 0;
    uart_modem_open(9600);
    at("AT", P_PROBE, 600);
}

static void stop(void)
{
    if (!uart_modem_owned()) return;
    uart_modem_close();
    ready = 0;
}

void modem_boot(void)
{
    load();
    if (!on) return;
    kprintf("modem: on GPIO14/15 (pins 8 TX / 10 RX), 9600 baud - the serial console is off now ('modem off' gives it back)\n");
    start();
}

static void init_cmds(void)                               /* after the first OK */
{
    at("ATE0", P_NONE, 1000);
    at("AT+CMEE=2", P_NONE, 1000);
    at("ATI", P_ATI, 1000);
    at("AT+CPIN?", P_CPIN, 5000);
}

static void after_sim(void)
{
    at("AT+CMGF=1", P_NONE, 2000);
    at("AT+CSCS=\"GSM\"", P_NONE, 2000);
    at("AT+CNMI=2,1,0,0,0", P_NONE, 2000);
    at("AT+CLIP=1", P_NONE, 2000);
    at("AT+COPS=3,0", P_NONE, 2000);
    at("AT+CMGD=1,4", P_NONE, 10000);                     /* old messages out (a stale DISARM must not run now) */
    at("AT+CSQ", P_CSQ, 2000);
    at("AT+CREG?", P_CREG, 2000);
    at("AT+COPS?", P_COPS, 10000);
    ready = 1;
}

/* ---------------- what the modem says ---------------- */

static void sms_in(const char *from, const char *text);

static void job_end(int ok)
{
    busy = 0;
    if (cur.type == J_SMS) {
        if (ok) sent_ok++; else sent_fail++;
        if (!ok) kprintf("\n[MODEM] SMS to %s NOT sent: %s\n", cur.a, lasterr[0] ? lasterr : "no answer");
        return;
    }
    if (cur.type == J_DIAL) {
        if (ok) { hang_at = timer_us() + 25000000; calls++; }
        else call_next = timer_us() + 3000000;
        return;
    }
    if (!ok) {
        if (cur.purpose == P_PROBE) {                     /* nobody at 9600: try 115200 once */
            if (++probes == 3) { baud = 115200; uart_modem_baud(115200); }
            if (probes == 6) { baud = 9600; uart_modem_baud(9600); probes = 0;
                               scopy(lasterr, "the modem does not answer (wires? power?)", sizeof(lasterr)); }
            at("AT", P_PROBE, 600);
        } else if (cur.purpose == P_CSQ || cur.purpose == P_CREG) {
            if (++misses >= 3) { kprintf("\n[MODEM] no answer - starting again\n"); misses = 0; start(); }
        } else if (cur.purpose == P_RAW) kprintf("[MODEM] %s\n", lasterr[0] ? lasterr : "no answer");
        return;
    }
    misses = 0;
    switch (cur.purpose) {
    case P_PROBE:
        if (baud != 9600) {                               /* an LTE module at 115200: switch it to 9600 for good */
            at("AT+IPR=9600", P_IPR, 1000);
            return;
        }
        lasterr[0] = 0;
        init_cmds();
        break;
    case P_IPR:
        at("AT&W", P_NONE, 1000);
        baud = 9600;
        uart_modem_baud(9600);                            /* the modem changes speed after the OK */
        at("AT", P_PROBE, 600);
        break;
    case P_ATI:
        for (int i = 0; i < rn; i++) if (rbuf[i][0] && strcmp(rbuf[i], "ATI")) { scopy(model, rbuf[i], sizeof(model)); break; }
        break;
    case P_CPIN:
        sim = 0;
        for (int i = 0; i < rn; i++) {
            if (strstr(rbuf[i], "READY")) sim = 1;
            else if (strstr(rbuf[i], "SIM PIN")) sim = 2;
        }
        if (sim == 2 && pin[0]) {
            char c[24] = "AT+CPIN=\""; int n = 9;
            for (const char *p = pin; *p && n < 20; p++) c[n++] = *p;
            c[n++] = '"'; c[n] = 0;
            at(c, P_NONE, 5000);
            at("AT+CPIN?", P_CPIN, 5000);
        } else if (sim == 1) after_sim();
        else scopy(lasterr, sim == 2 ? "the SIM wants a PIN ('modem pin <pin>')" : "no SIM card?", sizeof(lasterr));
        break;
    case P_CSQ:
        for (int i = 0; i < rn; i++) if (!strncmp(rbuf[i], "+CSQ:", 5)) {
            int v = 0; const char *p = rbuf[i] + 5; while (*p == ' ') p++;
            while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            csq = v;
        }
        break;
    case P_CREG:
        for (int i = 0; i < rn; i++) if (!strncmp(rbuf[i], "+CREG:", 6) || !strncmp(rbuf[i], "+CEREG:", 7)) {
            const char *p = strchr(rbuf[i], ',');
            if (p) reg = p[1] - '0';
        }
        break;
    case P_COPS:
        for (int i = 0; i < rn; i++) if (!strncmp(rbuf[i], "+COPS:", 6)) {
            const char *q = strchr(rbuf[i], '"');
            if (q) { int n = 0; for (q++; *q && *q != '"' && n < (int)sizeof(oper) - 1; q++) oper[n++] = *q; oper[n] = 0; }
            else oper[0] = 0;
        }
        break;
    case P_READ:
        if (rd_from[0] && rn > 1) sms_in(rd_from, rbuf[1]);
        break;
    case P_RAW:
        kprintf("OK\n");
        break;
    }
}

static void line(const char *l)
{
    if (!l[0]) return;
    /* unsolicited */
    if (!strncmp(l, "+CMTI:", 6)) {                       /* +CMTI: "SM",3 */
        const char *c = strrchr(l, ',');
        if (c) {
            char a[24] = "AT+CMGR="; int n = 8;
            for (c++; *c >= '0' && *c <= '9' && n < 20; c++) a[n++] = *c;
            a[n] = 0;
            push(J_READ, P_READ, a, 0, 5000);
        }
        return;
    }
    if (!strcmp(l, "RING") || !strncmp(l, "+CLIP:", 6) || !strcmp(l, "Call Ready") || !strcmp(l, "SMS Ready")
        || (!strncmp(l, "+CPIN: READY", 12) && !busy)) return;
    if (!busy) return;
    if (cur.purpose == P_RAW && strcmp(l, "OK")) kprintf("%s\n", l);
    if (!strcmp(l, "OK")) {
        if (cur.type == J_SMS && phase == 1) return;
        if (cur.type == J_READ) {                         /* read -> delete it from the SIM */
            char d[24] = "AT+CMGD="; scopy(d + 8, cur.a + 8, 16);
            job_end(1);
            at(d, P_NONE, 5000);
            return;
        }
        job_end(1);
        return;
    }
    if (!strcmp(l, "ERROR") || !strncmp(l, "+CMS ERROR", 10) || !strncmp(l, "+CME ERROR", 10)) {
        scopy(lasterr, l, sizeof(lasterr));
        job_end(0);
        return;
    }
    if (cur.type == J_DIAL && (!strcmp(l, "NO CARRIER") || !strcmp(l, "BUSY") || !strcmp(l, "NO ANSWER") || !strcmp(l, "NO DIALTONE"))) {
        scopy(lasterr, l, sizeof(lasterr));
        job_end(0);
        return;
    }
    if (cur.type == J_READ && !strncmp(l, "+CMGR:", 6)) {   /* +CMGR: "REC UNREAD","+421900...","","26/11/27,..." */
        rd_from[0] = 0;
        const char *q = strchr(l, ',');
        if (q && q[1] == '"') { int n = 0; for (q += 2; *q && *q != '"' && n < 23; q++) rd_from[n++] = *q; rd_from[n] = 0; }
    }
    if (rn < 4) scopy(rbuf[rn++], l, sizeof(rbuf[0]));
}

/* ---------------- incoming SMS commands ---------------- */

static void sms_in(const char *from, const char *text)
{
    got_in++;
    int known = 0;
    for (int i = 0; i < nn; i++) if (digits_tail_same(nums[i], from)) known = 1;
    kprintf("\n[MODEM] SMS from %s%s: %s\n", from, known ? "" : " (not in the list - ignored)", text);
    if (!known || !cmds) return;
    char reply[160];
    alarm_sms_cmd(text, from, reply, sizeof(reply));
    push(J_SMS, P_NONE, from, reply, 60000);
}

/* ---------------- the loop (core 0) ---------------- */

static char lb[120];
static int ll;
static u64 next_check;

static void start_job(void)
{
    jlock();
    if (jt == jh) { junlock(); return; }
    cur = jq[jt];
    jt = (jt + 1) % NJ;
    junlock();
    busy = 1; rn = 0; phase = 0;
    deadline = timer_us() + (u64)cur.ms * 1000;
    if (cur.type == J_SMS) {
        tx("AT+CMGS=\""); tx(cur.a); tx("\"\r");
        phase = 1;
        deadline = timer_us() + 5000000;
    } else if (cur.type == J_DIAL) {
        tx("ATD"); tx(cur.a); tx(";\r");
        deadline = timer_us() + 20000000;
    } else {
        tx(cur.a); tx("\r");
    }
}

void modem_poll(void)
{
    static int in;
    if (!loaded) load();
    if (!on || in) return;
    in = 1;
    int c;
    while ((c = rx_get()) >= 0) {
        if (c == '\r') continue;
        if (c == '\n') { lb[ll] = 0; line(lb); ll = 0; continue; }
        if (ll < (int)sizeof(lb) - 1) lb[ll++] = (char)c;
        if (busy && cur.type == J_SMS && phase == 1 && ll >= 1 && lb[0] == '>') {   /* the text prompt */
            ll = 0;
            char t[164]; int n = 0;
            for (const char *p = cur.text; *p && n < 160; p++) t[n++] = (*p >= 32 && *p < 127) ? *p : '?';
            t[n] = 0;
            tx(t);
            uart_modem_putc(0x1A);                        /* Ctrl+Z = send */
            phase = 2;
            deadline = timer_us() + 60000000;
        }
    }
    u64 t = timer_us();
    if (busy && t > deadline) {
        if (cur.type == J_SMS && phase == 1) uart_modem_putc(0x1B);   /* Esc: leave the text prompt */
        scopy(lasterr, "timeout", sizeof(lasterr));
        job_end(0);
    }
    if (!busy) start_job();
    if (ready && t >= next_check) {                       /* signal + network every 30 s */
        next_check = t + 30000000;
        at("AT+CSQ", P_CSQ, 2000);
        at("AT+CREG?", P_CREG, 2000);
        at("AT+COPS?", P_COPS, 10000);
    }
    /* calls in an alarm: one number after the other, 25 s each */
    if (hang_at && t >= hang_at) { hang_at = 0; at("ATH", P_NONE, 5000); call_next = t + 3000000; }
    if (call_idx >= 0 && !hang_at && t >= call_next && !busy) {
        if (call_idx >= nn) call_idx = -1;
        else { push(J_DIAL, P_NONE, nums[call_idx++], 0, 20000); call_next = ~0ULL; }
    }
    in = 0;
}

/* from alarm.c (any core): an event to report */
void modem_event(int kind, const char *text)
{
    if (!on || !nn) return;
    if ((kind == 1 && !sms_alarm) || (kind == 2 && !sms_arm) || (kind == 3 && !sms_disarm)) {
        if (kind == 3) { call_idx = -1; if (hang_at) hang_at = 1; }
        if (!(kind == 1 && do_call)) return;
    }
    char m[160]; int n = 0;
    const char *nm = net_name();
    for (const char *p = nm; *p && n < 20; p++) m[n++] = *p;
    u64 now = clock_now();
    if (now) { char tb[32]; clock_format(tb, now, 0); m[n++] = ' '; for (int i = 11; i < 16 && tb[i]; i++) m[n++] = tb[i]; }
    m[n++] = ':'; m[n++] = ' ';
    for (const char *p = text; *p && n < 159; p++) m[n++] = *p;
    m[n] = 0;
    if ((kind == 1 && sms_alarm) || (kind == 2 && sms_arm) || (kind == 3 && sms_disarm) || kind == 4)
        for (int i = 0; i < nn; i++) push(J_SMS, P_NONE, nums[i], m, 60000);
    if (kind == 1 && do_call) { call_idx = 0; call_next = timer_us() + 2000000; }
    if (kind == 3) { call_idx = -1; if (hang_at) hang_at = 1; }   /* disarmed: stop calling, hang up now */
}

int modem_info(char *b, int max)
{
    if (!loaded) load();
    if (!on) { scopy(b, "off", max); return 0; }
    char t[160]; int n = 0;
    #define A(s) do { const char *s_ = (s); while (*s_ && n < (int)sizeof(t) - 1) t[n++] = *s_++; } while (0)
    A(ready ? "ready" : "starting");
    if (oper[0]) { A(", "); A(oper); }
    if (csq < 99) { char d[16]; int v = -113 + 2 * csq, k = 0; char r[8]; int x = -v;
        d[k++] = ','; d[k++] = ' '; d[k++] = '-'; int q = 0; do { r[q++] = (char)('0' + x % 10); x /= 10; } while (x);
        while (q) d[k++] = r[--q]; d[k] = 0; A(d); A(" dBm"); }
    if (reg == 1) A(", home network"); else if (reg == 5) A(", roaming"); else if (reg == 2) A(", searching");
    else if (reg == 3) A(", REFUSED by the network"); else if (reg == 0) A(", not registered");
    if (lasterr[0]) { A(" - "); A(lasterr); }
    t[n] = 0;
    #undef A
    scopy(b, t, max);
    return 1;
}

/* ---------------- shell ---------------- */

static void show(void)
{
    char b[160];
    modem_info(b, sizeof(b));
    kprintf("modem: %s\n", b);
    if (on) kprintf("  model %s, SIM %s, %u SMS sent, %u failed, %u received, %u calls\n", model[0] ? model : "?",
                    sim == 1 ? "ready" : sim == 2 ? "wants a PIN" : "?", sent_ok, sent_fail, got_in, calls);
    kprintf("  numbers:");
    for (int i = 0; i < nn; i++) kprintf(" %s", nums[i]);
    kprintf("%s\n", nn ? "" : " none ('modem number add +421...')");
    kprintf("  SMS on alarm %s, arm %s, disarm %s; call on alarm %s; SMS commands %s (ARM/HOME/DISARM <code>, STATUS)\n",
            sms_alarm ? "on" : "off", sms_arm ? "on" : "off", sms_disarm ? "on" : "off", do_call ? "on" : "off", cmds ? "on" : "off");
    if (!on) kprintf("  wiring: modem RX <- pin 8 (GPIO14), modem TX -> pin 10 (GPIO15), GND; the modem's own 3.7-4.2 V / 2 A supply\n");
}

void modem_cmd(int argc, char **argv)
{
    if (!loaded) load();
    const char *a = argc > 1 ? argv[1] : "";
    int changed = 0;
    if (!*a) { show(); return; }
    if (!strcmp(a, "on") || !strcmp(a, "off")) {
        int w = a[1] == 'n';
        if (w && !on) { on = 1; start(); kprintf("modem on: the serial console is off now (HDMI + keyboard stay)\n"); }
        else if (!w && on) { on = 0; stop(); kprintf("modem off: the serial console is back\n"); }
        changed = 1;
    } else if (!strcmp(a, "number") && argc > 3) {
        if (!strcmp(argv[2], "add")) {
            if (nn >= MAXNUM) { kprintf("at most %d numbers\n", MAXNUM); return; }
            scopy(nums[nn++], argv[3], sizeof(nums[0]));
        } else if (!strcmp(argv[2], "del")) {
            int k = -1; for (int i = 0; i < nn; i++) if (digits_tail_same(nums[i], argv[3])) k = i;
            if (k < 0) { kprintf("not in the list\n"); return; }
            for (int i = k; i < nn - 1; i++) scopy(nums[i], nums[i + 1], sizeof(nums[0]));
            nn--;
        } else { kprintf("modem number add|del <+421...>\n"); return; }
        changed = 1;
    } else if (!strcmp(a, "pin") && argc > 2) {
        scopy(pin, strcmp(argv[2], "none") ? argv[2] : "", sizeof(pin));
        if (on) start();
        changed = 1;
    } else if (!strcmp(a, "set") && argc > 3) {
        int y = !strcmp(argv[3], "on");
        if (!strcmp(argv[2], "sms_alarm")) sms_alarm = y;
        else if (!strcmp(argv[2], "sms_arm")) sms_arm = y;
        else if (!strcmp(argv[2], "sms_disarm")) sms_disarm = y;
        else if (!strcmp(argv[2], "call")) do_call = y;
        else if (!strcmp(argv[2], "commands")) cmds = y;
        else { kprintf("modem set sms_alarm|sms_arm|sms_disarm|call|commands on|off\n"); return; }
        changed = 1;
    } else if (!strcmp(a, "sms") && argc > 3) {
        if (!on) { kprintf("the modem is off ('modem on')\n"); return; }
        char t[164]; int n = 0;
        for (int i = 3; i < argc && n < 160; i++) { for (const char *s = argv[i]; *s && n < 160; s++) t[n++] = *s; if (i + 1 < argc) t[n++] = ' '; }
        t[n] = 0;
        kprintf(push(J_SMS, P_NONE, argv[2], t, 60000) ? "the queue is full\n" : "queued (see 'modem' for sent / failed)\n");
        return;
    } else if (!strcmp(a, "call") && argc > 2) {
        if (!on) { kprintf("the modem is off ('modem on')\n"); return; }
        push(J_DIAL, P_NONE, argv[2], 0, 20000);
        kprintf("calling %s (hangs up after 25 s; 'modem hangup')\n", argv[2]);
        return;
    } else if (!strcmp(a, "hangup")) {
        call_idx = -1; hang_at = 0; at("ATH", P_NONE, 5000);
        return;
    } else if (!strcmp(a, "at") && argc > 2) {
        if (!on) { kprintf("the modem is off ('modem on')\n"); return; }
        char c[100]; int n = 0;
        for (int i = 2; i < argc && n < 96; i++) { for (const char *s = argv[i]; *s && n < 96; s++) c[n++] = *s; if (i + 1 < argc) c[n++] = ' '; }
        c[n] = 0;
        at(c, P_RAW, 10000);
        u64 end = timer_us() + 11000000;                  /* show the answer here */
        while (timer_us() < end) { modem_poll(); if (!busy && jt == jh) break; if (con_break()) break; }
        return;
    } else {
        kprintf("usage: modem                       status (signal, network, SIM, numbers)\n"
                "       modem on | off              the modem on GPIO14/15 (pins 8 TX, 10 RX) - takes the serial console\n"
                "       modem number add|del <n>    who gets the SMS / calls and may send commands (max %d)\n"
                "       modem pin <pin>|none        the SIM PIN\n"
                "       modem set sms_alarm|sms_arm|sms_disarm|call|commands on|off\n"
                "       modem sms <n> <text>        send an SMS (test)\n"
                "       modem call <n> | hangup     call (test)\n"
                "       modem at <command>          any AT command, the answer is printed\n"
                "  SMS commands from the listed numbers: ARM <code>, HOME <code>, DISARM <code>, STATUS, HELP\n", MAXNUM);
        return;
    }
    if (changed) kprintf(save() ? "(not saved: no SD card?)\n" : "(saved in /MODEM.TXT)\n");
    show();
}
