/*
 * The network device under lwIP and the RoboOS messages: Ethernet when its
 * cable is in (link up), else WiFi when it is connected. One IP interface; when
 * the device changes, ip.c takes the new MAC address and asks DHCP again.
 */
#include "kernel.h"
#include "usb.h"

static int eth_ok(void) { return eth_present() && !eth_error(0, 0); }

/* 0.140: `net ip wifi` = TCP/IP stays on WiFi while it is joined, even with a cable in (the cable then only
 * carries RoboOS messages, e.g. straight to another board); /LINK.TXT "ip wifi" */
static int ip_on_wifi;
void link_set_ip_wifi(int on) { ip_on_wifi = on; }
int  link_ip_wifi(void) { return ip_on_wifi; }
int  link_eth_up(void) { return eth_ok() && eth_link_cached() > 0; }

/* 1 = WiFi carries the traffic */
int link_wifi(void)
{
    if (ip_on_wifi && wifi_joined()) return 1;
    if (eth_ok() && eth_link_cached() > 0) return 0;
    if (wifi_joined()) return 1;
    return !eth_ok() && wifi_up();
}

int link_present(void) { return eth_ok() || wifi_up(); }
int link_up(void) { return link_wifi() ? wifi_joined() : (eth_ok() && eth_link_cached() > 0); }
const u8 *link_mac(void) { return link_wifi() ? wifi_mac() : eth_mac(); }
const char *link_name(void) { return link_wifi() ? "WiFi" : "Ethernet"; }

int link_send(const void *f, int len) { return link_wifi() ? wifi_send(f, len) : eth_send(f, len); }

int link_recv(void *buf, int max)
{
    if (link_wifi()) return wifi_recv(buf, max, 1);
    if (wifi_up()) wifi_recv(buf, max, 0);                /* only its events (scan, join) */
    return eth_ok() ? eth_recv(buf, max) : 0;
}

/* 0.140: a frame from either device; *src = 0 Ethernet, 1 WiFi (net.c: RoboOS messages from both, TCP/IP only
 * from the link lwIP uses). Both are polled in turn so one busy link does not starve the other. */
int link_recv2(void *buf, int max, int *src)
{
    static int turn;
    int w = link_wifi();
    turn ^= 1;
    for (int k = 0; k < 2; k++) {
        int dev = (turn + k) & 1;
        if (dev == 1 && wifi_up()) {
            int n = wifi_recv(buf, max, w || wifi_joined());
            if (n > 0) { *src = 1; return n; }
        }
        if (dev == 0 && eth_ok() && (!w || eth_link_cached() > 0)) {
            int n = eth_recv(buf, max);
            if (n > 0) { *src = 0; return n; }
        }
    }
    return 0;
}
