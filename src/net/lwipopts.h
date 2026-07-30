#ifndef HBOS_LWIPOPTS_H
#define HBOS_LWIPOPTS_H

/*
 * HBOS runs lwIP without an RTOS and uses it only for IPv6.  The existing
 * small IPv4 stack remains available while HPT and verified HTTPS migrate to
 * the dual-stack transport.
 */
#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0
#define LWIP_TIMERS                     1
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_NETIF_API                  0

#define LWIP_IPV4                       0
#define LWIP_IPV6                       1
#define LWIP_IPV6_AUTOCONFIG            1
#define LWIP_IPV6_SEND_ROUTER_SOLICIT   1
#define LWIP_IPV6_MLD                   1
#define LWIP_ND6_QUEUEING               1
#define LWIP_ND6_NUM_NEIGHBORS          8
#define LWIP_ND6_NUM_DESTINATIONS       8
#define LWIP_ND6_NUM_PREFIXES           4
#define LWIP_ND6_NUM_ROUTERS            4
#define LWIP_IPV6_NUM_ADDRESSES         4

#define LWIP_ETHERNET                   1
#define LWIP_ARP                        0
#define LWIP_ICMP                       0
#define LWIP_ICMP6                      1
#define LWIP_IGMP                       0
#define LWIP_RAW                        1
#define LWIP_UDP                        0
#define LWIP_DHCP                       0
#define LWIP_DHCP6                      0
#define LWIP_DNS                        0

#define LWIP_TCP                        1
#define LWIP_CALLBACK_API               1
#define LWIP_EVENT_API                  0
#define TCP_MSS                         1440
#define TCP_WND                         (8 * TCP_MSS)
#define TCP_SND_BUF                     (8 * TCP_MSS)
#define TCP_SND_QUEUELEN                32
#define TCP_QUEUE_OOSEQ                 1
#define LWIP_TCP_KEEPALIVE              1

#define MEM_ALIGNMENT                   8
#define MEM_SIZE                        (256 * 1024)
#define MEMP_NUM_PBUF                   32
#define MEMP_NUM_TCP_PCB                4
#define MEMP_NUM_TCP_SEG                48
#define PBUF_POOL_SIZE                  48
#define PBUF_POOL_BUFSIZE               1536

#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_SINGLE_NETIF               1
#define LWIP_STATS                      0
#define LWIP_DEBUG                      0
#define LWIP_CHECKSUM_CTRL_PER_NETIF    0

#endif
