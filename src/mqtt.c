/*
 * MQTT 3.1.1 client (QoS 0) on lwIP raw TCP - the IoT side of RoboOS.
 *
 * /MQTT.TXT (read on core 0 at boot, written by the 'mqtt' command):
 *   broker 192.168.0.10      host name or address ("off" = no MQTT)
 *   port 1883
 *   user name / pass secret  (optional)
 *   topic roboos/rpi3-3456   base topic (default roboos/<board name>)
 *   interval 60              seconds between sensor messages
 *   discovery on             Home Assistant MQTT discovery
 *
 * Topics (all under the base):
 *   status        "online" / "offline" (retained; offline is the last will)
 *   state         JSON every interval: cpu, temp, hum, press, a0-a3, uptime, ip, time
 *   gpio/<n>      the level after a change (retained)
 *   set/gpio/<n>  <- 1 / 0 / on / off / toggle: the pin becomes an output
 *   set/publish   <- send state now
 *   cmd/...       <- kept for programs (mqttReceive, BASIC MQTTRECV), inbox of 8
 * Runs on the I/O core (core 1); core 0 calls in with the io lock held.
 */
#include "kernel.h"
#include "sd.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

static char broker[64], user[48], pass[64], base[64];
static int port = 1883, interval = 60, discovery = 1, cfg_loaded;

enum { M_OFF, M_WAIT, M_DNS, M_CONNECTING, M_CONNACK, M_UP };
static const char *const mname[] = { "off", "waiting to retry", "looking up the name", "connecting", "logging in", "connected" };
static int st = M_OFF;
static struct tcp_pcb *pcb;
static ip_addr_t baddr;
static u64 retry_at, next_state, last_tx, last_rx;
static u16 pkt_id;
static u32 sent, received;
static const char *why = "";
static u8 rxb[1536];
static int rxn;

#define KEEPALIVE 60

/* ---------------- packets ---------------- */

static u8 txb[1536];
static int txn;

static void b_byte(u8 v) { if (txn < (int)sizeof(txb)) txb[txn++] = v; }
static void b_u16(u16 v) { b_byte((u8)(v >> 8)); b_byte((u8)v); }
static void b_str(const char *s) { int n = (int)strlen(s); b_u16((u16)n); for (int i = 0; i < n; i++) b_byte((u8)s[i]); }
static void b_raw(const char *s, int n) { for (int i = 0; i < n; i++) b_byte((u8)s[i]); }

/* type byte + remaining length in front of txb[5..txn) */
static int finish(u8 type)
{
    int len = txn - 5, k = 0;
    u8 h[5];
    h[k++] = type;
    do { u8 d = (u8)(len % 128); len /= 128; if (len) d |= 0x80; h[k++] = d; } while (len && k < 5);
    int start = 5 - k;
    memcpy(txb + start, h, (size_t)k);
    return start;
}

static int send_packet(u8 type)
{
    if (!pcb) return -1;
    int s = finish(type), n = txn - s;
    if (n > (int)tcp_sndbuf(pcb)) return -2;
    if (tcp_write(pcb, txb + s, (u16)n, TCP_WRITE_FLAG_COPY) != ERR_OK) return -3;
    tcp_output(pcb);
    last_tx = timer_us();
    return 0;
}

static void topic(char *out, int max, const char *sub)
{
    int n = 0;
    for (const char *s = base; *s && n < max - 2; s++) out[n++] = *s;
    out[n++] = '/';
    for (const char *s = sub; *s && n < max - 1; s++) out[n++] = *s;
    out[n] = 0;
}

static int publish_raw(const char *t, const char *payload, int plen, int retain)
{
    if (st != M_UP) return -1;
    txn = 5;
    b_str(t);
    b_raw(payload, plen);
    int r = send_packet((u8)(0x30 | (retain ? 1 : 0)));
    if (!r) sent++;
    return r;
}

static int publish(const char *sub, const char *payload, int retain)
{
    char t[128];
    topic(t, sizeof(t), sub);
    return publish_raw(t, payload, (int)strlen(payload), retain);
}

