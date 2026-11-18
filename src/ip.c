/*
 * TCP/IP for RoboOS: glue between lwIP (lwip/, BSD license, raw API without an
 * operating system) and the Ethernet driver (eth.c).
 *
 *  - frames that are not RoboOS messages (EtherType 0x88B5, net.c) go to lwIP
 *  - lwIP's timers (ARP, DHCP, TCP) run from the console idle loop, like the
 *    rest of the network: net_poll() -> ip_poll() every 5 ms
 *  - address: DHCP by default (the board's name is sent as the host name, so the
 *    router lists it), or a static address saved in /IP.TXT
 *
 * Commands: ip, ping, http (a small web page + the JSON API of webapi.c).
 * Sockets for programs (C API, BASIC): numbered TCP / UDP connections with a
 * receive buffer each; see the "sockets" section below.
 *
 * lwIP runs on the I/O core (core 1, smp.c) under the io lock. Everything here
 * that core 0 calls takes that lock, and lets go of it while it waits for an
 * answer, so the I/O core can receive it.
 */
#include "usb.h"
#include "sd.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/ip4.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "netif/ethernet.h"
#include "bearssl.h"
br_ssl_engine_context *tls_engine(struct tls_conn *c);   /* tls.c */

static struct netif nif;
static int lwip_ready, started, use_dhcp = 1, announce;
static ip4_addr_t shown_ip;

/* ---------------- lwIP port functions ---------------- */

u32_t sys_now(void) { return (u32_t)(timer_us() / 1000); }

unsigned int lwip_port_rand(void)
{
    static u64 x = 0x9E3779B97F4A7C15UL;
    x ^= timer_us();
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return (unsigned int)x;
}

void lwip_port_assert(const char *msg, int line, const char *file)
{
    con_color(COL_RED);
    kprintf("\n[lwIP] assertion failed: %s (%s:%d)\n", msg, file, line);
    con_color(COL_DEFAULT);
}

/* ---------------- network interface ---------------- */

/* frame counters for 'ip' (diagnostics on the screen) */
static u32 rx_all, rx_arp, rx_ip, rx_vlan, rx_other, tx_all, tx_err;

static err_t link_out(struct netif *n, struct pbuf *p)
{
    static u8 f[1536];
    (void)n;
    if (p->tot_len > 1514) return ERR_IF;
    pbuf_copy_partial(p, f, p->tot_len, 0);
    int len = p->tot_len;
    if (len < 60) { memset(f + len, 0, 60 - len); len = 60; }   /* minimum frame */
    tx_all++;
    if (eth_send(f, len) < 0) { tx_err++; return ERR_IF; }
    return ERR_OK;
}

static err_t nif_init(struct netif *n)
{
    n->name[0] = 'e'; n->name[1] = '0';
    n->output = etharp_output;
    n->linkoutput = link_out;
    n->mtu = 1500;
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, eth_mac(), 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif_set_hostname(n, net_name());
    return ERR_OK;
}

static void load_http(void);
static int  tls_want;
static int  https_start(void);
static int  http_want;                     /* /HTTP.TXT says "on": start with the network */
static int  http_start(void);

/* /IP.TXT: "dhcp", or "address [mask [gateway [dns]]]" */
static void load_config(ip4_addr_t *a, ip4_addr_t *m, ip4_addr_t *g, ip4_addr_t *d)
{
    struct fat_file f;
    char b[96], *w[4];
    int n = 0, k = 0;
    use_dhcp = 1;
    if (!fat_mounted() || fat_open("/IP.TXT", &f) || f.is_dir) return;
    n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    for (char *p = b; *p && k < 4; ) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') *p++ = 0;
        if (!*p) break;
        w[k++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    }
    if (!k || !ip4addr_aton(w[0], a)) return;
    use_dhcp = 0;
    if (k < 2 || !ip4addr_aton(w[1], m)) IP4_ADDR(m, 255, 255, 255, 0);
    if (k < 3 || !ip4addr_aton(w[2], g)) ip4_addr_set_zero(g);
    if (k < 4 || !ip4addr_aton(w[3], d)) ip4_addr_copy(*d, *g);
}

/* /IP.TXT is read on core 0 (the FAT code is not for two cores), before core 1 starts */
static ip4_addr_t cfg_a, cfg_m, cfg_g, cfg_d;
static int cfg_loaded;

void ip_prepare(void)
{
    if (cfg_loaded || !fat_mounted() || smp_core() != 0) return;
    ip4_addr_set_zero(&cfg_a); ip4_addr_set_zero(&cfg_m); ip4_addr_set_zero(&cfg_g); ip4_addr_set_zero(&cfg_d);
    load_config(&cfg_a, &cfg_m, &cfg_g, &cfg_d);
    load_http();
    cfg_loaded = 1;
}

static void start(void)
{
    ip4_addr_t a, m, g, d;
    if (!lwip_ready) { lwip_init(); lwip_ready = 1; }
    ip_prepare();
    a = cfg_a; m = cfg_m; g = cfg_g; d = cfg_d;
    netif_add(&nif, &a, &m, &g, 0, nif_init, ethernet_input);
    netif_set_default(&nif);
    netif_set_up(&nif);
    if (!use_dhcp && !ip4_addr_isany_val(d)) {
        ip_addr_t dd;
        ip_addr_copy_from_ip4(dd, d);
        dns_setserver(0, &dd);
    }
    if (use_dhcp) dhcp_start(&nif);
    if (http_want) http_start();                         /* /HTTP.TXT: "on" */
    if (http_want && tls_want) https_start();            /* ... "tls" */
    started = 1;
    ip4_addr_set_zero(&shown_ip);
}

int ip_started(void) { return started; }

/* a frame from the wire that is not a RoboOS message */
void ipstack_input(const u8 *f, int n)
{
    static u8 t[1536];
    if (!started || n < 14) return;
    rx_all++;
    if (n >= 18 && f[12] == 0x81 && f[13] == 0x00) {     /* 802.1Q tag (also VLAN 0 = priority only): */
        if (n - 4 > (int)sizeof(t)) return;              /* lwIP drops tagged frames, so step over it */
        memcpy(t, f, 12);
        memcpy(t + 12, f + 16, (size_t)(n - 16));
        f = t; n -= 4;
        rx_vlan++;
    }
    if (f[12] == 0x08 && f[13] == 0x06) rx_arp++;
    else if (f[12] == 0x08 && f[13] == 0x00) rx_ip++;
    else rx_other++;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
    if (!p) return;
    pbuf_take(p, f, (u16_t)n);
    if (nif.input(p, &nif) != ERR_OK) pbuf_free(p);
}

static void put_ip(const ip4_addr_t *a)
{
    char b[16];
    kprintf("%s", ip4addr_ntoa_r(a, b, sizeof(b)));
}

/* ---------------- NTP: the time from a server (SNTP, one UDP packet) ---------------- */

