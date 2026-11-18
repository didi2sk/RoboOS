/* lwIP configuration for RoboOS: no operating system (raw API, polled from the
 * console loop), IPv4 only, one network interface (the onboard Ethernet). */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_TIMERS                 1
#define LWIP_SINGLE_NETIF           1

/* memory: everything static, in the kernel BSS */
#define MEM_ALIGNMENT               8
#define MEM_SIZE                    (96 * 1024)
#define MEMP_NUM_PBUF               32
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_TCP_PCB            24   /* browsers keep several idle connections open */
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_TCP_SEG            48
#define MEMP_NUM_SYS_TIMEOUT        12
#define PBUF_POOL_SIZE              32

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define ETH_PAD_SIZE                0
#define ARP_TABLE_SIZE              16
#define ARP_QUEUEING                1
#define IP_REASSEMBLY               1
#define IP_FRAG                     1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)
#define LWIP_DHCP                   1
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_ACD                    0
#define LWIP_AUTOIP                 0
#define LWIP_DNS                    1
#define LWIP_IGMP                   0
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

#endif