/* ---------------- JSON state ---------------- */

static char js[512];
static int jn;
static void ja(const char *s) { while (*s && jn < (int)sizeof(js) - 1) js[jn++] = *s++; js[jn] = 0; }
static void jnum(long v) { char d[24]; int k = 0; if (v < 0) { ja("-"); v = -v; } do d[k++] = (char)('0' + v % 10); while (v /= 10); while (k) { char c[2] = { d[--k], 0 }; ja(c); } }
static void jfix(long v100) { if (v100 < 0) { ja("-"); v100 = -v100; } jnum(v100 / 100); ja("."); char c[3] = { (char)('0' + v100 % 100 / 10), (char)('0' + v100 % 10), 0 }; ja(c); }

static void publish_state(void)
{
    struct sensor_values v;
    sensors_read(&v);
    jn = 0;
    ja("{");
    if (v.have & SV_CPU) { ja("\"cpu\":"); jfix(v.cpu_mc / 10); ja(","); }
    if (v.have & SV_TEMP) { ja("\"temp\":"); jfix(v.temp_c100); ja(","); }
    if (v.have & SV_HUM) { ja("\"hum\":"); jfix(v.hum_c100); ja(","); }
    if (v.have & SV_PRESS) { ja("\"press\":"); jfix(v.press_pa); ja(","); }          /* hPa with 2 decimals */
    if (v.have & SV_AIN)
        for (int i = 0; i < 4; i++) { char k[8] = "\"a0\":"; k[2] = (char)('0' + i); ja(k); jnum(v.ain_mv[i]); ja(","); }
    ja("\"uptime\":"); jnum((long)(timer_us() / 1000000));
    ja(",\"ip\":\""); ja(ip_address_str()); ja("\"");
    u64 now = clock_now();
    if (now) { char tb[32]; clock_format(tb, now, 1); ja(",\"time\":\""); ja(tb); ja("\""); }
    ja("}");
    publish("state", js, 0);
}

/* Home Assistant: one retained config message per value, so it shows up by itself */
static void ha_one(const char *key, const char *name, const char *unit, const char *cls)
{
    char t[128], node[40];
    int n = 0;
    for (const char *s = net_name(); *s && n < 39; s++) node[n++] = (*s == '-' || *s == '.') ? '_' : *s;
    node[n] = 0;
    jn = 0;
    ja("homeassistant/sensor/"); ja(node); ja("/"); ja(key); ja("/config");
    memcpy(t, js, (size_t)jn + 1);
    jn = 0;
    ja("{\"name\":\""); ja(name); ja("\",\"state_topic\":\""); ja(base); ja("/state\",\"value_template\":\"{{ value_json.");
    ja(key); ja(" }}\",\"unit_of_measurement\":\""); ja(unit); ja("\",");
    if (cls) { ja("\"device_class\":\""); ja(cls); ja("\",\"state_class\":\"measurement\","); }
    ja("\"unique_id\":\""); ja(node); ja("_"); ja(key); ja("\",\"availability_topic\":\""); ja(base);
    ja("/status\",\"device\":{\"identifiers\":[\""); ja(node); ja("\"],\"name\":\""); ja(net_name());
    ja("\",\"model\":\""); ja(board_name()); ja("\",\"manufacturer\":\"RoboOS\",\"sw_version\":\""); ja(os_version()); ja("\"}}");
    publish_raw(t, js, jn, 1);
}

static void ha_discovery(void)
{
    int h = sensors_have();
    ha_one("cpu", "CPU temperature", "\\u00b0C", "temperature");
    if (h & 1) ha_one("temp", "Temperature", "\\u00b0C", "temperature");
    if (h & 2) ha_one("hum", "Humidity", "%", "humidity");
    if (h & 1) ha_one("press", "Pressure", "hPa", "pressure");
    if (h & 4) { ha_one("a0", "A0", "mV", "voltage"); ha_one("a1", "A1", "mV", "voltage"); ha_one("a2", "A2", "mV", "voltage"); ha_one("a3", "A3", "mV", "voltage"); }
}