static struct udp_pcb *ntp_pcb;
static u64 ntp_next, ntp_asked;
static int ntp_first = 1;

static void ntp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    u8 b[48];
    (void)arg; (void)pcb; (void)addr; (void)port;
    if (p->tot_len >= 48 && pbuf_copy_partial(p, b, 48, 0) == 48) {
        int mode = b[0] & 7, stratum = b[1];
        u32 sec = (u32)b[40] << 24 | (u32)b[41] << 16 | (u32)b[42] << 8 | b[43];
        u32 frac = (u32)b[44] << 24;
        if ((mode == 4 || mode == 5) && stratum > 0 && stratum < 16 && sec > 2208988800u + 1700000000u) {
            u64 t = (u64)sec - 2208988800u + (frac >= 0x80000000u);
            t += (timer_us() - ntp_asked) / 2000000;     /* half the round trip */
            clock_set_time(t, 1);
            ntp_next = timer_us() + 3600ULL * 1000000;   /* again in an hour */
            if (ntp_first) {
                char s[32];
                clock_format(s, t, 0);
                con_color(COL_CYAN); kprintf("\n[NET] time %s (NTP %s)\n", s, clock_server()); con_color(COL_DEFAULT);
                ntp_first = 0;
            }
        }
    }
    pbuf_free(p);
}

static void ntp_send(const ip_addr_t *a)
{
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 48, PBUF_RAM);
    if (!p) return;
    memset(p->payload, 0, 48);
    ((u8 *)p->payload)[0] = 0x23;                        /* version 4, client */
    ntp_asked = timer_us();
    udp_sendto(ntp_pcb, p, a, 123);
    pbuf_free(p);
}

static void ntp_dns(const char *name, const ip_addr_t *a, void *arg)
{
    (void)name; (void)arg;
    if (a) ntp_send(a);
}

static void ntp_tick(void)
{
    if (ip4_addr_isany_val(*netif_ip4_addr(&nif)) || !netif_is_link_up(&nif)) return;
    u64 now = timer_us();
    int forced = clock_ntp_wanted();
    if (!forced && now < ntp_next) return;
    if (!clock_server()[0]) { ntp_next = now + 60ULL * 1000000; return; }
    ntp_next = now + 30ULL * 1000000;                    /* no answer: try again in 30 s */
    if (!ntp_pcb) {
        ntp_pcb = udp_new();
        if (!ntp_pcb) return;
        udp_bind(ntp_pcb, IP_ADDR_ANY, 0);
        udp_recv(ntp_pcb, ntp_recv, 0);
    }
    ip_addr_t a;
    if (dns_gethostbyname(clock_server(), &a, ntp_dns, 0) == ERR_OK) ntp_send(&a);
}

/* called every 5 ms from net_poll(); notify = may print (shell prompt) */
void ip_poll(int notify)
{
    if (!started) {
        if (!eth_present() || eth_error(0, 0)) return;
        start();
    }
    int up = eth_present() && !eth_error(0, 0) && eth_link_cached() > 0;
    if (up && !netif_is_link_up(&nif)) netif_set_link_up(&nif);
    if (!up && netif_is_link_up(&nif)) netif_set_link_down(&nif);
    sys_check_timeouts();
    api_poll();
    ntp_tick();

    if (!ip4_addr_cmp(netif_ip4_addr(&nif), &shown_ip)) {
        ip4_addr_copy(shown_ip, *netif_ip4_addr(&nif));
        announce = !ip4_addr_isany_val(shown_ip);
    }
    if (announce && notify) {
        announce = 0;
        con_color(COL_CYAN); kprintf("\n[NET] IP address "); put_ip(&shown_ip);
        kprintf(use_dhcp ? " (DHCP)\n" : " (static)\n"); con_color(COL_DEFAULT);
    }
}

/* run the network for a while (commands that wait for an answer); 1 = Ctrl+C */
static int run_ms(u32 ms)
{
    int d = io_release(), r = 0;                        /* the I/O core works meanwhile */
    u64 end = timer_us() + (u64)ms * 1000;
    while (timer_us() < end)
        if (con_trygetc() == 3) { r = 1; break; }      /* without an I/O core this polls the network */
    io_reacquire(d);
    return r;
}

/* ---------------- command: ip ---------------- */

static int need_net(void)
{
    if (started) return 1;
    if (!eth_present()) kprintf("no Ethernet chip found\n");
    else kprintf("network not started (Ethernet init failed - see 'net')\n");
    return 0;
}

static void save_config(const char *text)
{
    if (!fat_mounted() || fat_write_file("/IP.TXT", text, (u32)strlen(text)))
        kprintf("(not saved: no SD card or write error)\n");
}

static void ip_cmd_l(int argc, char **argv)
{
    if (!need_net()) return;
    if (argc > 1 && !strcmp(argv[1], "dhcp")) {
        use_dhcp = 1;
        save_config("dhcp\r\n");
        dhcp_start(&nif);
        kprintf("asking for an address (DHCP)...\n");
        for (int i = 0; i < 100 && !dhcp_supplied_address(&nif); i++)
            if (run_ms(100)) break;
    } else if (argc > 2 && !strcmp(argv[1], "set")) {
        ip4_addr_t a, m, g;
        char t[80];
        int n = 0;
        if (!ip4addr_aton(argv[2], &a)) { kprintf("bad address: %s\n", argv[2]); return; }
        if (argc < 4 || !ip4addr_aton(argv[3], &m)) IP4_ADDR(&m, 255, 255, 255, 0);
        if (argc < 5 || !ip4addr_aton(argv[4], &g)) ip4_addr_set_zero(&g);
        dhcp_release_and_stop(&nif);
        use_dhcp = 0;
        netif_set_addr(&nif, &a, &m, &g);
        if (!ip4_addr_isany_val(g)) { ip_addr_t d; ip_addr_copy_from_ip4(d, g); dns_setserver(0, &d); }
        for (int i = 2; i < argc && i < 5; i++) {
            for (const char *s = argv[i]; *s && n < 70; s++) t[n++] = *s;
            t[n++] = ' ';
        }
        t[n++] = '\r'; t[n++] = '\n'; t[n] = 0;
        save_config(t);
    } else if (argc > 1) {
        kprintf("usage: ip [dhcp | set <address> [mask] [gateway]]\n");
        return;
    }
    const ip_addr_t *dns = dns_getserver(0);
    kprintf("host:    %s\n", net_name());
    kprintf("link:    %s\n", netif_is_link_up(&nif) ? "up" : "down (cable?)");
    kprintf("address: ");
    if (ip4_addr_isany_val(*netif_ip4_addr(&nif))) kprintf(use_dhcp ? "none yet (waiting for DHCP)" : "none");
    else { put_ip(netif_ip4_addr(&nif)); kprintf(" / "); put_ip(netif_ip4_netmask(&nif)); }
    kprintf("  (%s)\n", use_dhcp ? "DHCP" : "static, /IP.TXT");
    kprintf("gateway: "); put_ip(netif_ip4_gw(&nif));
    kprintf("\ndns:     "); put_ip(ip_2_ip4(dns)); con_putc('\n');
    kprintf("frames:  in %u (ARP %u, IPv4 %u, other %u; VLAN-tagged %u), out %u (errors %u)\n",
            rx_all, rx_arp, rx_ip, rx_other, rx_vlan, tx_all, tx_err);
}

