/*
 * The network device under lwIP and the RoboOS messages: Ethernet when its
 * cable is in (link up), else WiFi when it is connected. One IP interface; when
 * the device changes, ip.c takes the new MAC address and asks DHCP again.
 */
#include "kernel.h"
#include "usb.h"

static int eth_ok(void) { return eth_present() && !eth_error(0, 0); }

/* 1 = WiFi carries the traffic */
int link_wifi(void)
{
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