/* ---------------- inbox for programs (cmd/...) ---------------- */

#define INBOX 8
static struct { char topic[64]; char payload[128]; int plen; } inbox[INBOX];
static int ih, it;

static void incoming(const char *t, int tl, const char *p, int pl)
{
    char sub[96];
    int bl = (int)strlen(base);
    if (tl <= bl + 1 || memcmp_n(t, base, (size_t)bl) || t[bl] != '/') return;
    int sl = tl - bl - 1;
    if (sl > (int)sizeof(sub) - 1) sl = (int)sizeof(sub) - 1;
    memcpy(sub, t + bl + 1, (size_t)sl);
    sub[sl] = 0;
    received++;
    if (!strncmp(sub, "set/gpio/", 9)) {
        int pin = 0, k = 9;
        while (sub[k] >= '0' && sub[k] <= '9') pin = pin * 10 + (sub[k++] - '0');
        if (sub[k] || k == 9 || pin > 27) return;
        char v[8] = { 0 };
        int n = pl < 7 ? pl : 7;
        for (int i = 0; i < n; i++) v[i] = (char)(p[i] | 0x20);
        int on = !strcmp(v, "1") || !strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "high");
        int off = !strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "low");
        if (!strcmp(v, "toggle")) { gpio_func(pin, 1); gpio_set(pin, !gpio_get(pin)); }
        else if (on || off) { gpio_func(pin, 1); gpio_set(pin, on); }
        else return;
        char gt[16] = "gpio/";
        int gn = 5;
        if (pin >= 10) gt[gn++] = (char)('0' + pin / 10);
        gt[gn++] = (char)('0' + pin % 10);
        gt[gn] = 0;
        publish(gt, gpio_get(pin) ? "1" : "0", 1);
        return;
    }
    if (!strcmp(sub, "set/publish")) { publish_state(); return; }
    if (!strncmp(sub, "cmd/", 4) || !strcmp(sub, "cmd")) {
        int nx = (ih + 1) % INBOX;
        if (nx == it) return;                             /* full: dropped */
        int n = 0;
        for (const char *s = sub; *s && n < 63; s++) inbox[ih].topic[n++] = *s;
        inbox[ih].topic[n] = 0;
        n = pl < 127 ? pl : 127;
        memcpy(inbox[ih].payload, p, (size_t)n);
        inbox[ih].payload[n] = 0;
        inbox[ih].plen = n;
        ih = nx;
    }
}

/* ---------------- connection ---------------- */

static void drop(const char *reason, int abort)
{
    if (pcb) {
        tcp_arg(pcb, 0); tcp_recv(pcb, 0); tcp_err(pcb, 0); tcp_sent(pcb, 0);
        if (abort) tcp_abort(pcb); else if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
        pcb = 0;
    }
    if (st == M_UP) { con_color(COL_YELLOW); kprintf("\n[MQTT] disconnected: %s\n", reason); con_color(COL_DEFAULT); }
    why = reason;
    st = broker[0] ? M_WAIT : M_OFF;
    retry_at = timer_us() + 10000000ULL;
    rxn = 0;
}