/* ---------------- command: ping ---------------- */

#define PING_ID 0x524F                                  /* "RO" */
static u16 ping_seq;
static volatile int ping_got, ping_ttl;       /* set on the I/O core */

static u8 ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    struct icmp_echo_hdr e;
    (void)arg; (void)pcb; (void)addr;
    if (p->tot_len < IP_HLEN) return 0;
    u8 vhl, ttl;
    pbuf_copy_partial(p, &vhl, 1, 0);
    pbuf_copy_partial(p, &ttl, 1, 8);
    u16 hlen = (u16)((vhl & 0x0F) * 4);
    if (p->tot_len < hlen + sizeof(e)) return 0;
    pbuf_copy_partial(p, &e, sizeof(e), hlen);
    if (e.type != ICMP_ER || e.id != lwip_htons(PING_ID) || e.seqno != lwip_htons(ping_seq)) return 0;
    ping_got = 1;
    ping_ttl = ttl;
    pbuf_free(p);
    return 1;                                            /* eaten */
}

static struct { volatile int state; ip_addr_t addr; } dns_res;  /* 0 waiting, 1 found, -1 failed */

static void dns_found(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)name; (void)arg;
    if (addr) { ip_addr_copy(dns_res.addr, *addr); dns_res.state = 1; }
    else dns_res.state = -1;
}

/* name or dotted address -> address; 0 = ok, -1 unknown, -2 aborted by idle() */
static int resolve(const char *name, ip_addr_t *out, int (*idle)(void))
{
    if (ipaddr_aton(name, out)) return 0;
    dns_res.state = 0;
    err_t e = dns_gethostbyname(name, out, dns_found, 0);
    if (e == ERR_OK) return 0;
    if (e != ERR_INPROGRESS) return -1;
    u64 end = timer_us() + 5000000;
    int d = io_release(), stop = 0;
    while (!dns_res.state && timer_us() < end)
        if (idle()) { stop = 1; break; }
    io_reacquire(d);
    if (stop) return -2;
    if (dns_res.state != 1) return -1;
    ip_addr_copy(*out, dns_res.addr);
    return 0;
}

static int idle_shell(void) { return con_trygetc() == 3; }   /* polls the network, Ctrl+C aborts */

static void ping_cmd_l(int argc, char **argv)
{
    static struct raw_pcb *pcb;
    ip_addr_t dst;
    u64 count = 4;
    if (argc < 2) { kprintf("usage: ping <address or name> [count]\n"); return; }
    if (!need_net()) return;
    if (argc > 2) parse_num(argv[2], &count);
    int r = resolve(argv[1], &dst, idle_shell);
    if (r == -2) { kprintf("^C\n"); return; }
    if (r) { kprintf("cannot resolve %s\n", argv[1]); return; }
    if (!pcb) {
        pcb = raw_new(IP_PROTO_ICMP);
        if (!pcb) { kprintf("out of memory\n"); return; }
        raw_recv(pcb, ping_recv, 0);
        raw_bind(pcb, IP_ADDR_ANY);
    }
    kprintf("PING %s (", argv[1]); put_ip(ip_2_ip4(&dst)); kprintf(")\n");
    u32 sent = 0, got = 0;
    for (u32 i = 0; i < count; i++) {
        struct pbuf *p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr) + 32, PBUF_RAM);
        if (!p) { kprintf("out of memory\n"); break; }
        struct icmp_echo_hdr *e = p->payload;
        u8 *data = (u8 *)p->payload + sizeof(*e);
        ping_seq++;
        e->type = ICMP_ECHO; e->code = 0; e->chksum = 0;
        e->id = lwip_htons(PING_ID); e->seqno = lwip_htons(ping_seq);
        for (int k = 0; k < 32; k++) data[k] = (u8)('a' + k % 26);
        e->chksum = inet_chksum(e, p->len);
        ping_got = 0;
        u64 t0 = timer_us();
        raw_sendto(pcb, p, &dst);
        pbuf_free(p);
        sent++;
        int stop = 0, d = io_release();
        while (!ping_got && timer_us() - t0 < 1000000)
            if (con_trygetc() == 3) { stop = 1; break; }
        io_reacquire(d);
        if (stop) { kprintf("^C\n"); break; }
        if (ping_got) {
            u32 us = (u32)(timer_us() - t0);
            got++;
            kprintf("reply from "); put_ip(ip_2_ip4(&dst));
            kprintf(": seq=%u ttl=%d time=%u.%u ms\n", ping_seq, ping_ttl, us / 1000, us / 100 % 10);
            if (i + 1 < count && run_ms(1000 - us / 1000 > 1000 ? 0 : 1000 - us / 1000)) { kprintf("^C\n"); break; }
        } else {
            kprintf("no reply (seq=%u)\n", ping_seq);
        }
    }
    kprintf("%u sent, %u received\n", sent, got);
}

/* ---------------- command: http (a small page + the JSON API in webapi.c) ---------------- */

static struct tcp_pcb *http_pcb, *https_pcb;
static u32 http_hits;
static char page[4096];
static int plen;

static void add(const char *s) { while (*s && plen < (int)sizeof(page) - 1) page[plen++] = *s++; page[plen] = 0; }
static void add_u(u64 v)
{
    char t[24]; int k = 0;
    do { t[k++] = '0' + v % 10; v /= 10; } while (v);
    while (k && plen < (int)sizeof(page) - 1) page[plen++] = t[--k];
    page[plen] = 0;
}

static char addr_s[16];
const char *ip_address_str(void) { return ip4addr_ntoa_r(netif_ip4_addr(&nif), addr_s, sizeof(addr_s)); }

