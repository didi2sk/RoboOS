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
 * Commands: ip, ping, http (a small web page served by the board).
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
#include "netif/ethernet.h"

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

static err_t link_out(struct netif *n, struct pbuf *p)
{
    static u8 f[1536];
    (void)n;
    if (p->tot_len > 1514) return ERR_IF;
    pbuf_copy_partial(p, f, p->tot_len, 0);
    int len = p->tot_len;
    if (len < 60) { memset(f + len, 0, 60 - len); len = 60; }   /* minimum frame */
    return eth_send(f, len) < 0 ? ERR_IF : ERR_OK;
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

static void start(void)
{
    ip4_addr_t a, m, g, d;
    ip4_addr_set_zero(&a); ip4_addr_set_zero(&m); ip4_addr_set_zero(&g); ip4_addr_set_zero(&d);
    if (!lwip_ready) { lwip_init(); lwip_ready = 1; }
    load_config(&a, &m, &g, &d);
    netif_add(&nif, &a, &m, &g, 0, nif_init, ethernet_input);
    netif_set_default(&nif);
    netif_set_up(&nif);
    if (!use_dhcp && !ip4_addr_isany_val(d)) {
        ip_addr_t dd;
        ip_addr_copy_from_ip4(dd, d);
        dns_setserver(0, &dd);
    }
    if (use_dhcp) dhcp_start(&nif);
    started = 1;
    ip4_addr_set_zero(&shown_ip);
}

int ip_started(void) { return started; }

/* a frame from the wire that is not a RoboOS message */
void ipstack_input(const u8 *f, int n)
{
    if (!started || n < 14) return;
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
    u64 end = timer_us() + (u64)ms * 1000;
    while (timer_us() < end)
        if (con_trygetc() == 3) return 1;               /* con_trygetc() also polls the network */
    return 0;
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

void ip_cmd(int argc, char **argv)
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
}

/* ---------------- command: ping ---------------- */

#define PING_ID 0x524F                                  /* "RO" */
static u16 ping_seq;
static int ping_got, ping_ttl;

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

static void dns_found(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)name;
    int *state = arg;
    if (addr) { ip_addr_copy(*(ip_addr_t *)(state + 2), *addr); *state = 1; }
    else *state = -1;
}

/* name or dotted address -> address; 0 = ok */
static int resolve(const char *name, ip_addr_t *out)
{
    static int st[8];                                    /* [0] state, [2..] address */
    if (ipaddr_aton(name, out)) return 0;
    st[0] = 0;
    err_t e = dns_gethostbyname(name, out, dns_found, st);
    if (e == ERR_OK) return 0;
    if (e != ERR_INPROGRESS) return -1;
    for (int i = 0; i < 50 && !st[0]; i++)
        if (run_ms(100)) return -2;
    if (st[0] != 1) return -1;
    ip_addr_copy(*out, *(ip_addr_t *)(st + 2));
    return 0;
}

void ping_cmd(int argc, char **argv)
{
    static struct raw_pcb *pcb;
    ip_addr_t dst;
    u64 count = 4;
    if (argc < 2) { kprintf("usage: ping <address or name> [count]\n"); return; }
    if (!need_net()) return;
    if (argc > 2) parse_num(argv[2], &count);
    int r = resolve(argv[1], &dst);
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
        int stop = 0;
        while (!ping_got && timer_us() - t0 < 1000000)
            if (con_trygetc() == 3) { stop = 1; break; }
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

/* ---------------- command: http (one small page) ---------------- */

static struct tcp_pcb *http_pcb;
static u32 http_hits;
static char page[2048];
static int plen;

static void add(const char *s) { while (*s && plen < (int)sizeof(page) - 1) page[plen++] = *s++; page[plen] = 0; }
static void add_u(u64 v)
{
    char t[24]; int k = 0;
    do { t[k++] = '0' + v % 10; v /= 10; } while (v);
    while (k && plen < (int)sizeof(page) - 1) page[plen++] = t[--k];
    page[plen] = 0;
}

static void build_page(void)
{
    char b[16];
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
    add("<tr><td>address</td><td>"); add(ip4addr_ntoa_r(netif_ip4_addr(&nif), b, sizeof(b))); add("</td></tr>");
    add("<tr><td>uptime</td><td>"); add_u(s / 3600); add(" h "); add_u(s / 60 % 60); add(" min "); add_u(s % 60); add(" s</td></tr>");
    if (tc >= 0) { add("<tr><td>CPU</td><td>"); add_u((u64)tc / 1000); add("."); add_u((u64)tc % 1000 / 100); add(" C</td></tr>"); }
    add("<tr><td>visits</td><td>"); add_u(http_hits); add("</td></tr></table></body></html>\n");
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)err;
    if (!p) { tcp_close(pcb); return ERR_OK; }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (arg) return ERR_OK;                              /* already answered */
    tcp_arg(pcb, (void *)1);
    http_hits++;
    build_page();
    tcp_write(pcb, page, (u16_t)plen, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    tcp_recv(pcb, 0);
    tcp_close(pcb);                                      /* FIN after the queued data */
    return ERR_OK;
}

static err_t http_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    tcp_arg(pcb, 0);
    tcp_recv(pcb, http_recv);
    return ERR_OK;
}

void http_cmd(int argc, char **argv)
{
    char b[16];
    if (!need_net()) return;
    if (argc > 1 && !strcmp(argv[1], "on") && !http_pcb) {
        struct tcp_pcb *p = tcp_new();
        if (!p || tcp_bind(p, IP_ADDR_ANY, 80) != ERR_OK) { kprintf("cannot open port 80\n"); if (p) tcp_close(p); return; }
        http_pcb = tcp_listen(p);
        tcp_accept(http_pcb, http_accept);
    } else if (argc > 1 && !strcmp(argv[1], "off") && http_pcb) {
        tcp_close(http_pcb);
        http_pcb = 0;
    } else if (argc > 1 && strcmp(argv[1], "on") && strcmp(argv[1], "off")) {
        kprintf("usage: http [on|off]\n");
        return;
    }
    if (http_pcb) kprintf("web page on: http://%s/  (%u visits)\n", ip4addr_ntoa_r(netif_ip4_addr(&nif), b, sizeof(b)), http_hits);
    else          kprintf("web page off ('http on' starts it)\n");
}