static void got_packet(u8 type, const u8 *p, int len)
{
    last_rx = timer_us();
    switch (type >> 4) {
    case 2:                                               /* CONNACK */
        if (len >= 2 && p[1] == 0) {
            st = M_UP;
            why = "";
            /* online, the subscriptions, Home Assistant, a first state */
            publish("status", "online", 1);
            char t[128];
            txn = 5;
            b_u16(++pkt_id);
            topic(t, sizeof(t), "set/#"); b_str(t); b_byte(0);
            topic(t, sizeof(t), "cmd/#"); b_str(t); b_byte(0);
            send_packet(0x82);
            if (discovery) ha_discovery();
            next_state = timer_us();
            con_color(COL_CYAN); kprintf("\n[MQTT] connected to %s, topic %s/...\n", broker, base); con_color(COL_DEFAULT);
        } else {
            static const char *const rc[] = { "", "bad protocol", "client id refused", "broker unavailable", "bad user name or password", "not authorized" };
            drop(len >= 2 && p[1] < 6 ? rc[p[1]] : "refused", 0);
        }
        break;
    case 3: {                                             /* PUBLISH */
        if (len < 2) break;
        int tl = p[0] << 8 | p[1];
        int off = 2 + tl + (((type >> 1) & 3) ? 2 : 0);   /* QoS 1/2 carry a packet id */
        if (off > len) break;
        incoming((const char *)p + 2, tl, (const char *)p + off, len - off);
        break;
    }
    default:                                              /* SUBACK, PINGRESP, ... */
        break;
    }
}

static err_t m_recv(void *arg, struct tcp_pcb *tp, struct pbuf *pb, err_t err)
{
    (void)arg; (void)err;
    if (!pb) { drop("the broker closed the connection", 0); return ERR_OK; }
    int n = pb->tot_len;
    if (rxn + n > (int)sizeof(rxb)) { pbuf_free(pb); tcp_recved(tp, (u16)n); drop("message too long", 1); return ERR_ABRT; }
    pbuf_copy_partial(pb, rxb + rxn, (u16)n, 0);
    rxn += n;
    tcp_recved(tp, (u16)n);
    pbuf_free(pb);
    /* whole packets out of the buffer */
    for (;;) {
        if (rxn < 2) break;
        int len = 0, mul = 1, k = 1;
        while (k < rxn && k < 5) { len += (rxb[k] & 0x7F) * mul; mul *= 128; if (!(rxb[k++] & 0x80)) break; }
        if (k >= rxn && (rxb[k - 1] & 0x80)) break;
        if (k + len > rxn) break;
        got_packet(rxb[0], rxb + k, len);
        if (!pcb) return ERR_OK;
        memmove(rxb, rxb + k + len, (size_t)(rxn - k - len));
        rxn -= k + len;
    }
    return ERR_OK;
}

static void m_err(void *arg, err_t err)
{
    (void)arg; (void)err;
    pcb = 0;                                              /* lwIP freed it */
    drop("connection error", 0);
}

static err_t m_connected(void *arg, struct tcp_pcb *tp, err_t err)
{
    (void)arg; (void)tp;
    if (err != ERR_OK) { drop("cannot connect", 0); return ERR_OK; }
    char will[128], id[64];
    topic(will, sizeof(will), "status");
    int n = 0;
    const char *pre = "roboos-";
    while (*pre) id[n++] = *pre++;
    for (const char *s = net_name(); *s && n < 63; s++) id[n++] = *s;
    id[n] = 0;
    txn = 5;
    b_str("MQTT");
    b_byte(4);                                            /* 3.1.1 */
    b_byte((u8)(0x02 | 0x04 | 0x20 | (user[0] ? 0x80 : 0) | (pass[0] ? 0x40 : 0)));   /* clean, will (retained) */
    b_u16(KEEPALIVE);
    b_str(id);
    b_str(will);
    b_str("offline");
    if (user[0]) b_str(user);
    if (pass[0]) b_str(pass);
    st = M_CONNACK;
    last_rx = timer_us();
    if (send_packet(0x10)) drop("cannot send CONNECT", 1);
    return pcb ? ERR_OK : ERR_ABRT;
}

static void connect_to(const ip_addr_t *a)
{
    pcb = tcp_new();
    if (!pcb) { drop("no memory", 0); return; }
    tcp_arg(pcb, 0);
    tcp_recv(pcb, m_recv);
    tcp_err(pcb, m_err);
    st = M_CONNECTING;
    last_rx = timer_us();
    if (tcp_connect(pcb, a, (u16)port, m_connected) != ERR_OK) drop("cannot connect", 1);
}