static void build_page(void)
{
    u32 t[2] = { 0, 0 };
    u64 s = timer_us() / 1000000;
    int tc = mbox_prop(0x00030006, t, 2) ? (int)t[1] : -1;
    plen = 0;
    add("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>RoboOS</title>"
        "<meta name=\"viewport\" content=\"width=device-width\">"
        "<style>body{font-family:monospace;background:#101418;color:#c0c0c0;margin:2em}"
        "h1{color:#55ffff}td{padding:2px 12px 2px 0}</style></head><body><h1>RoboOS</h1><table>");
    add("<tr><td>name</td><td>"); add(net_name()); add("</td></tr>");
    add("<tr><td>board</td><td>"); add(board_name()); add("</td></tr>");
    add("<tr><td>version</td><td>"); add(os_version()); add("</td></tr>");
    add("<tr><td>address</td><td>"); add(ip_address_str()); add("</td></tr>");
    add("<tr><td>uptime</td><td>"); add_u(s / 3600); add(" h "); add_u(s / 60 % 60); add(" min "); add_u(s % 60); add(" s</td></tr>");
    if (tc >= 0) { add("<tr><td>CPU</td><td>"); add_u((u64)tc / 1000); add("."); add_u((u64)tc % 1000 / 100); add(" C</td></tr>"); }
    add("<tr><td>visits</td><td>"); add_u(http_hits); add("</td></tr>");
    add("<tr><td>JSON API</td><td>"); add(api_token()[0] ? "on (/api/..., Bearer token)" : "off"); add("</td></tr>");
    add("<tr><td>HTTPS</td><td>"); add(https_pcb ? "on" : "off"); add("</td></tr>");
    {
        char tb[32];
        u64 now = clock_now();
        if (now) { clock_format(tb, now, 0); add("<tr><td>time</td><td>"); add(tb); add("</td></tr>"); }
    }
    add("</table></body></html>\n");
}

/* one connection: the request is collected until the headers and Content-Length bytes are in.
 * With TLS (port 443) the bytes go through BearSSL first (tls.c), the answer back the same way. */
#define HTTP_CONNS 8
#define HTTP_REQ   2048
struct http_conn {
    int used, len, done, closing;
    u8 idle;
    u64 since;
    struct tcp_pcb *pcb;
    struct tls_conn *tls;          /* 0 = plain HTTP */
    struct pbuf *pin;              /* TLS: received bytes not yet given to BearSSL */
    u16 pin_off;
    char req[HTTP_REQ + 1];
};
static struct http_conn hconn[HTTP_CONNS];
static int tls_want;               /* /HTTP.TXT says "tls" */
static u32 https_hits;

static void conn_free(struct http_conn *c)
{
    if (!c) return;
    if (c->tls) { tls_close(c->tls); c->tls = 0; }
    if (c->pin) { pbuf_free(c->pin); c->pin = 0; }
    c->used = 0;
}

static void http_err(void *arg, err_t err) { (void)err; conn_free(arg); }   /* the pcb is gone already */

/* ERR_ABRT if the pcb had to be aborted (a callback must then return ERR_ABRT) */
static err_t http_finish(struct tcp_pcb *pcb, struct http_conn *c)
{
    tcp_arg(pcb, 0); tcp_recv(pcb, 0); tcp_err(pcb, 0); tcp_poll(pcb, 0, 0); tcp_sent(pcb, 0);
    conn_free(c);
    if (tcp_close(pcb) != ERR_OK) { tcp_abort(pcb); return ERR_ABRT; }   /* FIN after the queued data */
    return ERR_OK;
}

static void send_text(struct tcp_pcb *pcb, const char *t, int n)
{
    if (n > (int)tcp_sndbuf(pcb)) n = (int)tcp_sndbuf(pcb);
    tcp_write(pcb, t, (u16_t)n, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
}

/* the value of a header (case-insensitive name), cut at the end of its line; 0 = none */
static char *header(char *h, const char *name)
{
    int nl = (int)strlen(name);
    for (char *p = strstr(h, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        char *l = p + 2;
        if (l[0] == '\r') break;                         /* end of the headers */
        int k = 0;
        while (k < nl && l[k] && ((l[k] | 32) == (name[k] | 32))) k++;
        if (k == nl && l[k] == ':') { l += k + 1; while (*l == ' ') l++; return l; }
    }
    return 0;
}

/* 1 = the answer is in page[0..plen) */
static int http_try(struct tcp_pcb *pcb, struct http_conn *c)
{
    char *end = strstr(c->req, "\r\n\r\n");
    if (!end) {
        if (c->len >= HTTP_REQ) { plen = 0; add("HTTP/1.0 431 Too Large\r\nConnection: close\r\n\r\n"); return 1; }
        return 0;
    }
    char *body = end + 4;
    int have = c->len - (int)(body - c->req), need = 0;
    char *cl = header(c->req, "Content-Length");
    if (cl) while (*cl >= '0' && *cl <= '9') need = need * 10 + (*cl++ - '0');
    if (need > HTTP_REQ - (int)(body - c->req)) { plen = 0; add("HTTP/1.0 413 Too Large\r\nConnection: close\r\n\r\n"); return 1; }
    if (have < need) return 0;

    /* "METHOD /path HTTP/1.x" */
    char *m = c->req, *path = strchr(m, ' ');
    if (!path) { plen = 0; return 1; }
    *path++ = 0;
    char *sp = strchr(path, ' ');
    if (sp) *sp = 0;
    char *q = strchr(path, '?');
    if (q) *q = 0;
    char from[16];
    ip4addr_ntoa_r(ip_2_ip4(&pcb->remote_ip), from, sizeof(from));

    if (c->tls) https_hits++; else http_hits++;
    if (!strncmp(path, "/api", 4) && (path[4] == 0 || path[4] == '/')) {
        if (!c->tls && https_pcb) {                      /* with HTTPS on, the token must not go in clear text */
            plen = 0;
            add("HTTP/1.0 403 Forbidden\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                "{\"ok\":false,\"error\":\"HTTPS is on: use https://");
            add(ip_address_str());
            add("/api/...\"}\n");
        } else if (!strcmp(m, "OPTIONS")) {              /* a browser asks before a POST from a web page */
            plen = 0;
            add("HTTP/1.0 204 No Content\r\nAccess-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Authorization, Content-Type\r\nConnection: close\r\n\r\n");
        } else {
            char *auth = header(c->req + (sp ? (sp - c->req) + 1 : 0), "Authorization");
            if (auth) { char *e = strstr(auth, "\r\n"); if (e) *e = 0; }
            body[need] = 0;
            plen = api_handle(m, path, auth, body, need, from, page, sizeof(page));
        }
    } else {
        build_page();
    }
    return 1;
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct http_conn *c = arg;
    (void)err;
    if (!p) return http_finish(pcb, c);
    tcp_recved(pcb, p->tot_len);
    if (c && !c->done) {
        int n = p->tot_len;
        if (n > HTTP_REQ - c->len) n = HTTP_REQ - c->len;
        pbuf_copy_partial(p, c->req + c->len, (u16_t)n, 0);
        c->len += n;
        c->req[c->len] = 0;
        c->idle = 0;
        if (http_try(pcb, c)) {
            c->done = 1;
            send_text(pcb, page, plen);
            pbuf_free(p);
            return http_finish(pcb, c);
        }
    }
    pbuf_free(p);
    return ERR_OK;
}

/* ---- TLS: move bytes between TCP, BearSSL and the request / answer ---- */

static err_t tls_pump(struct tcp_pcb *pcb, struct http_conn *c)
{
    br_ssl_engine_context *e = tls_engine(c->tls);
    for (int guard = 0; guard < 64; guard++) {
        unsigned st = br_ssl_engine_current_state(e);
        int progress = 0;
        if (st & BR_SSL_CLOSED) return http_finish(pcb, c);
        if (st & BR_SSL_SENDREC) {                       /* records to send */
            size_t len;
            u8 *b = br_ssl_engine_sendrec_buf(e, &len);
            u32 room = tcp_sndbuf(pcb);
            if (len > room) len = room;
            if (len > 0xFFFF) len = 0xFFFF;
            if (len && tcp_write(pcb, b, (u16_t)len, TCP_WRITE_FLAG_COPY) == ERR_OK) { br_ssl_engine_sendrec_ack(e, len); progress = 1; }
        } else if (c->closing) {                         /* close_notify is out */
            tcp_output(pcb);
            return http_finish(pcb, c);
        }
        if (st & BR_SSL_RECVAPP) {                       /* decrypted request bytes */
            size_t len;
            u8 *b = br_ssl_engine_recvapp_buf(e, &len);
            int n = (int)len;
            if (n > HTTP_REQ - c->len) n = HTTP_REQ - c->len;
            memcpy(c->req + c->len, b, (size_t)n);
            c->len += n;
            c->req[c->len] = 0;
            br_ssl_engine_recvapp_ack(e, len);
            progress = 1;
            if (!c->done && http_try(pcb, c)) c->done = 1, c->closing = 2;   /* 2 = answer waiting */
        }
        if ((st & BR_SSL_SENDAPP) && c->closing == 2) {  /* the answer, then close_notify */
            size_t len;
            u8 *b = br_ssl_engine_sendapp_buf(e, &len);
            if ((int)len >= plen) {
                memcpy(b, page, (size_t)plen);
                br_ssl_engine_sendapp_ack(e, (size_t)plen);
                br_ssl_engine_flush(e, 0);
                br_ssl_engine_close(e);
                c->closing = 1;
                progress = 1;
            }
        }
        if ((st & BR_SSL_RECVREC) && c->pin) {           /* feed received records */
            size_t len;
            u8 *b = br_ssl_engine_recvrec_buf(e, &len);
            int left = c->pin->tot_len - c->pin_off;
            int n = left < (int)len ? left : (int)len;
            pbuf_copy_partial(c->pin, b, (u16_t)n, c->pin_off);
            br_ssl_engine_recvrec_ack(e, (size_t)n);
            tcp_recved(pcb, (u16_t)n);
            c->pin_off += (u16)n;
            if (c->pin_off >= c->pin->tot_len) { pbuf_free(c->pin); c->pin = 0; c->pin_off = 0; }
            progress = 1;
        }
        if (!progress) break;
    }
    tcp_output(pcb);
    return ERR_OK;
}

static err_t https_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct http_conn *c = arg;
    (void)err;
    if (!p) return http_finish(pcb, c);
    if (!c || !c->tls) { tcp_recved(pcb, p->tot_len); pbuf_free(p); return ERR_OK; }
    if (c->pin) return ERR_MEM;                          /* still busy with the last one: lwIP keeps it */
    c->pin = p; c->pin_off = 0; c->idle = 0;
    return tls_pump(pcb, c);
}

static err_t https_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    struct http_conn *c = arg;
    (void)len;
    if (c && c->tls) return tls_pump(pcb, c);
    return ERR_OK;
}

static err_t http_poll(void *arg, struct tcp_pcb *pcb)
{
    struct http_conn *c = arg;
    if (c && ++c->idle >= 10) return http_finish(pcb, c);   /* 5 s quiet */
    if (c && c->tls) return tls_pump(pcb, c);
    return ERR_OK;
}

static struct http_conn *conn_new(struct tcp_pcb *pcb)
{
    struct http_conn *c = 0;
    for (int i = 0; i < HTTP_CONNS; i++) if (!hconn[i].used) { c = &hconn[i]; break; }
    if (!c) {                                            /* all taken: drop the oldest silent one */
        struct http_conn *old = 0;                       /* (browsers open spare connections and wait) */
        for (int i = 0; i < HTTP_CONNS; i++)
            if (hconn[i].len == 0 && !hconn[i].tls && (!old || hconn[i].since < old->since)) old = &hconn[i];
        if (!old) return 0;                              /* all busy with real requests */
        struct tcp_pcb *op = old->pcb;
        tcp_arg(op, 0); tcp_recv(op, 0); tcp_err(op, 0); tcp_poll(op, 0, 0); tcp_sent(op, 0);
        tcp_abort(op);
        conn_free(old);
        c = old;
    }
    c->used = 1; c->len = 0; c->done = 0; c->closing = 0; c->idle = 0; c->req[0] = 0;
    c->since = timer_us(); c->pcb = pcb; c->tls = 0; c->pin = 0; c->pin_off = 0;
    return c;
}

static err_t http_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    struct http_conn *c = conn_new(pcb);
    if (!c) { tcp_abort(pcb); return ERR_ABRT; }
    tcp_arg(pcb, c);
    tcp_recv(pcb, http_recv);
    tcp_err(pcb, http_err);
    tcp_poll(pcb, http_poll, 1);                         /* every 0.5 s */
    return ERR_OK;
}

static err_t https_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    struct http_conn *c = conn_new(pcb);
    struct tls_conn *t = c ? tls_open() : 0;
    if (!t) { if (c) conn_free(c); tcp_abort(pcb); return ERR_ABRT; }   /* 4 TLS connections at most */
    c->tls = t;
    tcp_arg(pcb, c);
    tcp_recv(pcb, https_recv);
    tcp_sent(pcb, https_sent);
    tcp_err(pcb, http_err);
    tcp_poll(pcb, http_poll, 1);
    return ERR_OK;
}

static int http_start(void)
{
    if (http_pcb) return 1;
    struct tcp_pcb *p = tcp_new();
    if (!p || tcp_bind(p, IP_ADDR_ANY, 80) != ERR_OK) { if (p) tcp_close(p); return 0; }
    http_pcb = tcp_listen(p);
    tcp_accept(http_pcb, http_accept);
    return 1;
}

static int https_start(void)
{
    if (https_pcb) return 1;
    if (!tls_ready()) return 0;
    struct tcp_pcb *p = tcp_new();
    if (!p || tcp_bind(p, IP_ADDR_ANY, 443) != ERR_OK) { if (p) tcp_close(p); return 0; }
    https_pcb = tcp_listen(p);
    tcp_accept(https_pcb, https_accept);
    return 1;
}

static void https_stop(void)
{
    if (https_pcb) { tcp_close(https_pcb); https_pcb = 0; }
    for (int i = 0; i < HTTP_CONNS; i++)                 /* open TLS connections end */
        if (hconn[i].used && hconn[i].tls) {
            struct tcp_pcb *op = hconn[i].pcb;
            tcp_arg(op, 0); tcp_recv(op, 0); tcp_err(op, 0); tcp_poll(op, 0, 0); tcp_sent(op, 0);
            tcp_abort(op);
            conn_free(&hconn[i]);
        }
}