static void m_dns(const char *name, const ip_addr_t *a, void *arg)
{
    (void)name; (void)arg;
    if (st != M_DNS) return;
    if (!a) { drop("broker name not found", 0); return; }
    ip_addr_copy(baddr, *a);
    connect_to(&baddr);
}

/* from ip_poll (core 1): connect, keep alive, send the state */
void mqtt_poll(int have_ip)
{
    u64 now = timer_us();
    if (st == M_OFF) return;
    if (!have_ip) { if (st != M_WAIT) drop("no network", 1); return; }
    switch (st) {
    case M_WAIT:
        if (now < retry_at) return;
        st = M_DNS;
        last_rx = now;
        {
            err_t e = dns_gethostbyname(broker, &baddr, m_dns, 0);
            if (e == ERR_OK) connect_to(&baddr);
            else if (e != ERR_INPROGRESS) drop("broker name not found", 0);
        }
        return;
    case M_DNS: case M_CONNECTING: case M_CONNACK:
        if (now - last_rx > 15000000ULL) drop("no answer from the broker", 1);
        return;
    case M_UP:
        if (now - last_rx > (KEEPALIVE + KEEPALIVE / 2) * 1000000ULL) { drop("broker silent (keep-alive)", 1); return; }
        if (now - last_tx > (KEEPALIVE / 2) * 1000000ULL) { txn = 5; send_packet(0xC0); }   /* PINGREQ */
        if (interval > 0 && now >= next_state) { publish_state(); next_state = now + (u64)interval * 1000000; }
        return;
    }
}

/* ---------------- configuration ---------------- */

static void default_base(void)
{
    int n = 0;
    for (const char *s = "roboos/"; *s; s++) base[n++] = *s;
    for (const char *s = net_name(); *s && n < 63; s++) base[n++] = *s;
    base[n] = 0;
}

static void scopy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }

/* core 0, at boot */
void mqtt_prepare(void)
{
    struct fat_file f;
    char b[512];
    cfg_loaded = 1;
    default_base();
    if (!fat_mounted() || fat_open("/MQTT.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    for (char *l = b; l && *l; ) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        int k = (int)strlen(l);
        while (k && (l[k - 1] == '\r' || l[k - 1] == ' ')) l[--k] = 0;
        char *v = strchr(l, ' ');
        if (v) { *v++ = 0; while (*v == ' ') v++; }
        if (v) {
            u64 num = 0;
            if (!strcmp(l, "broker")) scopy(broker, strcmp(v, "off") ? v : "", sizeof(broker));
            else if (!strcmp(l, "port") && parse_num(v, &num)) port = (int)num;
            else if (!strcmp(l, "user")) scopy(user, v, sizeof(user));
            else if (!strcmp(l, "pass")) scopy(pass, v, sizeof(pass));
            else if (!strcmp(l, "topic")) scopy(base, v, sizeof(base));
            else if (!strcmp(l, "interval") && parse_num(v, &num)) interval = (int)num;
            else if (!strcmp(l, "discovery")) discovery = !strcmp(v, "on");
        }
        l = e ? e + 1 : 0;
    }
    if (broker[0]) { st = M_WAIT; retry_at = 0; }
}

static void save(void)
{
    char b[400];
    int n = 0;
#define ADD(s) do { const char *q_ = (s); while (*q_ && n < 390) b[n++] = *q_++; } while (0)
    char num[12];
    ADD("broker "); ADD(broker[0] ? broker : "off"); ADD("\r\nport ");
    { int k = 0, v = port; char d[8]; do d[k++] = (char)('0' + v % 10); while (v /= 10); int j = 0; while (k) num[j++] = d[--k]; num[j] = 0; } ADD(num);
    if (user[0]) { ADD("\r\nuser "); ADD(user); }
    if (pass[0]) { ADD("\r\npass "); ADD(pass); }
    ADD("\r\ntopic "); ADD(base);
    ADD("\r\ninterval ");
    { int k = 0, v = interval; char d[8]; do d[k++] = (char)('0' + v % 10); while (v /= 10); int j = 0; while (k) num[j++] = d[--k]; num[j] = 0; } ADD(num);
    ADD("\r\ndiscovery "); ADD(discovery ? "on" : "off"); ADD("\r\n");
#undef ADD
    if (!fat_mounted() || fat_write_file("/MQTT.TXT", b, (u32)n)) kprintf("(not saved: no SD card or write error)\n");
}

/* ---------------- for programs, BASIC and the shell (core 0, io lock taken here) ---------------- */

int mqtt_connected(void) { return st == M_UP; }

/* topic: relative to the base, or absolute when it starts with '/' */
int mqtt_publish(const char *t, const char *payload, int len, int retain)
{
    char full[128];
    if (t[0] == '/') scopy(full, t + 1, sizeof(full));
    else topic(full, sizeof(full), t);
    io_lock();
    int r = publish_raw(full, payload, len, retain);
    io_unlock();
    return r;
}

/* the next message under cmd/: topic (after the base) and payload; payload length or -1 */
int mqtt_receive(char *t, int tmax, char *p, int pmax)
{
    io_lock();
    if (it == ih) { io_unlock(); return -1; }
    scopy(t, inbox[it].topic, tmax);
    int n = inbox[it].plen < pmax - 1 ? inbox[it].plen : pmax - 1;
    memcpy(p, inbox[it].payload, (size_t)n);
    p[n] = 0;
    it = (it + 1) % INBOX;
    io_unlock();
    return n;
}

void mqtt_cmd(int argc, char **argv)
{
    if (!cfg_loaded) mqtt_prepare();
    int changed = 0;
    io_lock();
    if (argc > 2 && !strcmp(argv[1], "broker")) {
        scopy(broker, argv[2], sizeof(broker));
        if (argc > 3) { u64 p; if (parse_num(argv[3], &p)) port = (int)p; }
        changed = 1;
    } else if (argc > 1 && !strcmp(argv[1], "off")) { broker[0] = 0; changed = 1; }
    else if (argc > 2 && !strcmp(argv[1], "user")) { scopy(user, argv[2], sizeof(user)); scopy(pass, argc > 3 ? argv[3] : "", sizeof(pass)); changed = 1; }
    else if (argc > 2 && !strcmp(argv[1], "topic")) { scopy(base, argv[2], sizeof(base)); changed = 1; }
    else if (argc > 2 && !strcmp(argv[1], "interval")) { u64 v; if (parse_num(argv[2], &v)) interval = (int)v; changed = 1; }
    else if (argc > 2 && !strcmp(argv[1], "discovery")) { discovery = !strcmp(argv[2], "on"); changed = 1; }
    else if (argc > 3 && !strcmp(argv[1], "pub")) {
        io_unlock();
        int r = mqtt_publish(argv[2], argv[3], (int)strlen(argv[3]), 0);
        kprintf("%s\n", r ? "not connected" : "sent");
        return;
    } else if (argc > 1 && !strcmp(argv[1], "state")) { if (st == M_UP) publish_state(); }
    else if (argc > 1) {
        io_unlock();
        kprintf("usage: mqtt [broker <host> [port] | user <name> <password> | topic <base> | interval <s>\n"
                "             | discovery on|off | off | pub <topic> <text> | state]\n");
        return;
    }
    if (changed) {
        if (pcb || st == M_UP) { if (st == M_UP) { txn = 5; send_packet(0xE0); } drop("settings changed", 0); }
        st = broker[0] ? M_WAIT : M_OFF;
        retry_at = 0;
    }
    io_unlock();
    if (changed) save();
    kprintf("MQTT: %s", broker[0] ? mname[st] : "off ('mqtt broker <host>')");
    if (broker[0]) kprintf(" - %s:%d, topic %s/...", broker, port, base);
    kprintf("\n");
    if (broker[0]) kprintf("  every %d s to %s/state, Home Assistant discovery %s, %u sent, %u received%s%s\n",
                           interval, base, discovery ? "on" : "off", sent, received, why[0] ? ", last problem: " : "", why);
}