/* /HTTP.TXT: "on|off [token] [tls]" - read on core 0 in ip_prepare() */
static void load_http(void)
{
    struct fat_file f;
    char b[120];
    int n;
    if (!fat_mounted() || fat_open("/HTTP.TXT", &f) || f.is_dir) return;
    if ((n = fat_read(&f, b, sizeof(b) - 1)) <= 0) return;
    b[n] = 0;
    char *p = b, *w[4];
    int k = 0;
    while (*p && k < 4) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') *p++ = 0;
        if (!*p) break;
        w[k++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    }
    if (k) http_want = !strcmp(w[0], "on");
    for (int i = 1; i < k; i++) {
        if (!strcmp(w[i], "tls")) tls_want = 1;
        else api_set_token(w[i]);                        /* tokens have 8+ characters */
    }
    if (tls_want && !tls_prepare(0, "")) kprintf("HTTPS: %s\n", tls_error());
}

static void save_http(void)
{
    char b[110];
    int n = 0;
    const char *on = http_pcb ? "on" : "off", *t = api_token();
    while (*on) b[n++] = *on++;
    if (*t) { b[n++] = ' '; while (*t && n < 92) b[n++] = *t++; }
    if (tls_want) { b[n++] = ' '; b[n++] = 't'; b[n++] = 'l'; b[n++] = 's'; }
    b[n++] = '\r'; b[n++] = '\n'; b[n] = 0;
    if (!fat_mounted() || fat_write_file("/HTTP.TXT", b, (u32)n))
        kprintf("(not saved: no SD card or write error)\n");
}

static void http_cmd_l(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "token")) {
        if (argc > 2 && !strcmp(argv[2], "off")) api_set_token("");
        else if (argc > 2) {
            int l = (int)strlen(argv[2]), ok = l >= 8 && l <= 63;
            for (int i = 0; i < l; i++) if ((u8)argv[2][i] <= ' ' || (u8)argv[2][i] > '~' || argv[2][i] == '"') ok = 0;
            if (!ok) { kprintf("token: 8-63 characters, no spaces or quotes\n"); return; }
            api_set_token(argv[2]);
        }
        if (argc > 2) save_http();
        if (api_token()[0]) kprintf("API token: %s\n", api_token());
        else                kprintf("API off (no token; 'http token <secret>' turns it on)\n");
        return;
    }
    if (!need_net()) return;
    if (argc > 2 && !strcmp(argv[1], "tls")) {
        if (!strcmp(argv[2], "on") || !strcmp(argv[2], "new")) {
            int renew = !strcmp(argv[2], "new");
            if (renew) https_stop();
            if (!tls_prepare(renew, ip_address_str())) { kprintf("HTTPS: %s\n", tls_error()); return; }
            if (!http_pcb) http_start();
            if (!https_start()) { kprintf("cannot open port 443\n"); return; }
            tls_want = 1;
        } else if (!strcmp(argv[2], "off")) {
            https_stop();
            tls_want = 0;
        } else { kprintf("usage: http tls on|off|new\n"); return; }
        save_http();
    } else if (argc > 1 && !strcmp(argv[1], "on")) {
        if (!http_start()) { kprintf("cannot open port 80\n"); return; }
        if (tls_want) https_start();
        save_http();
    } else if (argc > 1 && !strcmp(argv[1], "off")) {
        if (http_pcb) { tcp_close(http_pcb); http_pcb = 0; }
        https_stop();
        save_http();
    } else if (argc > 1) {
        kprintf("usage: http [on|off]   http token [<secret>|off]   http tls on|off|new\n");
        return;
    }
    const char *a = ip_address_str();
    if (http_pcb) kprintf("web page on: http://%s/  (%u requests)\n", a, http_hits);
    else          kprintf("web page off ('http on' starts it, also after reboot)\n");
    if (https_pcb) {
        kprintf("HTTPS on:    https://%s/  (%u requests; the API only here)\n", a, https_hits);
        kprintf("certificate SHA-256 %s\n", tls_fingerprint());
    } else kprintf("HTTPS off ('http tls on')\n");
    if (api_token()[0]) kprintf("JSON API on: %s://%s/api/status  (Authorization: Bearer <token>, 'http token' shows it)\n",
                                https_pcb ? "https" : "http", a);
    else                kprintf("JSON API off ('http token <secret>' turns it on)\n");
}

/* ======================= sockets for programs ======================= */
/*
 * Handles 1..NSOCK. Every socket has a receive ring buffer; TCP data that does
 * not fit is refused (lwIP keeps it and the TCP window stays closed until the
 * program reads), UDP datagrams that do not fit are dropped. Blocking calls
 * (connect, DNS, send when the buffer is full) wait by calling idle(), which
 * must poll the network: con_trygetc() via the program's Ctrl+C check.
 */
#define NSOCK   8
#define SOCK_RX 4096
enum { S_FREE, S_UDP, S_TCP, S_LISTEN };
enum { T_CONNECTING, T_OPEN, T_PEER_CLOSED, T_ERROR };

struct sock {
    u8  type, state;
    struct tcp_pcb *t;
    struct udp_pcb *u;
    ip_addr_t peer;                     /* UDP: where sockWrite sends (fixed, or the last sender) */
    u16 peer_port;
    u8  fixed_peer;
    u16 head, count;                    /* ring buffer */
    u16 udp_left;                       /* bytes left of the datagram being read byte by byte */
    ip_addr_t from;                     /* UDP: sender of the datagram being read */
    u16 from_port;
    u8  queue[NSOCK], nq;               /* LISTEN: accepted connections not yet taken */
    u8  rx[SOCK_RX];
};
static struct sock socks[NSOCK + 1];    /* [0] unused */

const char *sock_error(int e)
{
    switch (e) {
    case -1: return "no network";
    case -2: return "unknown host name";
    case -3: return "no answer / connection refused";
    case -4: return "too many open connections";
    case -5: return "bad handle";
    case -6: return "connection closed";
    case -7: return "out of memory";
    case -8: return "port in use";
    default: return e >= 0 ? "ok" : "error";
    }
}

static struct sock *get(int h, int type)
{
    if (h < 1 || h > NSOCK || socks[h].type == S_FREE) return 0;
    if (type && socks[h].type != type) return 0;
    return &socks[h];
}

static int alloc_sock(int type)
{
    for (int h = 1; h <= NSOCK; h++)
        if (socks[h].type == S_FREE) {
            memset(&socks[h], 0, sizeof(socks[h]) - SOCK_RX);
            socks[h].type = (u8)type;
            return h;
        }
    return -4;
}

static u16 rb_free(struct sock *s) { return (u16)(SOCK_RX - s->count); }

static void rb_put(struct sock *s, const void *d, u16 n)
{
    const u8 *p = d;
    for (u16 i = 0; i < n; i++) s->rx[(s->head + s->count + i) % SOCK_RX] = p[i];
    s->count += n;
}

static u16 rb_get(struct sock *s, void *d, u16 n)
{
    u8 *p = d;
    if (n > s->count) n = s->count;
    for (u16 i = 0; i < n; i++) { if (p) p[i] = s->rx[s->head]; s->head = (s->head + 1) % SOCK_RX; }
    s->count -= n;
    return n;
}

/* ---- TCP callbacks (arg = handle) ---- */

static err_t t_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct sock *s = &socks[(uintptr_t)arg];
    (void)err;
    if (!p) { s->state = T_PEER_CLOSED; return ERR_OK; }
    if (p->tot_len > rb_free(s)) return ERR_MEM;          /* lwIP offers it again later */
    for (struct pbuf *q = p; q; q = q->next) rb_put(s, q->payload, q->len);
    (void)pcb;
    pbuf_free(p);                                        /* tcp_recved() when the program reads */
    return ERR_OK;
}

static void t_err(void *arg, err_t err)
{
    struct sock *s = &socks[(uintptr_t)arg];
    (void)err;
    s->t = 0;                                            /* lwIP has freed the pcb */
    s->state = T_ERROR;
}

static err_t t_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)pcb;
    socks[(uintptr_t)arg].state = err == ERR_OK ? T_OPEN : T_ERROR;
    return ERR_OK;
}

static void t_setup(int h, struct tcp_pcb *pcb)
{
    socks[h].t = pcb;
    tcp_arg(pcb, (void *)(uintptr_t)h);
    tcp_recv(pcb, t_recv);
    tcp_err(pcb, t_err);
}

static err_t t_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    struct sock *l = &socks[(uintptr_t)arg];
    if (err != ERR_OK || !pcb) return ERR_VAL;
    int h = alloc_sock(S_TCP);
    if (h < 0 || l->nq >= NSOCK) {
        if (h > 0) socks[h].type = S_FREE;
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    socks[h].state = T_OPEN;
    t_setup(h, pcb);
    l->queue[l->nq++] = (u8)h;
    return ERR_OK;
}

/* ---- UDP callback: record = len(2) addr(4) port(2) data ---- */

static void u_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    struct sock *s = &socks[(uintptr_t)arg];
    (void)pcb;
    if (p->tot_len + 8 <= rb_free(s)) {
        u8 h[8];
        u32 a = ip4_addr_get_u32(ip_2_ip4(addr));
        h[0] = (u8)p->tot_len; h[1] = (u8)(p->tot_len >> 8);
        memcpy(h + 2, &a, 4);
        h[6] = (u8)port; h[7] = (u8)(port >> 8);
        rb_put(s, h, 8);
        for (struct pbuf *q = p; q; q = q->next) rb_put(s, q->payload, q->len);
    }
    if (!s->fixed_peer) { ip_addr_copy(s->peer, *addr); s->peer_port = port; }   /* reply goes back */
    pbuf_free(p);
}

/* start reading the next datagram: fills from / from_port, returns its length or -1 */
static int u_next(struct sock *s)
{
    u8 h[8];
    u32 a;
    if (s->count < 8) return -1;
    rb_get(s, h, 8);
    memcpy(&a, h + 2, 4);
    ip_addr_set_ip4_u32(&s->from, a);
    s->from_port = (u16)(h[6] | (h[7] << 8));
    s->udp_left = (u16)(h[0] | (h[1] << 8));
    return s->udp_left;
}

/* ---- API ---- */

static int sock_tcp_connect_l(const char *host, int port, int (*idle)(void))
{
    ip_addr_t a;
    if (!started) return -1;
    int r = resolve(host, &a, idle);
    if (r) return -2;
    int h = alloc_sock(S_TCP);
    if (h < 0) return h;
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { socks[h].type = S_FREE; return -7; }
    socks[h].state = T_CONNECTING;
    t_setup(h, pcb);
    if (tcp_connect(pcb, &a, (u16_t)port, t_connected) != ERR_OK) { sock_close(h); return -3; }
    u64 end = timer_us() + 10000000;
    int d = io_release(), stop = 0;
    while (*(volatile u8 *)&socks[h].state == T_CONNECTING && timer_us() < end)
        if (idle()) { stop = 1; break; }
    io_reacquire(d);
    if (stop) { sock_close(h); return -3; }
    if (socks[h].state != T_OPEN) { sock_close(h); return -3; }
    return h;
}

static int sock_tcp_listen_l(int port)
{
    if (!started) return -1;
    int h = alloc_sock(S_LISTEN);
    if (h < 0) return h;
    struct tcp_pcb *p = tcp_new();
    if (!p) { socks[h].type = S_FREE; return -7; }
    if (tcp_bind(p, IP_ADDR_ANY, (u16_t)port) != ERR_OK) { tcp_close(p); socks[h].type = S_FREE; return -8; }
    struct tcp_pcb *l = tcp_listen(p);
    if (!l) { tcp_close(p); socks[h].type = S_FREE; return -7; }
    socks[h].t = l;
    tcp_arg(l, (void *)(uintptr_t)h);
    tcp_accept(l, t_accept);
    return h;
}

static int sock_tcp_accept_l(int h)
{
    struct sock *l = get(h, S_LISTEN);
    if (!l) return -5;
    if (!l->nq) return 0;
    int c = l->queue[0];
    memmove(l->queue, l->queue + 1, --l->nq);
    return c;
}

static int sock_udp_open_l(int lport, const char *host, int rport, int (*idle)(void))
{
    ip_addr_t a;
    if (!started) return -1;
    if (host && host[0] && resolve(host, &a, idle)) return -2;
    int h = alloc_sock(S_UDP);
    if (h < 0) return h;
    struct udp_pcb *u = udp_new();
    if (!u) { socks[h].type = S_FREE; return -7; }
    if (udp_bind(u, IP_ADDR_ANY, (u16_t)lport) != ERR_OK) { udp_remove(u); socks[h].type = S_FREE; return -8; }
    ip_set_option(u, SOF_BROADCAST);
    udp_recv(u, u_recv, (void *)(uintptr_t)h);
    socks[h].u = u;
    if (host && host[0]) { ip_addr_copy(socks[h].peer, a); socks[h].peer_port = (u16)rport; socks[h].fixed_peer = 1; }
    return h;
}

static int udp_out(struct sock *s, const ip_addr_t *to, u16 port, const void *d, int len)
{
    if (len > 1472) len = 1472;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p) return -7;
    memcpy(p->payload, d, len);
    err_t e = udp_sendto(s->u, p, to, port);
    pbuf_free(p);
    return e == ERR_OK ? len : -3;
}

static int sock_write_l(int h, const void *d, int len, int (*idle)(void))
{
    struct sock *s = get(h, 0);
    if (!s || s->type == S_LISTEN) return -5;
    if (s->type == S_UDP) {
        if (!s->peer_port) return -3;                    /* nowhere to send yet */
        return udp_out(s, &s->peer, s->peer_port, d, len);
    }
    const u8 *p = d;
    int done = 0;
    u64 end = timer_us() + 10000000;
    while (done < len) {
        if (!s->t || s->state == T_ERROR) return done ? done : -6;
        int n = tcp_sndbuf(s->t);
        if (n > len - done) n = len - done;
        if (n > 0 && tcp_write(s->t, p + done, (u16_t)n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            done += n;
            continue;
        }
        tcp_output(s->t);
        if (timer_us() > end) break;
        int dd = io_release(), stop = idle();
        io_reacquire(dd);
        if (stop) break;
        s = get(h, 0);
        if (!s) return -5;
    }
    if (s->t) tcp_output(s->t);
    return done;
}

static int sock_sendto_l(int h, const char *host, int port, const void *d, int len, int (*idle)(void))
{
    ip_addr_t a;
    struct sock *s = get(h, S_UDP);
    if (!s) return -5;
    if (resolve(host, &a, idle)) return -2;
    return udp_out(s, &a, (u16)port, d, len);
}

/* bytes waiting; -1 when a TCP connection is closed and nothing is left */
static int sock_avail_l(int h)
{
    struct sock *s = get(h, 0);
    if (!s) return -5;
    if (s->type == S_LISTEN) return s->nq;
    if (s->type == S_UDP) return s->udp_left ? s->udp_left : s->count >= 8 ? (s->rx[s->head] | (s->rx[(s->head + 1) % SOCK_RX] << 8)) : 0;
    if (!s->count && s->state >= T_PEER_CLOSED) return -1;
    return s->count;
}

static void tcp_consumed(struct sock *s, u16 n)
{
    if (s->t && n) tcp_recved(s->t, n);
}

/* TCP: bytes (0 = nothing yet, -1 closed). UDP: one datagram (-1 = none) */
static int sock_read_l(int h, void *buf, int max)
{
    struct sock *s = get(h, 0);
    if (!s || s->type == S_LISTEN) return -5;
    if (s->type == S_UDP) {
        if (!s->udp_left && u_next(s) < 0) return -1;
        u16 n = s->udp_left < max ? s->udp_left : (u16)max;
        rb_get(s, buf, n);
        rb_get(s, 0, (u16)(s->udp_left - n));           /* the rest of a too long datagram */
        s->udp_left = 0;
        return n;
    }
    if (!s->count) return s->state >= T_PEER_CLOSED ? -1 : 0;
    u16 n = rb_get(s, buf, (u16)(max > 0xFFFF ? 0xFFFF : max));
    tcp_consumed(s, n);
    return n;
}

static int sock_read_from_l(int h, void *buf, int max, char *ip, int *port)
{
    struct sock *s = get(h, S_UDP);
    if (!s) return -5;
    if (!s->udp_left && u_next(s) < 0) return -1;
    if (ip) ip4addr_ntoa_r(ip_2_ip4(&s->from), ip, 16);
    if (port) *port = s->from_port;
    return sock_read(h, buf, max);
}

/* one byte: >= 0, -1 nothing yet, -2 closed */
static int sock_getc_l(int h)
{
    struct sock *s = get(h, 0);
    u8 c;
    if (!s || s->type == S_LISTEN) return -2;
    if (s->type == S_UDP) {
        if (!s->udp_left && u_next(s) < 0) return -1;
        if (!s->udp_left) return -1;                     /* empty datagram */
        rb_get(s, &c, 1);
        s->udp_left--;
        return c;
    }
    if (!s->count) return s->state >= T_PEER_CLOSED ? -2 : -1;
    rb_get(s, &c, 1);
    tcp_consumed(s, 1);
    return c;
}

static void sock_close_l(int h)
{
    struct sock *s = get(h, 0);
    if (!s) return;
    if (s->type == S_UDP && s->u) udp_remove(s->u);
    if (s->type == S_LISTEN) {
        for (int i = 0; i < s->nq; i++) sock_close(s->queue[i]);
        if (s->t) tcp_close(s->t);
    }
    if (s->type == S_TCP && s->t) {
        tcp_arg(s->t, 0); tcp_recv(s->t, 0); tcp_err(s->t, 0);
        if (tcp_close(s->t) != ERR_OK) tcp_abort(s->t);
    }
    s->type = S_FREE;
    s->t = 0; s->u = 0;
}

static void sock_close_all_l(void)
{
    for (int h = 1; h <= NSOCK; h++) sock_close(h);
}

static const char *ip_address_l(void)
{
    static char b[16];
    if (!started) return "0.0.0.0";
    return ip4addr_ntoa_r(netif_ip4_addr(&nif), b, sizeof(b));
}

/* ---- the entry points for core 0: under the io lock ---- */
int sock_tcp_connect(const char *host, int port, int (*idle)(void)) { io_lock(); int r = sock_tcp_connect_l(host, port, idle); io_unlock(); return r; }
int sock_tcp_listen(int port) { io_lock(); int r = sock_tcp_listen_l(port); io_unlock(); return r; }
int sock_tcp_accept(int h) { io_lock(); int r = sock_tcp_accept_l(h); io_unlock(); return r; }
int sock_udp_open(int lport, const char *host, int rport, int (*idle)(void)) { io_lock(); int r = sock_udp_open_l(lport, host, rport, idle); io_unlock(); return r; }
int sock_write(int h, const void *d, int len, int (*idle)(void)) { io_lock(); int r = sock_write_l(h, d, len, idle); io_unlock(); return r; }
int sock_sendto(int h, const char *host, int port, const void *d, int len, int (*idle)(void)) { io_lock(); int r = sock_sendto_l(h, host, port, d, len, idle); io_unlock(); return r; }
int sock_avail(int h) { io_lock(); int r = sock_avail_l(h); io_unlock(); return r; }
int sock_read(int h, void *buf, int max) { io_lock(); int r = sock_read_l(h, buf, max); io_unlock(); return r; }
int sock_read_from(int h, void *buf, int max, char *ip, int *port) { io_lock(); int r = sock_read_from_l(h, buf, max, ip, port); io_unlock(); return r; }
int sock_getc(int h) { io_lock(); int r = sock_getc_l(h); io_unlock(); return r; }
void sock_close(int h) { io_lock(); sock_close_l(h); io_unlock(); }
void sock_close_all(void) { io_lock(); sock_close_all_l(); io_unlock(); }
const char *ip_address(void) { io_lock(); const char *r = ip_address_l(); io_unlock(); return r; }
void ip_cmd(int argc, char **argv) { io_lock(); ip_cmd_l(argc, argv); io_unlock(); }
void ping_cmd(int argc, char **argv) { io_lock(); ping_cmd_l(argc, argv); io_unlock(); }
void http_cmd(int argc, char **argv) { io_lock(); http_cmd_l(argc, argv); io_unlock(); }
